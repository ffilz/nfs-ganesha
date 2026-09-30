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
 * @file  nfs4_op_getattr_uring.c
 * @brief io_uring GETATTR: an MDCACHE hit is encoded in place, a miss rides
 *        a STATX SQE.
 *
 * This is the batched counterpart of nfs4_op_getattr() in
 * Protocols/NFS/nfs4_op_getattr.c, which stays the authority for everything
 * this file refuses to handle. Two things make GETATTR worth batching:
 *
 *  - it is the single most common operation in a compound, usually bolted
 *    onto the end of a READ or a WRITE, so folding it into the same chain
 *    removes a whole syscall per request;
 *
 *  - when the MDCACHE already has the attributes, no SQE is needed at all.
 *    That is what NFS4_URING_PREP_INLINE exists for: prep() encodes the
 *    fattr4 there and then and finish() never runs.
 *
 * What this file deliberately does *not* reproduce from the synchronous
 * handler is anything statx(2) cannot answer or that needs a second trip
 * through the FSAL: ACLs, fs_locations, security labels, referrals and the
 * write-delegation CB_GETATTR interlude. Every one of those returns
 * NFS4_URING_PREP_FALLBACK, which closes the chain before this operation and
 * lets the untouched synchronous handler produce the answer. Falling back is
 * always correct; it only costs the batching.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <string.h>
#include <linux/stat.h>

#include "abstract_mem.h"
#include "fsal.h"
#include "log.h"
#include "nfs4.h"
#include "nfs_convert.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "sal_functions.h"

/*
 * src/FSAL is on the include path -- src/CMakeLists.txt adds it so that
 * "FSAL_URING/uring_pers.h" resolves from Protocols/ -- but
 * src/FSAL/Stackable_FSALs is not, so the MDCACHE io_uring helpers have to
 * be named through their directory.
 */
#include "Stackable_FSALs/FSAL_MDCACHE/mdcache_int.h"

#include "nfs4_uring_internal.h"

/**
 * @brief State a planned GETATTR carries across the SQE/CQE gap.
 *
 * Lives in uctx->priv. The struct statx is a separate allocation because the
 * kernel writes into it after prep() has returned, so it may not live on
 * prep()'s stack.
 */
struct getattr_uring_state {
	/** Buffer the kernel fills; owned by this operation. */
	struct statx *stx;

	/** Attribute mask the client asked for, as prep() computed it. */
	attrmask_t mask;

	/**
	 * MDCACHE attribute generation sampled *before* the SQE was
	 * submitted. A mutation that completes while the statx is in flight
	 * bumps the generation, and feeding this value back to
	 * mdcache_uring_update_attrs_from_statx() is what stops a stale
	 * result being cached as trusted.
	 */
	uint32_t generation;
};

/**
 * @brief Bottom-most storage export's handler pair for STATX, or NULL.
 *
 * data->current_obj normally lives in MDCACHE, which stacks on the real
 * storage FSAL; batchability and the SQE layout are properties of that
 * bottom FSAL, so the export stack has to be walked all the way down.
 */
static const struct fsal_uring_fop_desc *
getattr_uring_desc(struct nfs4_uring_op_ctx *uctx)
{
	struct fsal_export *exp = (op_ctx != NULL) ? op_ctx->fsal_export : NULL;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	/* fsal_uring_fop_get() is NULL safe in exp. */
	return fsal_uring_fop_get(exp, uctx->req.fop);
}

/**
 * @brief Release everything prep() allocated for this operation.
 *
 * Idempotent, and safe on an operation whose prep() failed part way through,
 * which is what lets finish() and abort() share it.
 */
static void getattr_uring_free_state(struct nfs4_uring_op_ctx *uctx)
{
	struct getattr_uring_state *state = uctx->priv;

	/* Never leave the FSAL request pointing at freed memory. */
	uctx->req.u.statx.stx = NULL;
	uctx->priv = NULL;

	if (state == NULL)
		return;

	if (state->stx != NULL)
		gsh_free(state->stx, MEM_COMP_PROTOCOL);

	gsh_free(state, MEM_COMP_PROTOCOL);
}

/**
 * @brief Encode an fsal_attrlist into the GETATTR response's fattr4.
 *
 * This is the tail of file_To_Fattr() (Protocols/NFS/nfs_proto_tools.c) with
 * the getattrs() call removed, because both of our paths already hold the
 * attributes: the cache hit got them from MDCACHE, the cache miss from the
 * completed statx.
 *
 * @note @a attrs->request_mask is overwritten with @a mask, exactly as
 *       file_To_Fattr() restores the originally requested mask before
 *       encoding.
 *
 * @param[in]     data    Compound context; supplies currentFH and current_obj
 * @param[in]     mask    Attribute mask the client originally asked for
 * @param[in,out] attrs   Attributes to encode
 * @param[out]    fattr   Response fattr4; caller frees with nfs4_Fattr_Free()
 * @param[in]     bitmap  Requested attribute bitmap
 *
 * @return NFS4_OK, or NFS4ERR_IO if the attributes would not encode.
 */
static nfsstat4 getattr_uring_encode(compound_data_t *data, attrmask_t mask,
				     struct fsal_attrlist *attrs, fattr4 *fattr,
				     struct bitmap4 *bitmap)
{
	struct xdr_attrs_args args;

	memset(&args, 0, sizeof(args));

	args.attrs = attrs;
	args.data = data;
	args.hdl4 = &data->currentFH;

	/* The encoders take fileid and fsid from args, not from attrs, which
	 * is what keeps the export's view of the fsid (and of a junction's
	 * fileid) rather than the raw device numbers statx reports.
	 */
	args.fileid = data->current_obj->fileid;
	args.fsid = data->current_obj->fsid;

	if (attribute_is_set(bitmap, FATTR4_MOUNTED_ON_FILEID))
		get_mounted_on_fileid(data, &args.mounted_on_fileid);

	attrs->request_mask = mask;

	if (nfs4_FSALattr_To_Fattr(&args, bitmap, fattr) != 0)
		return NFS4ERR_IO;

	return NFS4_OK;
}

/**
 * @brief Would the synchronous handler need to do more than a statx?
 *
 * Only called for a REGULAR_FILE, which is the only type this file batches.
 *
 * Two things are being tested, both of which belong to the synchronous
 * handler's delegation interlude (RFC 7530 section 10.4.3):
 *
 *  - a write delegation is outstanding, so the authoritative size and mtime
 *    live on the client and nfs4_op_getattr() may have to CB_GETATTR or
 *    recall before it can answer;
 *
 *  - a CB_GETATTR response is already cached on the state handle, in which
 *    case file_To_Fattr() answers from it instead of from the filesystem.
 *
 * Deliberately coarser than the synchronous handler, which only takes the
 * interlude when the delegation belongs to a *different* client: telling the
 * two apart needs the clientid plumbing that the sync path owns, and the
 * cost of being wrong in this direction is only a lost batching opportunity.
 *
 * @note Takes and releases st_lock. Runs on a worker thread with a valid
 *       op_ctx, so this is safe.
 */
static bool getattr_uring_needs_sync_path(struct fsal_obj_handle *obj)
{
	nfs_client_id_t *deleg_client = NULL;
	bool delegated;
	bool have_cb_attrs;

	if (obj->state_hdl == NULL)
		return false;

	STATELOCK_lock(obj);

	delegated = is_write_delegated(obj, &deleg_client);
	have_cb_attrs =
		obj->state_hdl->file.cbgetattr.state == CB_GETATTR_RSP_OK;

	STATELOCK_unlock(obj);

	/* is_write_delegated() takes a reference when it reports one. */
	if (deleg_client != NULL)
		dec_client_id_ref(deleg_client);

	return delegated || have_cb_attrs;
}

/**
 * @brief Plan a GETATTR: serve it from the MDCACHE, or emit a STATX SQE.
 *
 * @param[in]     op    GETATTR arguments
 * @param[in,out] data  Compound context; current_obj is the target
 * @param[in,out] uctx  Per-operation context to fill in
 *
 * @retval NFS4_URING_PREP_INLINE    Response already encoded from the cache
 * @retval NFS4_URING_PREP_QUEUED    One STATX SQE planned
 * @retval NFS4_URING_PREP_FALLBACK  Run the synchronous handler instead
 * @retval NFS4_URING_PREP_ERROR     Validation failed, response holds it
 */
enum nfs4_uring_prep_result
nfs4_op_getattr_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			   struct nfs4_uring_op_ctx *uctx)
{
	GETATTR4args *const arg_GETATTR4 = &op->nfs_argop4_u.opgetattr;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	GETATTR4res *const res_GETATTR4 = &resp->nfs_resop4_u.opgetattr;
	fattr4 *obj_attributes =
		&res_GETATTR4->GETATTR4res_u.resok4.obj_attributes;
	struct fsal_obj_handle *obj = data->current_obj;
	const struct fsal_uring_fop_desc *desc;
	struct getattr_uring_state *state;
	struct fsal_attrlist attrs;
	fsal_status_t status;
	attrmask_t mask;
	unsigned int statx_mask;

	LogFullDebug(COMPONENT_NFS_V4, "uring GETATTR prep at position %u",
		     uctx->oppos);

	/* nfs4_op_precheck() does not set this; every handler does. */
	resp->resop = NFS4_OP_GETATTR;

	/* --- Validation, identical to nfs4_op_getattr() ------------------- */

	res_GETATTR4->status = nfs4_sanity_check_FH(data, NO_FILE_TYPE, false);

	if (res_GETATTR4->status != NFS4_OK)
		goto error;

	if (arg_GETATTR4->attr_request.bitmap4_len == 0) {
		/* Nothing was asked for, so nothing is to be done and
		 * NFS4_OK is returned with the zeroed fattr4 the response
		 * array was allocated with. data->op_resp_size is left at
		 * what nfs4_op_precheck() set, which is what the synchronous
		 * handler does too.
		 */
		res_GETATTR4->status = NFS4_OK;
		return NFS4_URING_PREP_INLINE;
	}

	if (!nfs4_Fattr_Check_Access_Bitmap(&arg_GETATTR4->attr_request,
					    FATTR4_ATTR_READ)) {
		res_GETATTR4->status = NFS4ERR_INVAL;
		goto error;
	}

	/* Errors here only mean an unrecognised attribute was requested; for
	 * GETATTR we simply do not return it, exactly as the sync path.
	 */
	(void)bitmap4_to_attrmask_t(&arg_GETATTR4->attr_request, &mask);

	nfs4_bitmap4_Remove_Unsupported(&arg_GETATTR4->attr_request);

	/* --- Everything statx cannot answer falls back -------------------- */

	/*
	 * An ACL, an fs_locations list or a security label is not in struct
	 * statx and is not reconstructible from one; each needs its own FSAL
	 * call that only the synchronous handler makes.
	 */
	if ((mask & (ATTR_ACL | ATTR4_FS_LOCATIONS | ATTR4_SEC_LABEL)) != 0) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring GETATTR: ACL/fs_locations/sec_label requested, falling back");
		return NFS4_URING_PREP_FALLBACK;
	}

	/*
	 * Only regular files are batched. That single test also disposes of
	 * referrals, which every in-tree is_referral() reports only for a
	 * DIRECTORY (fsal_common_is_referral() in FSAL/commonlib.c), so the
	 * NFS4ERR_MOVED / FATTR4_RDATTR_ERROR handling in nfs4_op_getattr()
	 * cannot apply to anything that reaches the code below.
	 */
	if (obj->type != REGULAR_FILE) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring GETATTR: object type %d is not a regular file, falling back",
			     obj->type);
		return NFS4_URING_PREP_FALLBACK;
	}

	if (getattr_uring_needs_sync_path(obj)) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring GETATTR: write delegation in effect, falling back");
		return NFS4_URING_PREP_FALLBACK;
	}

#ifdef ENABLE_RFC_ACL
	/*
	 * The ACL branch of file_To_Fattr() is unreachable here (an ACL
	 * request already fell back above), but its ENABLE_RFC_ACL
	 * counterpart is not: with that option on, reading attributes is
	 * itself an ACE4_READ_ATTRIBUTES permission check.
	 */
	status = fsal_access(obj, FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_READ_ATTR));

	if (FSAL_IS_ERROR(status)) {
		LogDebug(COMPONENT_NFS_V4_ACL,
			 "Permission check for ATTR for obj %p failed with %s",
			 obj, fsal_err_txt(status));
		res_GETATTR4->status = nfs4_Errno_status(status);
		goto error;
	}
#endif /* ENABLE_RFC_ACL */

	/* --- Cache hit: encode now, no SQE -------------------------------- */

	/*
	 * A mutating operation already planned into this same chain has not
	 * executed yet, so whatever MDCACHE holds describes the file as it
	 * was *before* that operation. Only the statx linked behind it can
	 * answer, which is why the cache is not even consulted here.
	 */
	if (!data->has_mutating_op) {
		fsal_prepare_attrs(&attrs, mask);

		if (mdcache_uring_attrs_valid(obj, mask, &attrs)) {
			res_GETATTR4->status = getattr_uring_encode(
				data, mask, &attrs, obj_attributes,
				&arg_GETATTR4->attr_request);

			fsal_release_attrs(&attrs);

			if (res_GETATTR4->status != NFS4_OK)
				goto error;

			/* Fill in and check response size and make sure it
			 * fits, exactly as nfs4_op_getattr() does.
			 */
			data->op_resp_size =
				sizeof(nfsstat4) +
				obj_attributes->attr_vals.attrlist4_len;

			res_GETATTR4->status =
				check_resp_room(data, data->op_resp_size);

			if (res_GETATTR4->status != NFS4_OK)
				goto error;

			LogFullDebug(COMPONENT_NFS_V4,
				     "uring GETATTR: served from MDCACHE, %u bytes",
				     obj_attributes->attr_vals.attrlist4_len);

			return NFS4_URING_PREP_INLINE;
		}

		/* A miss leaves attrs untouched, but stay symmetric. */
		fsal_release_attrs(&attrs);
	}

	/* --- Cache miss: one STATX SQE ------------------------------------ */

	desc = getattr_uring_desc(uctx);

	if (desc == NULL) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring GETATTR: no STATX handler for this export, falling back");
		return NFS4_URING_PREP_FALLBACK;
	}

	/*
	 * STATX_BASIC_STATS is a floor, not a preference:
	 * mdcache_uring_update_attrs_from_statx() refuses to install anything
	 * that does not cover ATTRS_POSIX and invalidates the entry instead,
	 * so asking for less would make every GETATTR miss *and* throw away
	 * the cache on the way through. Creation time is the one attribute
	 * outside the basic set that NFSv4 can ask for.
	 */
	statx_mask = STATX_BASIC_STATS;

	if ((mask & ATTR_CREATION) != 0)
		statx_mask |= STATX_BTIME;

	state = gsh_calloc(1, sizeof(*state), MEM_COMP_PROTOCOL);

	/* The kernel writes into this after prep() has returned, so it cannot
	 * live on this stack frame.
	 */
	state->stx = gsh_malloc(sizeof(struct statx), MEM_COMP_PROTOCOL);
	memset(state->stx, 0, sizeof(struct statx));

	state->mask = mask;

	/*
	 * Sample the generation *before* submitting. Anything that mutates
	 * this file while the statx is in flight bumps it, and the mismatch
	 * is what tells the completion path not to trust what it cached.
	 */
	state->generation = mdcache_uring_attr_generation(obj);

	uctx->priv = state;

	/* Unstack past MDCACHE: the SQE needs the handle of the FSAL that
	 * actually owns a descriptor.
	 */
	uctx->req.obj = mdcache_uring_sub_handle(obj);
	uctx->req.state = NULL;
	uctx->req.openflags = FSAL_O_READ;
	uctx->req.bypass = true;
	uctx->req.u.statx.mask = statx_mask;
	uctx->req.u.statx.stx = state->stx;

	/*
	 * The descriptor is the FSAL's to resolve in prep(); it is the only
	 * layer that knows how to get one. Poisoning the field first means an
	 * FSAL that cannot produce one refuses the operation (and we fall
	 * back) rather than inheriting the zero the planner memset it to,
	 * which is a perfectly valid descriptor number.
	 */
	uctx->req.fd = -1;

	uctx->nr_slots = 1;

	status = desc->prep(&uctx->req, &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(status)) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring GETATTR: FSAL refused to prep STATX (%s), falling back",
			     fsal_err_txt(status));

		/*
		 * A FALLBACK does not go through abort_prep -- the planner
		 * simply closes the chain before this operation -- so release
		 * anything the failed prep() managed to acquire here.
		 */
		if (desc->release != NULL)
			desc->release(&uctx->req);

		getattr_uring_free_state(uctx);

		return NFS4_URING_PREP_FALLBACK;
	}

	return NFS4_URING_PREP_QUEUED;

error:

	/* The attributes that may have been allocated will not be consumed.
	 * Since the response array was allocated with gsh_calloc, the buffer
	 * pointer is always NULL or valid.
	 */
	nfs4_Fattr_Free(obj_attributes);

	/* Indicate the failed response size. */
	data->op_resp_size = sizeof(nfsstat4);

	return NFS4_URING_PREP_ERROR;
}

/**
 * @brief Turn a completed STATX into the GETATTR response.
 *
 * Runs on a resume worker thread with a valid op_ctx, so it may take locks
 * and call back into the FSAL.
 *
 * @param[in]     op    GETATTR arguments
 * @param[in,out] data  Compound context
 * @param[out]    resp  GETATTR results
 * @param[in,out] uctx  Per-operation context; its priv is released here
 *
 * @retval NFS_REQ_OK     Response formatted
 * @retval NFS_REQ_ERROR  Response holds an error, the compound stops
 */
enum nfs_req_result
nfs4_op_getattr_uring_finish(struct nfs_argop4 *op, compound_data_t *data,
			     struct nfs_resop4 *resp,
			     struct nfs4_uring_op_ctx *uctx)
{
	GETATTR4args *const arg_GETATTR4 = &op->nfs_argop4_u.opgetattr;
	GETATTR4res *const res_GETATTR4 = &resp->nfs_resop4_u.opgetattr;
	fattr4 *obj_attributes =
		&res_GETATTR4->GETATTR4res_u.resok4.obj_attributes;
	struct getattr_uring_state *state = uctx->priv;
	const struct fsal_uring_fop_desc *desc;
	struct fsal_attrlist attrs;
	fsal_status_t status;

	resp->resop = NFS4_OP_GETATTR;

	desc = getattr_uring_desc(uctx);

	if (state == NULL || desc == NULL) {
		/* Unreachable: finish_cqe only runs for an operation whose
		 * prep returned QUEUED, which means both of these were
		 * resolved. Report rather than fault.
		 */
		LogCrit(COMPONENT_NFS_V4,
			"uring GETATTR completion without state (state=%p desc=%p)",
			state, desc);
		res_GETATTR4->status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	/* The FSAL's own completion runs first and turns the raw CQE into an
	 * fsal_status_t; only then is there anything to encode.
	 */
	status = desc->finish(&uctx->req);

	if (FSAL_IS_ERROR(status)) {
		res_GETATTR4->status = nfs4_uring_cqe_to_nfsstat(uctx->req.res);

		/* A failure the raw CQE does not describe (res >= 0) must
		 * not be reported as success.
		 */
		if (res_GETATTR4->status == NFS4_OK)
			res_GETATTR4->status = nfs4_Errno_status(status);

		LogDebug(COMPONENT_NFS_V4,
			 "uring GETATTR: statx failed, res=%d status=%s",
			 uctx->req.res, nfsstat4_to_str(res_GETATTR4->status));

		goto out;
	}

	/* Fold the fresh result back into the attribute cache. Installs it
	 * only if it covers everything a struct stat would have, and applies
	 * the generation race guard sampled in prep().
	 */
	mdcache_uring_update_attrs_from_statx(data->current_obj, state->stx,
					      state->generation);

	fsal_prepare_attrs(&attrs, state->mask);

	mdcache_uring_statx_to_attrs(state->stx, &attrs);

	res_GETATTR4->status =
		getattr_uring_encode(data, state->mask, &attrs, obj_attributes,
				     &arg_GETATTR4->attr_request);

	fsal_release_attrs(&attrs);

	if (res_GETATTR4->status != NFS4_OK)
		goto out;

	/* Fill in and check response size and make sure it fits. */
	data->op_resp_size = sizeof(nfsstat4) +
			     obj_attributes->attr_vals.attrlist4_len;

	res_GETATTR4->status = check_resp_room(data, data->op_resp_size);

out:

	getattr_uring_free_state(uctx);

	if (res_GETATTR4->status != NFS4_OK) {
		/* The attributes that may have been allocated will not be
		 * consumed. nfs4_Fattr_Free() is safe on an already freed or
		 * never allocated fattr4.
		 */
		nfs4_Fattr_Free(obj_attributes);

		/* Indicate the failed response size. */
		data->op_resp_size = sizeof(nfsstat4);
	}

	return nfsstat4_to_nfs_req_result(res_GETATTR4->status);
}

/**
 * @brief Release a planned GETATTR that will never complete.
 *
 * Covers the path where the chain is abandoned before submission, so no CQE
 * will ever arrive and finish() will never run. Idempotent, and safe on an
 * operation whose prep() failed part way through.
 *
 * @param[in,out] uctx  Per-operation context to drain
 */
void nfs4_op_getattr_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	const struct fsal_uring_fop_desc *desc = getattr_uring_desc(uctx);

	/* Let the FSAL drop whatever its prep() acquired (a descriptor, a
	 * share reservation) before the buffer it was going to write into
	 * goes away. release() is documented as idempotent and safe on a req
	 * whose prep() failed.
	 */
	if (desc != NULL && desc->release != NULL)
		desc->release(&uctx->req);

	getattr_uring_free_state(uctx);
}

#endif /* USE_FSAL_URING */
