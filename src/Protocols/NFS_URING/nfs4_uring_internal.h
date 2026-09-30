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
 * @file  nfs4_uring_internal.h
 * @brief optabv4_uring[] and the segmented sub-batch compound engine.
 *
 * The legacy compound engine runs one operation at a time: each I/O suspends
 * the request, a reaper wakes it, and the operations after it fall back to
 * blocking syscalls on the resume worker. This engine instead walks the
 * compound looking for runs of operations the FSAL can express as SQEs, and
 * submits each run as one linked chain in a single io_uring_submit().
 *
 * Operations fall into three categories:
 *
 *   A  Protocol/state, in memory      SEQUENCE, PUTFH, GETFH, SAVEFH, ...
 *   B  Batchable storage I/O          READ, WRITE, COMMIT, GETATTR, ACCESS,
 *                                     CLOSE, ALLOCATE, DEALLOCATE
 *   C  Complex, not batchable         OPEN, LOOKUP, RENAME, LOCK, ...
 *
 * A and C run synchronously through the untouched optabv4[] handler at a
 * segment boundary; a maximal run of B becomes one chain. So
 * {SEQUENCE, PUTFH, OPEN, READ, GETATTR, CLOSE} is executed as
 * [sync: SEQUENCE, PUTFH, OPEN] then [chain: READ -> GETATTR -> CLOSE].
 *
 * LOOKUP is category C even though it looks cheap: obj_ops->lookup() has to
 * create an fsal_obj_handle and may allocate an MDCACHE entry, which needs
 * op_ctx and export references that do not exist on a reaper thread.
 *
 * Nothing here is reachable unless Enable_Uring_Compound is set AND the
 * export's FSAL reports fso_uring_batch, so the legacy path is bit-for-bit
 * unchanged when io_uring is off.
 */

#ifndef NFS4_URING_INTERNAL_H
#define NFS4_URING_INTERNAL_H

#include "config.h"

#ifdef USE_FSAL_URING

#include "fsal.h"
#include "FSAL/fsal_uring_fops.h"
#include "io_uring/core/uring_engine.h"
#include "nfs_proto_data.h"
#include "nfs_proto_functions.h"

/**
 * @brief Outcome of planning one operation into a chain.
 *
 * Distinct from enum nfs_req_result because "I filled the response without
 * needing the kernel" and "I cannot batch this, run the normal handler" are
 * both successes that the planner must treat very differently, and neither
 * maps onto NFS_REQ_OK.
 */
enum nfs4_uring_prep_result {
	/** SQE(s) planned; uctx->nr_slots of them. finish_cqe will run. */
	NFS4_URING_PREP_QUEUED,
	/**
	 * Served in place, no SQE emitted and finish_cqe will NOT run. Used
	 * by GETATTR on an MDCACHE hit, which already has the attributes in
	 * memory and only has to encode them.
	 */
	NFS4_URING_PREP_INLINE,
	/**
	 * Cannot be batched after all; the planner closes the chain before
	 * this operation and lets the synchronous handler take it. Used by
	 * ACCESS on an export with NFSv4 ACLs, which must be evaluated in
	 * memory.
	 */
	NFS4_URING_PREP_FALLBACK,
	/** Validation failed; resp already holds the error status. */
	NFS4_URING_PREP_ERROR
};

struct nfs4_uring_batch;

/**
 * @brief Per-operation planning and completion context.
 *
 * One of these exists for each NFS operation in a sub-batch. It is the only
 * thing passed between prep_sqe and finish_cqe, so anything an operation
 * needs to carry across the async gap belongs in @priv.
 */
struct nfs4_uring_op_ctx {
	/** Batch this operation belongs to. */
	struct nfs4_uring_batch *batch;

	/** Index of this operation within the batch, 0 based. */
	unsigned int op_idx;

	/** Position in data->argarray / data->resarray. */
	unsigned int oppos;

	/** First chain slot this operation owns. */
	unsigned int slot;

	/**
	 * Chain slots this operation owns, set by prep_sqe. Defaults to 1.
	 * A stable WRITE sets 2: the WRITEV plus the SYNC_FILE_RANGE linked
	 * behind it. Its CQEs are chain->cqes[slot .. slot + nr_slots - 1].
	 */
	unsigned int nr_slots;

	/**
	 * prep_sqe served this operation from memory and already completed
	 * its response, so it must never be executed again. Distinct from
	 * nr_slots == 0, which after an abort also means "owns nothing".
	 */
	bool inline_done;

	/** Storage-level request handed to the FSAL's prep/finish pair. */
	struct fsal_uring_fop_req req;

	/**
	 * Operation-private state allocated by prep_sqe, released by
	 * finish_cqe (or by the planner's abort path if the chain is never
	 * submitted). NULL when the operation needs none.
	 */
	void *priv;
};

/**
 * @brief Plan one operation into the chain.
 *
 * Runs on a worker thread with a valid op_ctx, before submission, so it may
 * take locks and touch the MDCACHE.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context; current_obj is the target
 * @param[in,out] uctx  Per-operation context to fill in
 */
typedef enum nfs4_uring_prep_result (*nfs4_uring_prep_fn_t)(
	struct nfs_argop4 *op, compound_data_t *data,
	struct nfs4_uring_op_ctx *uctx);

/**
 * @brief Turn this operation's CQEs into its XDR response.
 *
 * Runs on a resume worker thread, never on the reaper, so it may take the
 * SAL state lock and call back into the FSAL. Reads its results from
 * uctx->batch->chain.cqes[uctx->slot ...].
 *
 * Must release uctx->priv.
 *
 * @retval NFS_REQ_OK    response formatted, compound continues
 * @retval NFS_REQ_ERROR response holds an error, compound short-circuits
 */
typedef enum nfs_req_result (*nfs4_uring_finish_fn_t)(
	struct nfs_argop4 *op, compound_data_t *data, struct nfs_resop4 *resp,
	struct nfs4_uring_op_ctx *uctx);

/**
 * @brief One row of optabv4_uring[].
 *
 * Mirrors struct nfs4_op_desc in nfs4_Compound.c. prep_sqe == NULL marks a
 * category A or C operation, which is what count_batchable_ops() stops on;
 * such a row needs no finish_cqe.
 */
struct nfs4_uring_op_desc {
	const char *name;

	/** Which storage operation this maps onto, for the FSAL vector. */
	enum fsal_uring_fop fop;

	/** NULL for category A/C: run the legacy optabv4[] handler. */
	nfs4_uring_prep_fn_t prep_sqe;

	/** Must be non-NULL whenever prep_sqe is. */
	nfs4_uring_finish_fn_t finish_cqe;

	/**
	 * Release what prep_sqe allocated, for an operation that was planned
	 * but will never be submitted (a later operation in the same batch
	 * failed to plan, or the submission itself failed). finish_cqe does
	 * this on the normal path, so abort_prep exists only to cover the
	 * path where no CQE will ever arrive.
	 *
	 * May be NULL if prep_sqe allocates nothing.
	 */
	void (*abort_prep)(struct nfs4_uring_op_ctx *uctx);

	/** Largest number of chain slots this operation can consume. */
	unsigned int max_slots;
};

/** Indexed by opcode, exactly like optabv4[]. */
extern const struct nfs4_uring_op_desc optabv4_uring[];

/** One in-flight sub-batch. Allocated once per compound and reused. */
/**
 * @brief Who finishes a submitted batch: the submitter, or the completion.
 *
 * The kernel often satisfies a chain inline during io_uring_enter(), so by
 * the time fsal_uring_submit_chain() returns the CQEs are already in the
 * ring. The submitting worker then polls for them rather than suspending,
 * which saves two context switches -- but it is racing the reaper, which
 * may harvest the same CQEs first. Exactly one of the two must finish the
 * batch, and these three states are how they agree.
 *
 * INFLIGHT -> DONE       the completion ran first; it does NOT resume the
 *                        request, because the submitter is still polling
 *                        and will see DONE and finish inline.
 * INFLIGHT -> SUSPENDED  the submitter gave up polling and parked; the
 *                        completion will svc_resume() as it always did.
 *
 * Both transitions are compare-and-swap from INFLIGHT, so whichever gets
 * there first owns the outcome and the loser does nothing. This mirrors
 * the ASYNC_PROC_EXIT / ASYNC_PROC_DONE handshake nfs4_op_read.c uses
 * around fsal_read2().
 */
enum nfs4_uring_batch_state {
	NFS4_URING_BATCH_INFLIGHT = 0,
	NFS4_URING_BATCH_DONE,
	NFS4_URING_BATCH_SUSPENDED,
};

struct nfs4_uring_batch {
	/** Must stay put while SQEs are in flight; slots[] point into it. */
	struct uring_chain_req chain;

	/** enum nfs4_uring_batch_state, CAS'd between the two finishers. */
	uint32_t state;

	compound_data_t *data;

	/** argarray index of the first operation in this sub-batch. */
	unsigned int start_op;

	/** NFS operations in this sub-batch (not the same as chain.nr_ops). */
	unsigned int nr_ops;

	/** Engine the chain was submitted through. */
	struct fsal_uring_engine *engine;

	struct nfs4_uring_op_ctx op[URING_CHAIN_MAX_OPS];
};

/* --- Per-operation handlers -------------------------------------------
 *
 * One nfs4_op_<name>_uring.c per operation. Each mirrors the validation and
 * the response contract of its synchronous twin in Protocols/NFS/, and each
 * may return NFS4_URING_PREP_FALLBACK at any time to hand the operation back
 * to that twin.
 */

#define NFS4_URING_OP_HANDLERS(name)                                      \
	enum nfs4_uring_prep_result nfs4_op_##name##_uring_prep(          \
		struct nfs_argop4 *op, compound_data_t *data,             \
		struct nfs4_uring_op_ctx *uctx);                          \
	enum nfs_req_result nfs4_op_##name##_uring_finish(                \
		struct nfs_argop4 *op, compound_data_t *data,             \
		struct nfs_resop4 *resp, struct nfs4_uring_op_ctx *uctx); \
	void nfs4_op_##name##_uring_abort(struct nfs4_uring_op_ctx *uctx)

NFS4_URING_OP_HANDLERS(read);
NFS4_URING_OP_HANDLERS(write);
NFS4_URING_OP_HANDLERS(getattr);
NFS4_URING_OP_HANDLERS(access);
NFS4_URING_OP_HANDLERS(commit);
NFS4_URING_OP_HANDLERS(allocate);
NFS4_URING_OP_HANDLERS(deallocate);

/* --- Compound engine (nfs4_Compound_uring.c) --- */

/**
 * @brief Should this compound take the io_uring path at all?
 *
 * Checks only the global switch, because the export (and therefore the
 * FSAL) is not known until PUTFH has run. Per-operation batchability is
 * decided later by nfs4_uring_op_batchable().
 */
bool nfs4_uring_compound_enabled(void);

/**
 * @brief May @opcode be batched right now, for the current export?
 *
 * Answers false when there is no export yet (before PUTFH), when the FSAL
 * does not report fso_uring_batch, when the operation is category A/C, or
 * when the kernel does not implement the opcode the FSAL would use.
 */
bool nfs4_uring_op_batchable(compound_data_t *data, nfs_opnum4 opcode);

/**
 * @brief Run a compound through the segmented sub-batch engine.
 *
 * Drives data->oppos from its current value to the end of the compound,
 * alternating synchronous segments and submitted chains.
 *
 * @retval NFS_REQ_ASYNC_WAIT  a chain is in flight; the caller must return
 *                             immediately and touch nothing
 * @retval NFS_REQ_OK          every operation completed
 * @retval NFS_REQ_ERROR       an operation failed; the compound stops
 */
enum nfs_req_result nfs4_Compound_uring(compound_data_t *data,
					nfsstat4 *status);

/** Release a batch and anything its operations still hold. */
void nfs4_uring_batch_free(compound_data_t *data);

/* --- Shared helpers for the per-operation handlers --- */

/**
 * @brief Unstack data->current_obj to the underlying storage handle.
 *
 * SQE preparation needs the real file descriptor, which a stackable handle
 * such as an MDCACHE entry does not have. Returns @obj unchanged if it is
 * not stacked.
 *
 * Note this is the MDCACHE-aware helper from mdcache_uring.c; this tree has
 * no fsal_class taxonomy to generalise over.
 */
struct fsal_obj_handle *mdcache_uring_sub_handle(struct fsal_obj_handle *obj);

/**
 * @brief Map a negative CQE result to an NFSv4 status.
 *
 * Handles -ECANCELED, which the kernel returns for every SQE linked behind
 * one that failed, so an operation that was never actually attempted does
 * not report a misleading errno.
 */
nfsstat4 nfs4_uring_cqe_to_nfsstat(int32_t res);

#endif /* USE_FSAL_URING */

#endif /* NFS4_URING_INTERNAL_H */
