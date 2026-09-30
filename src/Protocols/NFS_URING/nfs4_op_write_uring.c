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
 * @file  nfs4_op_write_uring.c
 * @brief io_uring batched form of NFS4_OP_WRITE.
 *
 * The synchronous handler, nfs4_op_write() in Protocols/NFS/nfs4_op_write.c,
 * remains the authority for what a WRITE means: every validation step below
 * is a transcription of that function, in the same order, producing the same
 * status codes. The only thing that differs is how the data reaches the
 * filesystem — a WRITEV SQE placed in the compound's chain instead of a
 * write2() call that suspends the request on its own.
 *
 * Two properties make the batched form cheap:
 *
 *  - no copy. arg_WRITE4->data.iov already points into the decoded RPC
 *    buffer, which ntirpc keeps alive for the whole request, so the client's
 *    iovec is handed to the kernel as is.
 *
 *  - no per-operation suspend. The planner emits this SQE alongside the rest
 *    of the sub-batch and the request suspends once, for the whole chain.
 *
 * @note A stable write (DATA_SYNC4/FILE_SYNC4, or an export with Commit)
 *       owns two chain slots, the WRITEV and a linked fsync, and is
 *       replied FILE_SYNC4 only if that fsync completed.
 *       See nfs4_op_write_uring_prep().
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include "abstract_atomic.h"
#include "export_mgr.h"
#include "fsal.h"
#include "fsal_convert.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "sal_functions.h"
#include "server_stats.h"

/* src/FSAL is on the include path (see the include_directories() block in
 * src/CMakeLists.txt), which is the only way to reach an MDCACHE-internal
 * header from the protocol layer. mdcache_int.h's own quoted includes
 * resolve relative to its directory, so pulling it in from here is enough.
 */
#include "Stackable_FSALs/FSAL_MDCACHE/mdcache_int.h"

#include "nfs4_uring_internal.h"

/**
 * @brief What the WRITE completion needs that the CQE does not carry.
 *
 * prep_sqe runs on the worker that planned the chain and finish_cqe on the
 * worker that resumed it; nothing survives in between except struct
 * nfs4_uring_op_ctx, so every reference taken during validation is parked
 * here and released by exactly one of finish_cqe or abort_prep.
 */
struct nfs4_uring_write_priv {
	/** Referenced state owner, or NULL. Mirrors nfs4_write_data::owner. */
	state_owner_t *owner;

	/** Referenced state from nfs4_Check_Stateid(), or NULL. */
	state_t *state;

	/** Response slot to fill in. */
	WRITE4res *res_WRITE4;

	/**
	 * data->current_obj as it was when the chain was planned, i.e. the
	 * MDCACHE handle rather than the unstacked one in req.obj. Needed so
	 * the completion can invalidate the attribute cache. Not referenced:
	 * the compound holds current_obj for the life of the request, which
	 * is exactly what nfs4_write_data::obj relies on too.
	 */
	struct fsal_obj_handle *obj;

	/**
	 * Bytes the client asked for, as nfs4_complete_write() accounts them
	 * (write_arg->iov[0].iov_len), so the I/O statistics match the
	 * synchronous path byte for byte.
	 */
	uint64_t requested;

	/**
	 * The write was stable (DATA_SYNC4/FILE_SYNC4 or an export with
	 * EXPORT_OPTION_COMMIT), so the operation owns two chain slots: the
	 * WRITEV at uctx->slot and the linked fsync at uctx->slot + 1.
	 */
	bool stable;
};

/**
 * @brief Bottom-most storage export under the current, possibly stacked one.
 *
 * The uring vector is published by the FSAL that owns the file descriptor,
 * never by MDCACHE, so the stack has to be walked all the way down.
 */
static struct fsal_export *write_uring_storage_export(void)
{
	struct fsal_export *exp;

	if (op_ctx == NULL)
		return NULL;

	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	return exp;
}

/** Descriptor pair for FSAL_URING_FOP_WRITE on the current export, or NULL. */
static const struct fsal_uring_fop_desc *
write_uring_desc(struct nfs4_uring_op_ctx *uctx)
{
	struct fsal_export *exp = write_uring_storage_export();

	if (exp == NULL)
		return NULL;

	return fsal_uring_fop_get(exp, uctx->req.fop);
}

/** Release the references parked in @priv and free it. Idempotent. */
static void write_uring_release_priv(struct nfs4_uring_op_ctx *uctx)
{
	struct nfs4_uring_write_priv *priv = uctx->priv;

	if (priv == NULL)
		return;

	if (priv->owner != NULL) {
		if (op_ctx != NULL)
			op_ctx->clientid = NULL;
		dec_state_owner_ref(priv->owner);
		priv->owner = NULL;
	}

	if (priv->state != NULL) {
		dec_state_t_ref(priv->state);
		priv->state = NULL;
	}

	/* The FSAL must not be left holding a pointer to a state whose
	 * reference we have just dropped.
	 */
	uctx->req.state = NULL;

	gsh_free(priv, MEM_COMP_PROTOCOL);
	uctx->priv = NULL;
}

/**
 * @brief Plan an NFS4_OP_WRITE into the compound's chain.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context; current_obj is the target file
 * @param[in,out] uctx  Per-operation context to fill in
 *
 * @retval NFS4_URING_PREP_QUEUED   one WRITEV SQE planned
 * @retval NFS4_URING_PREP_INLINE   zero-length write, answered in place
 * @retval NFS4_URING_PREP_FALLBACK run nfs4_op_write() for this operation
 * @retval NFS4_URING_PREP_ERROR    validation failed, response holds the error
 */
enum nfs4_uring_prep_result
nfs4_op_write_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			 struct nfs4_uring_op_ctx *uctx)
{
	WRITE4args *const arg_WRITE4 = &op->nfs_argop4_u.opwrite;
	struct nfs_resop4 *const resp = &data->resarray[uctx->oppos];
	WRITE4res *const res_WRITE4 = &resp->nfs_resop4_u.opwrite;
	enum nfs4_uring_prep_result result = NFS4_URING_PREP_ERROR;
	struct fsal_obj_handle *obj = NULL;
	struct fsal_obj_handle *sub = NULL;
	const struct fsal_uring_fop_desc *desc;
	const struct fsal_uring_fop_desc *sync_desc = NULL;
	struct nfs4_uring_write_priv *priv;
	state_t *state_found = NULL;
	state_t *state_open = NULL;
	state_owner_t *owner = NULL;
	fsal_status_t fsal_status;
	struct gsh_buffdesc verf_desc;
	bool anonymous_started = false;
	uint64_t MaxWrite;
	uint64_t MaxOffsetWrite;
	bool force_sync;
	bool stable;
	uint64_t offset;
	uint64_t size;

	resp->resop = NFS4_OP_WRITE;
	res_WRITE4->status = NFS4_OK;

	/*
	 * A pNFS data server write does not go through an fsal_obj_handle at
	 * all; op_dswrite() calls into ctx_pnfs_ds with the DS handle. There
	 * is nothing to hang an SQE off, so the synchronous handler takes it.
	 */
	if (data->minorversion > 0 && nfs4_Is_Fh_DSHandle(&data->currentFH))
		return NFS4_URING_PREP_FALLBACK;

	MaxWrite = atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxWrite);
	MaxOffsetWrite =
		atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxOffsetWrite);
	force_sync = (op_ctx->export_perms.options & EXPORT_OPTION_COMMIT) != 0;

	/*
	 * DATA_SYNC4/FILE_SYNC4 (and an export with EXPORT_OPTION_COMMIT,
	 * which makes every write stable) are a WRITEV plus an fsync linked
	 * behind it: two chain slots that share this operation's single
	 * struct fsal_uring_fop_req (and so its descriptor and credential).
	 * The kernel runs the fsync only if the WRITEV succeeded in full.
	 *
	 * The reply is FILE_SYNC4 only when that fsync completed; otherwise
	 * it is UNSTABLE4, exactly as nfs4_op_write() downgrades on a failed
	 * fsync. Answering FILE_SYNC4 for a WRITEV whose fsync did not run
	 * would tell the client its data is durable when it is only in the
	 * page cache.
	 */
	stable = arg_WRITE4->stable != UNSTABLE4 || force_sync;

	/* Do basic checks on a filehandle. Only files can be written. */
	res_WRITE4->status = nfs4_sanity_check_FH(data, REGULAR_FILE, true);

	if (res_WRITE4->status != NFS4_OK)
		return NFS4_URING_PREP_ERROR;

	/* If quota support is active, check whether the FSAL still has room
	 * for us.
	 */
	fsal_status = op_ctx->fsal_export->exp_ops.check_quota(
		op_ctx->fsal_export, CTX_FULLPATH(op_ctx), FSAL_QUOTA_BLOCKS);

	if (FSAL_IS_ERROR(fsal_status)) {
		res_WRITE4->status = NFS4ERR_DQUOT;
		return NFS4_URING_PREP_ERROR;
	}

	/* The vnode to manage is the current one. */
	obj = data->current_obj;

	/* Check stateid correctness and get a pointer to the state (this also
	 * recognises the special stateids).
	 */
	res_WRITE4->status = nfs4_Check_Stateid(&arg_WRITE4->stateid, obj,
						&state_found, data,
						STATEID_SPECIAL_ANY, 0, false,
						"WRITE");

	if (res_WRITE4->status != NFS4_OK)
		return NFS4_URING_PREP_ERROR;

	/* NB: after this point, if state_found == NULL then the stateid is
	 * all-0 or all-1.
	 */
	if (state_found != NULL) {
		struct state_deleg *sdeleg;

		switch (state_found->state_type) {
		case STATE_TYPE_SHARE:
			state_open = state_found;
			/* Note this causes an extra refcount, but it
			 * simplifies logic below.
			 */
			inc_state_t_ref(state_open);
			break;

		case STATE_TYPE_LOCK:
			state_open = nfs4_State_Get_Pointer(
				state_found->state_data.lock.openstate_key);

			if (state_open == NULL) {
				res_WRITE4->status = NFS4ERR_BAD_STATEID;
				goto out;
			}

			break;

		case STATE_TYPE_DELEG:
			/* Check that the delegation state allows WRITE. */
			sdeleg = &state_found->state_data.deleg;

			if (!(sdeleg->sd_type == OPEN_DELEGATE_WRITE ||
			      sdeleg->sd_type ==
				      OPEN_DELEGATE_WRITE_ATTRS_DELEG)) {
				LogDebug(COMPONENT_STATE,
					 "Delegation type:%d state:%d",
					 sdeleg->sd_type, sdeleg->sd_state);
				res_WRITE4->status = NFS4ERR_BAD_STATEID;
				goto out;
			}

			state_open = NULL;
			break;

		default:
			res_WRITE4->status = NFS4ERR_BAD_STATEID;
			LogDebug(COMPONENT_STATE,
				 "WRITE with invalid stateid of type %d",
				 (int)state_found->state_type);
			goto out;
		}

		/* This is a write operation, so the file MUST have been
		 * opened for writing.
		 */
		if (state_open != NULL &&
		    (state_open->state_data.share.share_access &
		     OPEN4_SHARE_ACCESS_WRITE) == 0) {
			/* Bad open mode, return NFS4ERR_OPENMODE */
			res_WRITE4->status = NFS4ERR_OPENMODE;

			if (isDebug(COMPONENT_STATE)) {
				char str[LOG_BUFF_LEN] = "\0";
				struct display_buffer dspbuf = { sizeof(str),
								 str, str };

				display_stateid(&dspbuf, state_found);
				LogDebug(
					COMPONENT_STATE,
					"WRITE %s doesn't have OPEN4_SHARE_ACCESS_WRITE",
					str);
			}

			goto out;
		}
	} else {
		/* Special stateid, no open state; check for share conflicts. */
		state_open = NULL;

		if (state_deleg_conflict(obj, true)) {
			res_WRITE4->status = NFS4ERR_DELAY;
			goto out;
		}

		anonymous_started = true;
	}

	/* Need to permission check the write. */
	fsal_status = obj->obj_ops->test_access(obj, FSAL_WRITE_ACCESS, NULL,
						NULL, true);

	if (FSAL_IS_ERROR(fsal_status)) {
		res_WRITE4->status = nfs4_Errno_status(fsal_status);
		goto out;
	}

	/* Get the characteristics of the I/O to be made. */
	offset = arg_WRITE4->offset;
	size = arg_WRITE4->data.data_len;

	LogFullDebug(COMPONENT_NFS_V4,
		     "offset = %" PRIu64 "  length = %" PRIu64 "  stable = %d",
		     offset, size, arg_WRITE4->stable);

	if (MaxOffsetWrite < UINT64_MAX) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "Write offset=%" PRIu64 " count=%" PRIu64
			     " MaxOffSet=%" PRIu64,
			     offset, size, MaxOffsetWrite);

		if ((offset + size) > MaxOffsetWrite) {
			LogEvent(COMPONENT_NFS_V4,
				 "A client tried to violate"
				 " max file size %" PRIu64
				 " for exportid #%hu",
				 MaxOffsetWrite,
				 op_ctx->ctx_export->export_id);
			res_WRITE4->status = NFS4ERR_FBIG;
			goto out;
		}
	}

	if (size > MaxWrite) {
		/*
		 * The client asked for more than the export allows.
		 * nfs4_op_write() restricts it by clamping write_arg
		 * ->io_request, but the iovec it hands the FSAL still
		 * describes the whole request and only the FSAL knows how to
		 * reconcile the two. The ring has nothing but the iovec, so
		 * honouring the clamp here would mean building a truncated
		 * copy of the client's iovec array — more machinery than this
		 * path deserves for a case clients avoid by reading
		 * FATTR4_MAXWRITE at mount time. Hand it to nfs4_op_write().
		 */
		LogFullDebug(COMPONENT_NFS_V4,
			     "write requested size = %" PRIu64
			     " write allowed size = %" PRIu64 ", not batching",
			     size, MaxWrite);
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	LogFullDebug(COMPONENT_NFS_V4, "offset = %" PRIu64 " length = %" PRIu64,
		     offset, size);

	/* If size == 0 no I/O is actually made and everything is alright.
	 * Answered in place, exactly as nfs4_op_write() does, including
	 * FILE_SYNC4: nothing was written, so nothing is unstable.
	 */
	if (size == 0) {
		res_WRITE4->WRITE4res_u.resok4.count = 0;
		res_WRITE4->WRITE4res_u.resok4.committed = FILE_SYNC4;

		verf_desc.addr = res_WRITE4->WRITE4res_u.resok4.writeverf;
		verf_desc.len = sizeof(verifier4);
		op_ctx->fsal_export->exp_ops.get_write_verifier(
			op_ctx->fsal_export, &verf_desc);

		res_WRITE4->status = NFS4_OK;
		server_stats_io_done(0, 0, true, true /*is_write*/);

		result = NFS4_URING_PREP_INLINE;
		goto out;
	}

	/* An iovec is the only thing the SQE can be built from. */
	if (arg_WRITE4->data.iov == NULL || arg_WRITE4->data.iovcnt == 0) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "WRITE of %" PRIu64
			     " bytes with no iovec, not batching",
			     size);
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	if (!anonymous_started && data->minorversion == 0) {
		owner = get_state_owner_ref(state_found);

		if (owner != NULL) {
			op_ctx->clientid =
				&owner->so_owner.so_nfs4_owner.so_clientid;
		}
	}

	/* SQEs act on the storage handle, not on the MDCACHE wrapper. */
	sub = mdcache_uring_sub_handle(obj);

	if (sub == NULL) {
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	uctx->req.obj = sub;
	uctx->req.state = state_found;
	uctx->req.openflags = FSAL_O_WRITE;
	/*
	 * nfs4_op_write() calls write2() with bypass == false unconditionally.
	 * Unlike READ, which bypasses the share reservation check for an
	 * anonymous stateid with a non-zero seqid, a WRITE never does.
	 */
	uctx->req.bypass = false;

	/*
	 * The payload is still sitting in the decoded RPC buffer, which
	 * ntirpc keeps alive until the reply is sent, so the client's iovec
	 * can be handed to the kernel unchanged. Copying it would defeat the
	 * point of the whole path.
	 */
	uctx->req.u.rw.iov = arg_WRITE4->data.iov;
	uctx->req.u.rw.iovcnt = (int)arg_WRITE4->data.iovcnt;
	uctx->req.u.rw.offset = offset;
	/* No registered buffer: the data is wire memory, not ring memory. */
	uctx->req.u.rw.buf = NULL;

	/* req.fd is deliberately left to the FSAL's prep(). Resolving a
	 * descriptor means fsal_start_io(), which also takes a share
	 * reservation, and only the layer that takes it can give it back in
	 * finish()/release().
	 */

	/* One WRITEV, plus a linked fsync when the write is stable. */
	uctx->nr_slots = 1;

	desc = write_uring_desc(uctx);

	if (desc == NULL) {
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	if (stable) {
		/* Look the fsync handler up before anything is acquired, so
		 * an FSAL or kernel without it falls back cleanly.
		 */
		sync_desc = fsal_uring_fop_get(write_uring_storage_export(),
					       FSAL_URING_FOP_SYNC);

		if (sync_desc == NULL) {
			result = NFS4_URING_PREP_FALLBACK;
			goto out;
		}
	}

	fsal_status = desc->prep(&uctx->req,
				 &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(fsal_status)) {
		LogDebug(COMPONENT_NFS_V4,
			 "uring WRITE prep failed (%s), not batching",
			 msg_fsal_err(fsal_status.major));

		/* Give back whatever a partially successful prep() took. */
		if (desc->release != NULL)
			desc->release(&uctx->req);

		uctx->req.state = NULL;
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	if (stable) {
		/* Same req, so the FSAL's sync prep reuses the descriptor the
		 * write prep acquired rather than taking a second one.
		 */
		fsal_status = sync_desc->prep(
			&uctx->req, &uctx->batch->chain.ops[uctx->slot + 1]);

		if (FSAL_IS_ERROR(fsal_status)) {
			LogDebug(COMPONENT_NFS_V4,
				 "uring WRITE sync prep failed (%s), not batching",
				 msg_fsal_err(fsal_status.major));

			if (desc->release != NULL)
				desc->release(&uctx->req);

			uctx->req.state = NULL;
			result = NFS4_URING_PREP_FALLBACK;
			goto out;
		}

		uctx->nr_slots = 2;
	}

	priv = gsh_malloc(sizeof(*priv), MEM_COMP_PROTOCOL);
	priv->owner = owner;
	priv->state = state_found;
	priv->res_WRITE4 = res_WRITE4;
	priv->obj = obj;
	/* nfs4_complete_write() accounts iov[0].iov_len, not data_len. */
	priv->requested = arg_WRITE4->data.iov[0].iov_len;
	priv->stable = stable;

	uctx->priv = priv;

	result = NFS4_URING_PREP_QUEUED;

out:

	if (state_open != NULL) {
		dec_state_t_ref(state_open);
		state_open = NULL;
	}

	if (result != NFS4_URING_PREP_QUEUED) {
		/*
		 * Nothing is queued, so neither finish_cqe nor abort_prep will
		 * run for this operation and these references have no other
		 * owner.
		 *
		 * Note that nfs4_op_write() does not drop state_found on its
		 * own "goto out" error paths — the reference nfs4_Check_
		 * Stateid() took is only ever released from
		 * nfs4_complete_write(), so those paths leak it. That is a
		 * pre-existing bug in the synchronous handler and there is no
		 * reason to reproduce it here; releasing is correct and
		 * affects nothing outside this function.
		 */
		if (owner != NULL) {
			op_ctx->clientid = NULL;
			dec_state_owner_ref(owner);
			owner = NULL;
		}

		if (state_found != NULL) {
			dec_state_t_ref(state_found);
			state_found = NULL;
		}

		uctx->req.state = NULL;
	}

	return result;
}

/**
 * @brief Turn the WRITEV CQE into a WRITE4res.
 *
 * Runs on a resume worker with a valid op_ctx, so it may take locks and call
 * back into the FSAL. Mirrors nfs4_complete_write().
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context
 * @param[out]    resp  Response slot for this operation
 * @param[in,out] uctx  Per-operation context planned by prep
 *
 * @retval NFS_REQ_OK    response formatted, compound continues
 * @retval NFS_REQ_ERROR response holds an error, compound short-circuits
 */
enum nfs_req_result nfs4_op_write_uring_finish(struct nfs_argop4 *op,
					       compound_data_t *data,
					       struct nfs_resop4 *resp,
					       struct nfs4_uring_op_ctx *uctx)
{
	WRITE4res *const res_WRITE4 = &resp->nfs_resop4_u.opwrite;
	struct nfs4_uring_write_priv *priv = uctx->priv;
	const struct fsal_uring_fop_desc *desc;
	struct gsh_buffdesc verf_desc;
	uint64_t amount = 0;
	int32_t sync_res = 0;

	resp->resop = NFS4_OP_WRITE;

	if (priv == NULL) {
		/* prep_sqe only returns QUEUED after allocating priv, and the
		 * planner only arranges a finish for a QUEUED operation, so
		 * this cannot happen. Report it rather than dereference NULL.
		 */
		LogCrit(COMPONENT_NFS_V4,
			"uring WRITE completion with no planning state");
		res_WRITE4->status = NFS4ERR_SERVERFAULT;
		return NFS_REQ_ERROR;
	}

	/* The linked fsync's CQE sits right behind the WRITEV's. Read it
	 * before the FSAL's finish() runs; that only consumes req.res.
	 */
	if (priv->stable)
		sync_res = uctx->batch->chain.cqes[uctx->slot + 1].res;

	/*
	 * The FSAL's completion runs first: it releases the descriptor and
	 * the share reservation its prep() acquired, and turns the raw CQE
	 * result into req->status and req->u.rw.amount.
	 */
	desc = write_uring_desc(uctx);

	if (desc != NULL && desc->finish != NULL) {
		(void)desc->finish(&uctx->req);
	} else {
		/* The vector is per export and was present at prep time, so
		 * this is unreachable; derive the byte count rather than
		 * report a stale one.
		 */
		LogCrit(COMPONENT_NFS_V4,
			"uring WRITE completion with no FSAL descriptor");
		uctx->req.u.rw.amount =
			uctx->req.res > 0 ? (uint64_t)uctx->req.res : 0;
	}

	if (uctx->req.res < 0) {
		/*
		 * nfs4_uring_cqe_to_nfsstat() runs the errno through
		 * posix2fsal_status() and nfs4_Errno_status(), so -ENOSPC
		 * becomes NFS4ERR_NOSPC and -EFBIG becomes NFS4ERR_FBIG, and
		 * it maps the -ECANCELED the kernel gives every SQE linked
		 * behind a failed one onto NFS4ERR_IO.
		 */
		res_WRITE4->status = nfs4_uring_cqe_to_nfsstat(uctx->req.res);
	} else {
		WRITE4resok *const resok = &res_WRITE4->WRITE4res_u.resok4;

		amount = uctx->req.u.rw.amount;

		resok->count = (count4)amount;

		/*
		 * FILE_SYNC4 only when the linked fsync actually completed.
		 * If it failed, or was cancelled because the WRITEV was short,
		 * the data is only in the page cache: UNSTABLE4 is the honest
		 * answer and the client resends or COMMITs, as with
		 * nfs4_op_write()'s downgrade.
		 */
		if (priv->stable && sync_res >= 0) {
			resok->committed = FILE_SYNC4;
		} else {
			resok->committed = UNSTABLE4;

			if (priv->stable)
				LogDebug(COMPONENT_NFS_V4,
					 "uring WRITE linked fsync did not complete (%d), replying UNSTABLE4",
					 sync_res);
		}

		verf_desc.addr = resok->writeverf;
		verf_desc.len = sizeof(verifier4);
		op_ctx->fsal_export->exp_ops.get_write_verifier(
			op_ctx->fsal_export, &verf_desc);

		res_WRITE4->status = NFS4_OK;

		/*
		 * The SQE changed size, mtime and ctime behind MDCACHE's
		 * back: it never saw a write2() call, so nothing invalidated
		 * its attribute cache. Without this a GETATTR later in the
		 * same compound would be answered from the pre-write cache
		 * and the client would be told the file never grew.
		 *
		 * The helper bumps attr_generation before clearing
		 * MDCACHE_TRUST_ATTRS, which is the order a concurrent
		 * refresher's generation check depends on, and is a no-op
		 * when MDCACHE is not stacked on this export.
		 */
		mdcache_uring_invalidate_attrs(priv->obj);
	}

	server_stats_io_done(priv->requested, amount,
			     res_WRITE4->status == NFS4_OK, true /*is_write*/);

	write_uring_release_priv(uctx);

	return nfsstat4_to_nfs_req_result(res_WRITE4->status);
}

/**
 * @brief Release what prep planned, for a chain that will never be submitted.
 *
 * Reached when a later operation in the same sub-batch fails to plan, or when
 * the submission itself fails, so no CQE will ever arrive for this operation.
 * Safe to call more than once and safe on a context whose prep failed part
 * way through.
 *
 * The wire data buffer is deliberately not touched: req.u.rw.iov points into
 * the decoded RPC message, which this operation does not own.
 *
 * @param[in,out] uctx  Per-operation context to unwind
 */
void nfs4_op_write_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	const struct fsal_uring_fop_desc *desc = write_uring_desc(uctx);

	/* Hands back the descriptor and share reservation the FSAL's prep()
	 * took. NULL for an FSAL whose prep() acquires nothing.
	 */
	if (desc != NULL && desc->release != NULL)
		desc->release(&uctx->req);

	uctx->req.state = NULL;

	write_uring_release_priv(uctx);
}

#endif /* USE_FSAL_URING */
