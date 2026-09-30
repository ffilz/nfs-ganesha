// SPDX-License-Identifier: LGPL-3.0-or-later
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
 * @file  nfs4_op_allocate_uring.c
 * @brief io_uring prep/finish handlers for NFSv4.2 ALLOCATE and DEALLOCATE.
 *
 * Both operations are a single fallocate(2) against the current filehandle
 * and differ only in the FALLOC_FL_* mode, exactly as they do on the
 * synchronous path where allocate_deallocate() in nfs4_op_allocate.c serves
 * both. That sharing is reproduced here: everything below is one pair of
 * static helpers parameterised by the mode and by where the NFSv4 status
 * goes, with six thin public wrappers on top.
 *
 * The validation in uring_falloc_prep() is a deliberate line-by-line mirror
 * of allocate_deallocate(). The batched and sequential paths must accept and
 * reject exactly the same requests, otherwise whether a client's ALLOCATE
 * succeeds would depend on which other operations happened to share its
 * compound. Where that duplicated logic looks odd, the oddity is reproduced
 * and commented rather than quietly fixed.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <inttypes.h>
#include <linux/falloc.h>

#include "fsal.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_core.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "export_mgr.h"
#include "sal_functions.h"

/* mdcache_uring_invalidate_attrs(). src/FSAL is on the include path (see the
 * include_directories() block in src/CMakeLists.txt), but
 * src/FSAL/Stackable_FSALs is not, so the sub-FSAL directory has to be named
 * explicitly. src/ itself is on the path too, hence the FSAL/ prefix.
 */
#include "FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_int.h"

#include "nfs4_uring_internal.h"

/** fallocate(2) mode for ALLOCATE: reserve blocks, let the file size grow. */
#define URING_FALLOC_MODE_ALLOCATE 0

/**
 * fallocate(2) mode for DEALLOCATE: punch a hole and leave the file size
 * alone. Identical to what vfs_fallocate() passes for allocate == false.
 */
#define URING_FALLOC_MODE_DEALLOCATE \
	(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)

/**
 * @brief What uring_falloc_finish() needs that it cannot recompute.
 *
 * Allocated by prep only on the NFS4_URING_PREP_QUEUED path, so its mere
 * existence means an SQE was planned and a CQE is coming.
 */
struct uring_falloc_priv {
	/**
	 * Referenced state from nfs4_Check_Stateid(), or NULL when the
	 * client used an anonymous (all-0 / all-1) stateid. The reference is
	 * held across the async gap because the FSAL's prep() took the share
	 * reservation against it; it is dropped exactly once, by finish or
	 * by abort.
	 */
	struct state_t *state;

	/**
	 * ALLOCATE4res::ar_status or DEALLOCATE4res::dr_status. Stashed
	 * rather than recomputed so the completion path needs no knowledge
	 * of which of the two operations it is finishing.
	 */
	nfsstat4 *status;
};

/**
 * @brief The storage FSAL's FALLOCATE handler pair, or NULL.
 *
 * data->current_obj normally lives in MDCACHE, which stacks on the FSAL that
 * actually owns the file descriptor, so the export stack has to be walked
 * down before the vector is consulted.
 *
 * fsal_uring_fop_get() applies the runtime kernel probe as well as the
 * FSAL's own override, so a kernel without IORING_OP_FALLOCATE simply yields
 * NULL here and the caller falls back. That is why there is no compile-time
 * guard: IORING_OP_FALLOCATE is an enumerator of enum io_uring_op, not a
 * macro, so a #if on it would always be false and would delete this code.
 */
static const struct fsal_uring_fop_desc *uring_falloc_fop_desc(void)
{
	struct fsal_export *exp = op_ctx != NULL ? op_ctx->fsal_export : NULL;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	if (exp == NULL)
		return NULL;

	return fsal_uring_fop_get(exp, FSAL_URING_FOP_FALLOCATE);
}

/**
 * @brief Plan one ALLOCATE or DEALLOCATE into the chain.
 *
 * @param[in,out] data        Compound context; current_obj is the target
 * @param[in,out] uctx        Per-operation context to fill in
 * @param[in]     stateid     aa_stateid / da_stateid from the arguments
 * @param[in]     offset      aa_offset / da_offset
 * @param[in]     length      aa_length / da_length
 * @param[out]    out_status  ar_status / dr_status
 * @param[in]     mode        FALLOC_FL_* bits for the operation
 * @param[in]     tag         "ALLOCATE" or "DEALLOCATE", for logs
 */
static enum nfs4_uring_prep_result
uring_falloc_prep(compound_data_t *data, struct nfs4_uring_op_ctx *uctx,
		  stateid4 *stateid, uint64_t offset, uint64_t length,
		  nfsstat4 *out_status, int mode, const char *tag)
{
	struct state_t *state = NULL;
	struct fsal_obj_handle *obj;
	struct fsal_obj_handle *sub;
	const struct fsal_uring_fop_desc *desc;
	struct uring_falloc_priv *priv;
	fsal_status_t fsal_status;
	nfsstat4 status;
	uint64_t max_offset_write;

	/* Only files have an allocation to change. The third argument is
	 * false, matching allocate_deallocate(): a DS handle is not a valid
	 * target.
	 */
	status = nfs4_sanity_check_FH(data, REGULAR_FILE, false);

	if (status != NFS4_OK)
		goto error;

	/* If quota support is active the FSAL decides whether this client may
	 * allocate blocks at all.
	 */
	fsal_status = op_ctx->fsal_export->exp_ops.check_quota(
		op_ctx->fsal_export, CTX_FULLPATH(op_ctx), FSAL_QUOTA_BLOCKS);

	if (FSAL_IS_ERROR(fsal_status)) {
		status = NFS4ERR_DQUOT;
		goto error;
	}

	obj = data->current_obj;

	/* Check stateid correctness and get a referenced state; this also
	 * accepts the special stateids, in which case state stays NULL.
	 */
	status = nfs4_Check_Stateid(stateid, obj, &state, data,
				    STATEID_SPECIAL_ANY, 0, false, tag);

	if (status != NFS4_OK)
		goto error;

	/* NB: from here on, state == NULL means the stateid was all-0 or
	 * all-1.
	 */
	if (state != NULL) {
		struct state_t *state_open;
		struct state_deleg *sdeleg;

		switch (state->state_type) {
		case STATE_TYPE_SHARE:
			break;

		case STATE_TYPE_LOCK:
			state_open = nfs4_State_Get_Pointer(
				state->state_data.lock.openstate_key);

			if (state_open == NULL) {
				status = NFS4ERR_BAD_STATEID;
				goto error;
			}

			dec_state_t_ref(state);
			state = state_open;
			break;

		case STATE_TYPE_DELEG:
			/*
			 * As with WRITE, the stateid is only here to provide
			 * ordering with respect to locks; what matters is that
			 * the delegation permits writing at all.
			 */
			sdeleg = &state->state_data.deleg;

			if (!(sdeleg->sd_type == OPEN_DELEGATE_WRITE ||
			      sdeleg->sd_type ==
				      OPEN_DELEGATE_WRITE_ATTRS_DELEG)) {
				LogDebug(COMPONENT_STATE,
					 "Delegation type:%d state:%d",
					 sdeleg->sd_type, sdeleg->sd_state);
				status = NFS4ERR_BAD_STATEID;
				goto error;
			}
			break;

		default:
			LogDebug(COMPONENT_STATE,
				 "%s with invalid stateid of type %d", tag,
				 (int)state->state_type);
			status = NFS4ERR_BAD_STATEID;
			goto error;
		}

		/*
		 * The file MUST have been opened for writing.
		 *
		 * Reproduced verbatim from allocate_deallocate(), including
		 * the fact that a delegation stateid also reaches this test
		 * and has state_data.share.share_access read out of a union
		 * currently holding state_data.deleg. That is questionable,
		 * but it is what the sequential path does, and the batched
		 * path must not accept a request the sequential path refuses
		 * (or the other way round).
		 */
		if ((state->state_data.share.share_access &
		     OPEN4_SHARE_ACCESS_WRITE) == 0) {
			status = NFS4ERR_OPENMODE;

			if (isDebug(COMPONENT_STATE)) {
				char str[LOG_BUFF_LEN] = "\0";
				struct display_buffer dspbuf = { sizeof(str),
								 str, str };

				display_stateid(&dspbuf, state);
				LogDebug(
					COMPONENT_STATE,
					"%s %s doesn't have OPEN4_SHARE_ACCESS_WRITE",
					tag, str);
			}

			goto error;
		}
	} else {
		/*
		 * Anonymous stateid -- make sure it does not conflict with an
		 * outstanding delegation.
		 */
		if (state_deleg_conflict(obj, true)) {
			status = NFS4ERR_DELAY;
			goto error;
		}
	}

	/* Same permissions as required for a WRITE. */
	fsal_status = obj->obj_ops->test_access(obj, FSAL_WRITE_ACCESS, NULL,
						NULL, true);

	if (FSAL_IS_ERROR(fsal_status)) {
		status = nfs4_Errno_status(fsal_status);
		goto error;
	}

	max_offset_write =
		atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxOffsetWrite);

	if (max_offset_write < UINT64_MAX) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "Write offset=%" PRIu64 " count=%" PRIu64
			     " MaxOffSet=%" PRIu64,
			     offset, length, max_offset_write);

		if ((offset + length) > max_offset_write) {
			LogEvent(COMPONENT_NFS_V4,
				 "A client tried to violate"
				 " max file size %" PRIu64
				 " for exportid #%hu",
				 max_offset_write,
				 op_ctx->ctx_export->export_id);
			status = NFS4ERR_FBIG;
			goto error;
		}
	}

	LogFullDebug(COMPONENT_NFS_V4,
		     "%s offset = %" PRIu64 "  length = %" PRIu64, tag, offset,
		     length);

	if (length == 0) {
		/*
		 * Nothing changes, so there is nothing to submit. RFC 7862
		 * asks for NFS4ERR_INVAL on a zero length, but
		 * allocate_deallocate() reports success instead; the batched
		 * path deliberately agrees with the sequential one rather
		 * than with the RFC, because a client must not get a
		 * different answer depending on how its compound was split.
		 */
		*out_status = NFS4_OK;

		if (state != NULL)
			dec_state_t_ref(state);

		return NFS4_URING_PREP_INLINE;
	}

	/* The SQE acts on the storage FSAL's handle, not on the MDCACHE
	 * wrapper around it.
	 */
	sub = mdcache_uring_sub_handle(obj);

	if (sub == NULL)
		goto fallback;

	desc = uring_falloc_fop_desc();

	if (desc == NULL || desc->prep == NULL)
		goto fallback;

	uctx->req.obj = sub;
	uctx->req.state = state;
	uctx->req.openflags = FSAL_O_WRITE;
	/* vfs_fallocate() calls fsal_start_io() with bypass == false, so the
	 * share reservation is honoured here too.
	 */
	uctx->req.bypass = false;
	uctx->req.u.falloc.mode = mode;
	uctx->req.u.falloc.offset = offset;
	uctx->req.u.falloc.len = length;
	/* req.fd is deliberately left alone: resolving a descriptor needs the
	 * handle's private fd struct and its share reservation, which only
	 * the owning FSAL's prep() can reach.
	 */

	fsal_status = desc->prep(&uctx->req,
				 &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(fsal_status)) {
		LogDebug(COMPONENT_NFS_V4,
			 "%s: FSAL fallocate prep failed (%s), not batching",
			 tag, fsal_err_txt(fsal_status));

		if (desc->release != NULL)
			desc->release(&uctx->req);

		goto fallback;
	}

	priv = gsh_malloc(sizeof(*priv), MEM_COMP_PROTOCOL);
	priv->state = state;
	priv->status = out_status;
	uctx->priv = priv;

	return NFS4_URING_PREP_QUEUED;

fallback:
	/*
	 * The planner closes the chain in front of this operation and runs it
	 * through optabv4[] instead. It will not call abort_prep for this
	 * uctx, so everything acquired above has to go back now.
	 */
	uctx->req.obj = NULL;
	uctx->req.state = NULL;

	if (state != NULL)
		dec_state_t_ref(state);

	return NFS4_URING_PREP_FALLBACK;

error:
	/* Likewise: on NFS4_URING_PREP_ERROR the planner aborts the
	 * operations before this one, never this one.
	 */
	if (state != NULL)
		dec_state_t_ref(state);

	*out_status = status;

	return NFS4_URING_PREP_ERROR;
}

/**
 * @brief Turn a FALLOCATE CQE into the ALLOCATE/DEALLOCATE response.
 *
 * Runs on a resume worker with a valid op_ctx, so taking the SAL state lock
 * and calling back into the FSAL are both legal here.
 */
static enum nfs_req_result uring_falloc_finish(compound_data_t *data,
					       struct nfs_resop4 *resp,
					       struct nfs4_uring_op_ctx *uctx,
					       const char *tag)
{
	struct uring_falloc_priv *priv = uctx->priv;
	const struct fsal_uring_fop_desc *desc = uring_falloc_fop_desc();
	nfsstat4 *out_status;
	nfsstat4 status;

	/*
	 * priv is always present on this path, because finish only runs for
	 * an operation whose prep returned NFS4_URING_PREP_QUEUED. Fall back
	 * on the trick complete_op() uses -- every NFSv4 result union member
	 * starts with its nfsstat4 -- rather than leaving the response
	 * uninitialised if that ever stops being true.
	 */
	if (priv != NULL) {
		out_status = priv->status;
	} else {
		LogCrit(COMPONENT_NFS_V4, "%s: completion with no prep state",
			tag);
		out_status = &resp->nfs_resop4_u.opaccess.status;
	}

	/*
	 * The FSAL's own completion runs first: it releases the descriptor
	 * and share reservation its prep() acquired, and turns the raw CQE
	 * result into an fsal_status_t. It must run even when the operation
	 * failed, and even when the SQE was cancelled.
	 */
	if (desc != NULL && desc->finish != NULL)
		(void)desc->finish(&uctx->req);

	if (uctx->req.res < 0) {
		status = nfs4_uring_cqe_to_nfsstat(uctx->req.res);

		LogDebug(COMPONENT_NFS_V4,
			 "%s failed, cqe res %" PRId32 " -> %s", tag,
			 uctx->req.res, nfsstat4_to_str(status));
	} else {
		status = NFS4_OK;

		/*
		 * Mandatory. Both operations change the file's space usage
		 * and DEALLOCATE changes its content, and they did so behind
		 * MDCACHE's back because the SQE went straight to the storage
		 * FSAL's descriptor. A GETATTR later in the same compound
		 * must not be answered from the attributes cached before
		 * this.
		 */
		mdcache_uring_invalidate_attrs(data->current_obj);
	}

	*out_status = status;

	/* The FSAL is done with the state, so the reference prep took can go
	 * back now.
	 */
	uctx->req.state = NULL;

	if (priv != NULL) {
		if (priv->state != NULL)
			dec_state_t_ref(priv->state);

		gsh_free(priv, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
	}

	return nfsstat4_to_nfs_req_result(status);
}

/**
 * @brief Release prep state for a planned operation that will never run.
 *
 * Reached when a later operation in the same sub-batch failed to plan, or
 * when the chain could not be submitted. Idempotent, and safe on a uctx
 * whose prep never got as far as allocating anything.
 */
static void uring_falloc_abort(struct nfs4_uring_op_ctx *uctx)
{
	struct uring_falloc_priv *priv = uctx->priv;
	const struct fsal_uring_fop_desc *desc = uring_falloc_fop_desc();

	/* Hands back the descriptor and share reservation the FSAL's prep()
	 * took. Required to be safe on a req that was never prepped.
	 */
	if (desc != NULL && desc->release != NULL)
		desc->release(&uctx->req);

	uctx->req.state = NULL;

	if (priv == NULL)
		return;

	if (priv->state != NULL)
		dec_state_t_ref(priv->state);

	gsh_free(priv, MEM_COMP_PROTOCOL);
	uctx->priv = NULL;
}

/* -------------------------------------------------------------------------
 * ALLOCATE
 * -------------------------------------------------------------------------
 */

/**
 * @brief Plan an NFS4_OP_ALLOCATE into the chain.
 *
 * @param[in]     op    Arguments for nfs4_op
 * @param[in,out] data  Compound request's data
 * @param[in,out] uctx  Per-operation io_uring context
 */
enum nfs4_uring_prep_result
nfs4_op_allocate_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			    struct nfs4_uring_op_ctx *uctx)
{
	ALLOCATE4args *const arg_ALLOCATE4 = &op->nfs_argop4_u.opallocate;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	ALLOCATE4res *const res_ALLOCATE4 = &resp->nfs_resop4_u.opallocate;

	/* nfs4_op_precheck() only stamps resop on its own error path, and the
	 * planner turns an NFS4_URING_PREP_ERROR straight into a response, so
	 * the opcode has to be in place before any early return below.
	 */
	resp->resop = NFS4_OP_ALLOCATE;

	return uring_falloc_prep(data, uctx, &arg_ALLOCATE4->aa_stateid,
				 arg_ALLOCATE4->aa_offset,
				 arg_ALLOCATE4->aa_length,
				 &res_ALLOCATE4->ar_status,
				 URING_FALLOC_MODE_ALLOCATE, "ALLOCATE");
}

/**
 * @brief Turn an ALLOCATE CQE into its response.
 *
 * @param[in]     op    Arguments for nfs4_op
 * @param[in,out] data  Compound request's data
 * @param[out]    resp  Results for nfs4_op
 * @param[in,out] uctx  Per-operation io_uring context
 */
enum nfs_req_result
nfs4_op_allocate_uring_finish(struct nfs_argop4 *op, compound_data_t *data,
			      struct nfs_resop4 *resp,
			      struct nfs4_uring_op_ctx *uctx)
{
	return uring_falloc_finish(data, resp, uctx, "ALLOCATE");
}

/**
 * @brief Release ALLOCATE prep state for a chain that will never be sent.
 *
 * @param[in,out] uctx  Per-operation io_uring context
 */
void nfs4_op_allocate_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	uring_falloc_abort(uctx);
}

/* -------------------------------------------------------------------------
 * DEALLOCATE
 * -------------------------------------------------------------------------
 */

/**
 * @brief Plan an NFS4_OP_DEALLOCATE into the chain.
 *
 * @param[in]     op    Arguments for nfs4_op
 * @param[in,out] data  Compound request's data
 * @param[in,out] uctx  Per-operation io_uring context
 */
enum nfs4_uring_prep_result
nfs4_op_deallocate_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			      struct nfs4_uring_op_ctx *uctx)
{
	DEALLOCATE4args *const arg_DEALLOC = &op->nfs_argop4_u.opdeallocate;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	DEALLOCATE4res *const res_DEALLOC = &resp->nfs_resop4_u.opdeallocate;

	resp->resop = NFS4_OP_DEALLOCATE;

	return uring_falloc_prep(data, uctx, &arg_DEALLOC->da_stateid,
				 arg_DEALLOC->da_offset, arg_DEALLOC->da_length,
				 &res_DEALLOC->dr_status,
				 URING_FALLOC_MODE_DEALLOCATE, "DEALLOCATE");
}

/**
 * @brief Turn a DEALLOCATE CQE into its response.
 *
 * @param[in]     op    Arguments for nfs4_op
 * @param[in,out] data  Compound request's data
 * @param[out]    resp  Results for nfs4_op
 * @param[in,out] uctx  Per-operation io_uring context
 */
enum nfs_req_result
nfs4_op_deallocate_uring_finish(struct nfs_argop4 *op, compound_data_t *data,
				struct nfs_resop4 *resp,
				struct nfs4_uring_op_ctx *uctx)
{
	return uring_falloc_finish(data, resp, uctx, "DEALLOCATE");
}

/**
 * @brief Release DEALLOCATE prep state for a chain that will never be sent.
 *
 * @param[in,out] uctx  Per-operation io_uring context
 */
void nfs4_op_deallocate_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	uring_falloc_abort(uctx);
}

#endif /* USE_FSAL_URING */
