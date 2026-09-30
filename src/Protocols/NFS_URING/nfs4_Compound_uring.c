/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (C) 2026, IBM. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file  nfs4_Compound_uring.c
 * @brief Segmented sub-batch execution engine for NFSv4 COMPOUND.
 *
 * See nfs4_uring_internal.h for the overall shape. This file owns the
 * segmentation loop, chain planning and submission, and the resume handler
 * that turns reaped CQEs back into XDR responses.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_config.h" /* fsal_supports() */
#include "FSAL/fsal_uring.h" /* LogUring / COMPONENT_URING */
#include "FSAL_URING/uring_pers.h"
#include "gsh_config.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_core.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"

#include "nfs4_uring_internal.h"

bool nfs4_uring_compound_enabled(void)
{
	return nfs_param.core_param.enable_uring_compound;
}

nfsstat4 nfs4_uring_cqe_to_nfsstat(int32_t res)
{
	if (res >= 0)
		return NFS4_OK;

	/*
	 * Every SQE linked behind one that failed is completed with
	 * -ECANCELED. The operation was never attempted, so reporting the
	 * literal errno would be misleading; RFC 8881 lets us return a
	 * server-side I/O failure for it. The operation that actually failed
	 * reports its own error, and because the compound short-circuits on
	 * the first error the client sees that one.
	 */
	if (res == -ECANCELED)
		return NFS4ERR_IO;

	return nfs4_Errno_status(posix2fsal_status(-res));
}

/**
 * @brief Opcode for the operation at @pos, remapped to OP_ILLEGAL if it is
 *        out of range for the minor version, exactly as process_one_op does.
 */
static inline nfs_opnum4 uring_opcode_at(compound_data_t *data,
					 unsigned int pos)
{
	nfs_opnum4 opcode = data->argarray[pos].argop;

	if (opcode > LastOpcode[data->minorversion])
		opcode = 0;

	return opcode;
}

/**
 * @brief Bottom-most storage export under the current (possibly stacked) one.
 *
 * data->current_obj normally lives in MDCACHE, which stacks on the real
 * storage FSAL. Batchability and the SQE layout are properties of the
 * storage FSAL, so the whole stack has to be walked down.
 */
static struct fsal_export *nfs4_uring_storage_export(void)
{
	struct fsal_export *exp;

	if (op_ctx == NULL)
		return NULL;

	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	return exp;
}

bool nfs4_uring_op_batchable(compound_data_t *data, nfs_opnum4 opcode)
{
	const struct nfs4_uring_op_desc *desc = &optabv4_uring[opcode];
	struct fsal_export *exp;

	/*
	 * Every refusal below is logged. "Why is nothing batching?" is the
	 * first question anyone asks of this feature, and all four reasons
	 * look identical from the outside: the compound simply produces a
	 * correct answer the slow way. Set COMPONENT_URING to FULL_DEBUG to
	 * see which one fired.
	 */

	/* Category A or C: no SQE form at all. */
	if (desc->prep_sqe == NULL) {
		LogUring(NIV_FULL_DEBUG,
			 "op %s not batchable: no prep_sqe (category A/C)",
			 desc->name != NULL ? desc->name : "?");
		return false;
	}

	/*
	 * Before PUTFH there is no export and therefore no FSAL to ask, so
	 * nothing batches. This is why the uring/legacy decision cannot be
	 * made once at the top of nfs4_Compound(): for NFSv4 the request
	 * context starts with a NULL export.
	 */
	exp = nfs4_uring_storage_export();

	if (exp == NULL || exp->fsal == NULL) {
		LogUring(NIV_FULL_DEBUG,
			 "op %s not batchable: no storage export yet (before PUTFH?)",
			 desc->name);
		return false;
	}

	if (!fsal_supports(&exp->fsal->fs_info, fso_uring_batch)) {
		LogUring(NIV_FULL_DEBUG,
			 "op %s not batchable: FSAL %s does not report fso_uring_batch",
			 desc->name, exp->fsal->name);
		return false;
	}

	/* Asks the FSAL's vector, which also consults the kernel opcode
	 * probe and the FSAL's own override.
	 */
	if (!fsal_uring_fop_can_batch(exp, desc->fop)) {
		LogUring(NIV_FULL_DEBUG,
			 "op %s not batchable: FSAL %s has no usable fop %d (vector, kernel probe or FSAL override refused)",
			 desc->name, exp->fsal->name, (int)desc->fop);
		return false;
	}

	return true;
}

/**
 * @brief Try to finish this batch on the submitting thread.
 *
 * The kernel satisfies most of what this engine submits inline, during
 * io_uring_enter() itself -- a page cache read never blocks, so io_read()
 * runs in the submitting task and posts its CQE before submit returns.
 * Suspending the request at that point, waking a reaper to notice, and
 * waking a second worker to encode the reply costs two context switches to
 * deliver an answer this thread already has.
 *
 * So: poll the ring for our own completion first, and only park if it
 * really has not arrived. @a Uring_Inline_Poll bounds how many rounds to
 * try; 0 disables this entirely and restores the suspend-always behaviour.
 *
 * Racing the reaper is the whole difficulty, and
 * enum nfs4_uring_batch_state is how it is settled: whoever wins the CAS
 * out of INFLIGHT owns the batch, and the loser does nothing.
 *
 * @return true  the batch is complete and this thread must finish it;
 *         false this thread has parked and the completion will resume it.
 */
static bool nfs4_uring_claim_inline(struct nfs4_uring_batch *batch)
{
	unsigned int rounds = nfs_param.core_param.uring_inline_poll;
	uint32_t expected = NFS4_URING_BATCH_INFLIGHT;

	for (;;) {
		if (__atomic_load_n(&batch->state, __ATOMIC_ACQUIRE) ==
		    NFS4_URING_BATCH_DONE)
			return true;

		if (rounds == 0)
			break;

		rounds--;

		/*
		 * Harvest the ring. This runs the completion callbacks for
		 * whatever is ready, including ours -- which is what sets
		 * our state to DONE -- and including other requests', whose
		 * resumes are batched into one dispatch by the bracket
		 * (when libntirpc provides the batch API).
		 */
#ifdef HAVE_SVC_RESUME_BATCH
		svc_resume_defer_begin();
#endif
		(void)fsal_uring_poll(batch->engine, batch->chain.ring_idx);
#ifdef HAVE_SVC_RESUME_BATCH
		svc_resume_defer_end();
#endif
	}

	if (__atomic_compare_exchange_n(&batch->state, &expected,
					NFS4_URING_BATCH_SUSPENDED, false,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		/* Parked. nfs4_uring_chain_done() will svc_resume() us. */
		return false;
	}

	/* The completion landed between the last load and the CAS. */
	return true;
}

/**
 * @brief Length of the run of batchable operations starting at @start.
 *
 * Returns 0 when the operation at @start is not batchable, which is the
 * signal to run one operation synchronously instead.
 *
 * Only operations that leave data->current_obj alone may be batched, because
 * the whole run is planned against the current filehandle before any of it
 * executes. Every category B operation satisfies that; the ones that do not
 * (OPEN, LOOKUP, PUTFH, ...) have no prep_sqe and so terminate the run.
 */
static unsigned int count_batchable_ops(compound_data_t *data,
					unsigned int start)
{
	unsigned int len = 0;
	unsigned int slots = 0;
	unsigned int i;

	for (i = start; i < data->argarray_len; i++) {
		nfs_opnum4 opcode = uring_opcode_at(data, i);
		const struct nfs4_uring_op_desc *desc = &optabv4_uring[opcode];

		if (!nfs4_uring_op_batchable(data, opcode))
			break;

		if (len == URING_CHAIN_MAX_OPS)
			break;

		/* A stable WRITE needs two slots (WRITEV plus the linked
		 * sync), so reserve the worst case before accepting it.
		 */
		if (slots + desc->max_slots > URING_CHAIN_MAX_OPS)
			break;

		slots += desc->max_slots;
		len++;
	}

	return len;
}

/** Does this operation change file data or metadata? */
static inline bool uring_op_is_mutating(nfs_opnum4 opcode)
{
	return opcode == NFS4_OP_WRITE || opcode == NFS4_OP_SETATTR ||
	       opcode == NFS4_OP_ALLOCATE || opcode == NFS4_OP_DEALLOCATE;
}

/**
 * @brief Release everything operation @idx acquired during planning.
 *
 * Safe to call on an operation that was served inline (it owns nothing) and
 * safe to call twice, because the handlers' abort_prep are idempotent.
 */
static void uring_abort_one(struct nfs4_uring_batch *batch, unsigned int idx)
{
	struct nfs4_uring_op_ctx *uctx = &batch->op[idx];
	nfs_opnum4 opcode;
	const struct nfs4_uring_op_desc *desc;

	/* Operations served inline never acquired anything. */
	if (uctx->inline_done || uctx->nr_slots == 0)
		return;

	opcode = uring_opcode_at(batch->data, uctx->oppos);
	desc = &optabv4_uring[opcode];

	if (desc->abort_prep != NULL)
		desc->abort_prep(uctx);

	fsal_uring_fop_release_creds(&uctx->req);

	/* Mark it as owning nothing so a second pass skips it. */
	uctx->nr_slots = 0;
}

/** Release prep state for operations [0, planned) that will not be sent. */
static void uring_abort_planned(struct nfs4_uring_batch *batch,
				unsigned int planned)
{
	unsigned int i;

	for (i = 0; i < planned; i++)
		uring_abort_one(batch, i);
}

void nfs4_uring_batch_free(compound_data_t *data)
{
	struct nfs4_uring_batch *batch = data->uring_batch;

	if (batch == NULL)
		return;

	/* Nothing may be in flight here; the compound only completes once
	 * every chain has been reaped.
	 */
	uring_abort_planned(batch, batch->nr_ops);

	gsh_free(batch, MEM_COMP_PROTOCOL);
	data->uring_batch = NULL;
}

/**
 * @brief Reaper-thread completion for a chain.
 *
 * Runs with no op_ctx and no FSAL thread state, so it does the minimum
 * possible: hand the request back to a worker. Every finish_cqe runs later,
 * on that worker, which is what makes it safe for CLOSE to take the SAL
 * state lock.
 */
static void nfs4_uring_chain_done(void *cb_arg, struct uring_chain_req *chain)
{
	struct nfs4_uring_batch *batch = cb_arg;
	uint32_t expected = NFS4_URING_BATCH_INFLIGHT;

	if (__atomic_compare_exchange_n(&batch->state, &expected,
					NFS4_URING_BATCH_DONE, false,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		/*
		 * The submitter has not parked yet, so it is still polling
		 * and will see DONE and finish the batch itself. Resuming
		 * here as well would run the compound twice.
		 *
		 * Note this branch is also taken when the submitter is the
		 * one running this callback, out of its own poll -- which
		 * is the common case for an I/O the kernel completed inline
		 * during io_uring_enter().
		 */
		return;
	}

	/* expected is now SUSPENDED: the submitter gave up and parked, so
	 * the request has to be handed back to a worker the usual way.
	 */
	svc_resume(batch->data->req);
}

/* Forward declaration: the planner installs this as the resume callback. */
static enum xprt_stat nfs4_compound_uring_resume(struct svc_req *req);

/* Forward declaration: shared by the inline finish in plan_and_submit_batch()
 * and by the resume, and defined next to the latter.
 */
static enum nfs_req_result
nfs4_uring_finish_batch(compound_data_t *data, nfsstat4 *status,
			unsigned int *next_oppos);

/**
 * @brief Run operations [from, to) through the legacy sequential handler.
 *
 * Leaves data->oppos on the first operation it did not complete.
 */
static enum nfs_req_result uring_exec_sync_range(compound_data_t *data,
						 unsigned int from,
						 unsigned int to,
						 nfsstat4 *status)
{
	enum nfs_req_result result = NFS_REQ_OK;

	for (data->oppos = from; result == NFS_REQ_OK && data->oppos < to;
	     data->oppos++) {
		result = process_one_op(data, status);

		if (result == NFS_REQ_ASYNC_WAIT)
			return result;
	}

	return result;
}

/**
 * @brief Re-run a batch synchronously after the chain could not be submitted.
 *
 * Only operations that were going to use the ring are re-run. Operations
 * that prep_sqe already served inline (an MDCACHE attribute hit) have had
 * their response written and complete_op() called, and must not be executed
 * a second time; those are the ones with nr_slots == 0.
 *
 * Leaves data->oppos on the first operation it did not complete.
 */
static enum nfs_req_result uring_exec_sync_fallback(compound_data_t *data,
						    struct nfs4_uring_batch
							    *batch,
						    unsigned int planned,
						    nfsstat4 *status)
{
	enum nfs_req_result result = NFS_REQ_OK;
	unsigned int i;

	for (i = 0; i < planned; i++) {
		if (batch->op[i].inline_done) {
			/* Already completed during planning; running it a
			 * second time would duplicate its response.
			 */
			continue;
		}

		data->oppos = batch->op[i].oppos;

		result = process_one_op(data, status);

		if (result == NFS_REQ_ASYNC_WAIT)
			return result;

		if (result != NFS_REQ_OK) {
			data->oppos = batch->op[i].oppos + 1;
			return result;
		}
	}

	data->oppos = batch->start_op + planned;

	return NFS_REQ_OK;
}

/**
 * @brief Plan a run of batchable operations and submit it as one chain.
 *
 * @retval NFS_REQ_ASYNC_WAIT  chain in flight, caller must return at once
 * @retval NFS_REQ_OK          the run completed without needing the ring
 * @retval NFS_REQ_ERROR       an operation failed during planning
 */
static enum nfs_req_result plan_and_submit_batch(compound_data_t *data,
						 unsigned int start,
						 unsigned int len,
						 nfsstat4 *status)
{
	struct nfs4_uring_batch *batch = data->uring_batch;
	struct fsal_export *exp = nfs4_uring_storage_export();
	/* Next free chain slot. Not the same as the operation index: a
	 * stable WRITE owns two slots.
	 */
	unsigned int slot = 0;
	/* Operations planned, including any served inline. */
	unsigned int planned = 0;
	unsigned int i;
	int rc;

	/* count_batchable_ops() returned non-zero, so the export and its
	 * vector must exist; check anyway rather than fault on a NULL.
	 */
	if (exp == NULL || exp->uring_fops == NULL ||
	    exp->uring_fops->engine == NULL)
		return uring_exec_sync_range(data, start, start + len, status);

	if (batch == NULL) {
		batch = gsh_calloc(1, sizeof(*batch), MEM_COMP_PROTOCOL);
		data->uring_batch = batch;
	}

	batch->data = data;
	batch->start_op = start;
	batch->nr_ops = 0;
	batch->engine = exp->uring_fops->engine;
	batch->chain.nr_done = 0;
	batch->state = NFS4_URING_BATCH_INFLIGHT;
	batch->chain.done_cb = nfs4_uring_chain_done;
	batch->chain.cb_arg = batch;

	data->has_mutating_op = false;

	for (i = 0; i < len; i++) {
		struct nfs4_uring_op_ctx *uctx = &batch->op[i];
		const struct nfs4_uring_op_desc *desc;
		enum nfs4_uring_prep_result prep;
		enum nfs_req_result r;
		nfs_opnum4 opcode;

		memset(uctx, 0, sizeof(*uctx));
		uctx->batch = batch;
		uctx->op_idx = i;
		uctx->oppos = start + i;
		uctx->slot = slot;
		uctx->nr_slots = 1;

		/*
		 * -1, not the 0 the memset leaves behind. The FSAL preps
		 * guard with "fd < 0", so a zeroed descriptor would sail
		 * through and the SQE would be built against fd 0 — the
		 * daemon's own stdin. A close(0) or readv(0) there is silent
		 * and catastrophic, so the invalid value has to be the
		 * default state rather than something prep remembers to set.
		 */
		uctx->req.fd = -1;

		data->oppos = start + i;

		/* Identical position, export-permission and response-room
		 * checks the sequential path applies. Sets data->opcode.
		 */
		r = nfs4_op_precheck(data, status);

		if (r != NFS_REQ_OK) {
			batch->nr_ops = planned;
			uring_abort_planned(batch, planned);
			batch->nr_ops = 0;
			return r;
		}

		opcode = data->opcode;
		desc = &optabv4_uring[opcode];

		uctx->req.fop = desc->fop;

		prep = desc->prep_sqe(&data->argarray[uctx->oppos], data,
				      uctx);

		if (prep == NFS4_URING_PREP_QUEUED) {
			fsal_status_t cst;

			/*
			 * Credentials are resolved here, after prep, because
			 * only prep knows how many SQEs the operation emits
			 * and credentials are a per-SQE property. The policy
			 * itself belongs to the FSAL's fop descriptor, not to
			 * this table: whether an identity is needed depends on
			 * how the storage layer expresses the operation.
			 */
			cst = fsal_uring_fop_resolve_creds(exp, &uctx->req);

			if (FSAL_IS_ERROR(cst)) {
				LogUring(NIV_DEBUG,
					 "op %s: credential resolution failed (%s), not batching",
					 desc->name, fsal_err_txt(cst));

				if (desc->abort_prep != NULL)
					desc->abort_prep(uctx);

				prep = NFS4_URING_PREP_FALLBACK;
			}
		}

		if (prep == NFS4_URING_PREP_FALLBACK) {
			/* Cannot batch after all. Close the chain before this
			 * operation; the segmentation loop will run it
			 * synchronously on the next pass.
			 */
			fsal_uring_fop_release_creds(&uctx->req);
			break;
		}

		if (prep == NFS4_URING_PREP_ERROR) {
			/*
			 * Close the chain before this operation and let the
			 * synchronous handler produce the error on the next
			 * pass, exactly as for a fallback.
			 *
			 * Reporting the error from here instead would be
			 * wrong: operations already planned into this chain
			 * have NOT run (nothing has been submitted yet), but
			 * complete_op() truncates the compound *after* them,
			 * so they would be encoded as successful results for
			 * work that never happened. Letting them execute
			 * first, and re-deriving this error sequentially,
			 * keeps the reply honest and costs only a repeated
			 * validation on a path that is already failing.
			 */
			fsal_uring_fop_release_creds(&uctx->req);
			break;
		}

		if (prep == NFS4_URING_PREP_INLINE) {
			/* Served from cache; no SQE, and finish_cqe will not
			 * run for it.
			 */
			uctx->nr_slots = 0;

			fsal_uring_fop_release_creds(&uctx->req);

			/*
			 * Only complete it here if nothing is queued ahead of
			 * it. Completing out of order would let complete_op()
			 * tally and possibly truncate the compound around
			 * operations earlier in the reply that have not run
			 * yet, encoding results for work that never happened.
			 *
			 * With something already queued, close the chain
			 * instead; this operation is re-planned on the next
			 * pass, where it will be first and can be served
			 * inline cleanly. prep() filling the response twice
			 * is harmless, it is the same cached answer.
			 */
			if (slot > 0)
				break;

			uctx->inline_done = true;

			r = complete_op(data, status, NFS_REQ_OK);
			planned++;

			if (r != NFS_REQ_OK) {
				batch->nr_ops = planned;
				uring_abort_planned(batch, planned);
				batch->nr_ops = 0;
				return r;
			}

			continue;
		}

		/* NFS4_URING_PREP_QUEUED */
		slot += uctx->nr_slots;
		planned++;

		if (uring_op_is_mutating(opcode))
			data->has_mutating_op = true;
	}

	batch->nr_ops = planned;

	if (slot == 0) {
		batch->nr_ops = 0;

		if (planned == 0) {
			/*
			 * The very first operation asked to fall back, so
			 * nothing was planned and data->oppos has not moved.
			 * Returning here would leave the segmentation loop to
			 * re-plan the same operation forever, because
			 * batchability does not change between passes. Run it
			 * synchronously instead, which is what the fallback
			 * was asking for and guarantees forward progress.
			 */
			return uring_exec_sync_range(data, start, start + 1,
						     status);
		}

		/* Everything planned was served inline; nothing to submit. */
		data->oppos = start + planned;
		return NFS_REQ_OK;
	}

	/* One SQE per slot, which is what the engine submits. */
	batch->chain.nr_ops = slot;

	/*
	 * Install the uring resume before submitting: once the chain is in
	 * the ring a CQE can land, and the reaper will call svc_resume()
	 * immediately.
	 */
	data->req->rq_resume_cb = nfs4_compound_uring_resume;

	LogUring(NIV_DEBUG,
		 "compound: submitting chain ops[%u..%u] (%u op(s), %u SQE(s))",
		 start, start + planned - 1, planned, slot);

	rc = fsal_uring_submit_chain(batch->engine, &batch->chain);

	if (rc != 0) {
		LogEvent(COMPONENT_NFS_V4,
			 "uring compound: chain submit failed (%s), falling back to sequential execution",
			 strerror(-rc));

		data->req->rq_resume_cb = nfs4_compound_resume;

		uring_abort_planned(batch, planned);

		/* Released above; make sure a later batch_free() does not
		 * walk these operations a second time.
		 */
		batch->nr_ops = 0;

		return uring_exec_sync_fallback(data, batch, planned, status);
	}

	/*
	 * Most of what we submit is already finished by now -- the kernel
	 * runs a cache-hot read inline inside io_uring_enter(). Collect it
	 * here rather than suspending and being handed it back.
	 */
	if (nfs4_uring_claim_inline(batch)) {
		nfsstat4 inline_status = NFS4_OK;
		unsigned int next_oppos;
		enum nfs_req_result result;

		LogUring(NIV_FULL_DEBUG,
			 "compound: chain completed inline, no suspend");

		/* op_ctx was never given up, so unlike the resume path this
		 * can finish the batch directly. On NFS_REQ_OK the helper
		 * leaves data->oppos on the next operation, which is exactly
		 * what our caller's loop wants, so the rest of the compound
		 * carries on as if this run had been synchronous.
		 */
		result = nfs4_uring_finish_batch(data, &inline_status,
						 &next_oppos);

		/* Carry the last operation's status up, the same way the
		 * synchronous runs in this function do.
		 */
		*status = inline_status;

		/* The chain's resume is no longer installed for this batch;
		 * anything that suspends from here finishes the compound
		 * the legacy way until the planner submits again.
		 */
		data->req->rq_resume_cb = nfs4_compound_resume;

		return result;
	}

	suspend_op_context();

	return NFS_REQ_ASYNC_WAIT;
}

enum nfs_req_result nfs4_Compound_uring(compound_data_t *data,
					nfsstat4 *status)
{
	enum nfs_req_result result = NFS_REQ_OK;

	/*
	 * Steer this request's resumes to the dedicated pool, if there is
	 * one. Set here rather than next to each rq_resume_cb assignment so
	 * that it holds for every way this request can later suspend --
	 * the chain's own resume, the legacy resume reinstalled when the
	 * planner falls back mid-compound, and an FSAL that suspends an
	 * operation the planner ran synchronously. All of them are
	 * completed by an io_uring reaper, and all of them should stay off
	 * svc_work_pool's queue mutex.
	 *
	 * Idempotent: this function is re-entered once per resume.
	 */
#ifdef HAVE_SVC_RESUME_BATCH
	data->req->rq_resume_pool = nfs4_uring_resume_pool();
#endif

	while (result == NFS_REQ_OK && data->oppos < data->argarray_len) {
		unsigned int start = data->oppos;
		unsigned int len = count_batchable_ops(data, start);

		if (len > 0) {
			LogUring(NIV_FULL_DEBUG,
				 "compound: batchable run of %u op(s) at position %u of %" PRIu32,
				 len, start, data->argarray_len);

			result = plan_and_submit_batch(data, start, len,
						       status);

			if (result == NFS_REQ_ASYNC_WAIT)
				return result;

			/* plan_and_submit_batch left data->oppos on the first
			 * operation it did not complete.
			 */
			continue;
		}

		/* Category A or C: run it through the untouched handler. */
		result = process_one_op(data, status);

		if (result == NFS_REQ_ASYNC_WAIT) {
			/*
			 * A synchronous operation suspended on its own (an
			 * OPEN that had to go to the FSAL, say). The legacy
			 * resume callback is still installed, so the rest of
			 * the compound finishes sequentially. Correct, just
			 * not batched.
			 */
			return result;
		}

		data->oppos++;
	}

	return result;
}

/**
 * @brief Resume a compound whose chain has completed.
 *
 * Runs on a worker thread, so finish_cqe handlers may take locks and call
 * into the FSAL.
 */
/**
 * @brief Turn a completed chain's CQEs into responses.
 *
 * Shared by the two ways a batch can be finished: the submitting worker
 * that polled its own completion out of the ring, and the resume that runs
 * when the reaper handed the request back instead. The only difference
 * between them is op_ctx -- the submitter never let go of it, the resume
 * has to restore it first -- so that part stays with the callers.
 *
 * Leaves data->oppos on the first operation after the sub-batch when it
 * returns NFS_REQ_OK, and resets the batch either way.
 *
 * @param[in,out] data       Compound being run
 * @param[out]    status     Status of the last operation processed
 * @param[out]    next_oppos First argarray index after this sub-batch
 *
 * @return Result of the last operation processed
 */
static enum nfs_req_result
nfs4_uring_finish_batch(compound_data_t *data, nfsstat4 *status,
			unsigned int *next_oppos)
{
	struct nfs4_uring_batch *batch = data->uring_batch;
	enum nfs_req_result result = NFS_REQ_OK;
	unsigned int i;

	/*
	 * Hand each operation its own CQEs. Operations served inline during
	 * planning were completed then and have nr_slots == 0.
	 */
	for (i = 0; i < batch->nr_ops; i++) {
		struct nfs4_uring_op_ctx *uctx = &batch->op[i];
		const struct nfs4_uring_op_desc *desc;
		nfs_opnum4 opcode;

		if (uctx->nr_slots == 0)
			continue;

		data->oppos = uctx->oppos;
		opcode = uring_opcode_at(data, uctx->oppos);
		desc = &optabv4_uring[opcode];

		data->opcode = opcode;
		data->opname = desc->name;

		/* The FSAL's own completion runs first and turns the raw CQE
		 * into an fsal_status_t, then the protocol handler turns that
		 * into the XDR response.
		 */
		uctx->req.res = batch->chain.cqes[uctx->slot].res;

		LogUring(NIV_FULL_DEBUG,
			 "compound: finishing %s at position %u, slot %u, cqe res=%" PRId32,
			 desc->name, uctx->oppos, uctx->slot, uctx->req.res);

		result = desc->finish_cqe(&data->argarray[uctx->oppos], data,
					  &data->resarray[uctx->oppos], uctx);

		fsal_uring_fop_release_creds(&uctx->req);

		result = complete_op(data, status, result);

		if (result != NFS_REQ_OK) {
			/* This one is finished; the rest are not. */
			i++;
			break;
		}
	}

	/*
	 * Operations after a failure never had finish_cqe called, so they
	 * still hold a descriptor, a buffer and state references. Zeroing
	 * nr_ops below would also hide them from nfs4_uring_batch_free(),
	 * so they have to be released here. A leaked fsal_fd work reference
	 * is especially bad: it blocks close_fsal_fd() on that handle for
	 * the lifetime of the server.
	 *
	 * Their CQEs have already been reaped, so the kernel is done with
	 * them and this is purely a teardown.
	 */
	for (; i < batch->nr_ops; i++)
		uring_abort_one(batch, i);

	/* First operation after the sub-batch. Must be computed before the
	 * batch is reset.
	 */
	*next_oppos = batch->start_op + batch->nr_ops;

	/* The batch is spent. */
	batch->nr_ops = 0;

	if (result == NFS_REQ_OK)
		data->oppos = *next_oppos;

	return result;
}

static enum xprt_stat nfs4_compound_uring_resume(struct svc_req *req)
{
	nfs_request_t *reqdata = container_of(req, nfs_request_t, svc);
	compound_data_t *data = reqdata->proc_data;
	enum nfs_req_result result;
	nfsstat4 status = NFS4_OK;
	unsigned int next_oppos;

	/* The submitter parked rather than polling its own completion, so
	 * its op_ctx has to be put back before anything else runs.
	 */
	resume_op_context(&reqdata->op_context);

	result = nfs4_uring_finish_batch(data, &status, &next_oppos);

	if (result == NFS_REQ_OK) {
		/* A synchronous operation that suspends from here must come
		 * back through the legacy resume, which finishes the rest of
		 * the compound sequentially. The planner reinstalls the uring
		 * resume before it submits the next chain.
		 */
		req->rq_resume_cb = nfs4_compound_resume;

		result = nfs4_Compound_uring(data, &status);

		if (result == NFS_REQ_ASYNC_WAIT) {
			suspend_op_context();
			return XPRT_SUSPEND;
		}
	}

	complete_nfs4_compound(data, status, result);

	compound_data_Free(data);

	if (op_ctx->ctx_export)
		clear_op_context_export();

	nfs_rpc_complete_async_request(reqdata, NFS_REQ_OK);

	return XPRT_IDLE;
}

#endif /* USE_FSAL_URING */
