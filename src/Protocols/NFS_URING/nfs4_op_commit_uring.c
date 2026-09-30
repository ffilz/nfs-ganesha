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
 * @file  nfs4_op_commit_uring.c
 * @brief io_uring batched NFS4_OP_COMMIT.
 *
 * The synchronous counterpart is nfs4_op_commit() in Protocols/NFS, which is
 * still the authority: everything validated there is validated here, in the
 * same order, and the response this file produces is byte-for-byte what that
 * handler would have produced.  The only difference is that the durability
 * barrier is issued as an SQE in a linked chain instead of as a blocking
 * fsal_commit().
 *
 * Mapping onto the storage layer is FSAL_URING_FOP_SYNC, which the planner
 * has already stamped into uctx->req.fop before prep runs.
 *
 * A note on the range.  arg_COMMIT4->offset / ->count are passed down in
 * req->u.sync, but they are advisory: FSAL_VFS_URING deliberately implements
 * FSAL_URING_FOP_SYNC as io_uring_prep_fsync() rather than
 * IORING_OP_SYNC_FILE_RANGE, because sync_file_range(2) writes back page
 * cache only — it flushes neither the inode metadata nor the device write
 * cache — and therefore cannot honour the durability an NFS COMMIT promises
 * (RFC 8881 18.3.3: the data must survive a server restart).  An fsync is a
 * strict superset of the requested range, so ignoring offset/len is
 * conservative rather than wrong, and a backend that can honour a true range
 * sync is free to read these fields.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <inttypes.h>
#include <stdint.h>

#include "fsal.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"

/* mdcache_uring_invalidate_attrs() lives in the MDCACHE private header.
 * src/FSAL is on the global include path (see the include_directories() block
 * in src/CMakeLists.txt), but src/FSAL/Stackable_FSALs is not, so the path has
 * to be spelled relative to src/FSAL.
 */
#include "Stackable_FSALs/FSAL_MDCACHE/mdcache_int.h"

#include "nfs4_uring_internal.h"

/**
 * @brief Plan an NFS4_OP_COMMIT into the chain.
 *
 * Runs on a worker thread with a valid op_ctx, before submission.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context; current_obj is the file to commit
 * @param[in,out] uctx  Per-operation context to fill in
 *
 * @return One of enum nfs4_uring_prep_result.
 */
enum nfs4_uring_prep_result
nfs4_op_commit_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			  struct nfs4_uring_op_ctx *uctx)
{
	COMMIT4args *const arg_COMMIT4 = &op->nfs_argop4_u.opcommit;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	COMMIT4res *const res_COMMIT4 = &resp->nfs_resop4_u.opcommit;
	struct fsal_obj_handle *sub;
	struct fsal_export *exp;
	const struct fsal_uring_fop_desc *desc;
	fsal_status_t st;

	resp->resop = NFS4_OP_COMMIT;
	res_COMMIT4->status = NFS4_OK;

	LogFullDebug(COMPONENT_NFS_V4,
		     "uring commit over offset = %" PRIu64 ", size = %" PRIu32,
		     arg_COMMIT4->offset, arg_COMMIT4->count);

	/* A pNFS data-server commit goes through ctx_pnfs_ds->s_ops, not
	 * through an fsal_obj_handle at all, so there is no descriptor to
	 * fsync and nothing here can express it. op_dscommit() handles it on
	 * the synchronous path.
	 */
	if (nfs4_Is_Fh_DSHandle(&data->currentFH))
		return NFS4_URING_PREP_FALLBACK;

	/* Same basic filehandle checks as nfs4_op_commit(): COMMIT is only
	 * defined on a regular file.
	 */
	res_COMMIT4->status = nfs4_sanity_check_FH(data, REGULAR_FILE, true);

	if (res_COMMIT4->status != NFS4_OK)
		return NFS4_URING_PREP_ERROR;

	sub = mdcache_uring_sub_handle(data->current_obj);

	if (sub == NULL)
		return NFS4_URING_PREP_FALLBACK;

	uctx->req.obj = sub;
	uctx->req.state = NULL;
	/* Sentinel. The FSAL's prep() resolves the real descriptor; leaving
	 * this at the memset(0) value would name fd 0.
	 */
	uctx->req.fd = -1;
	uctx->req.openflags = FSAL_O_WRITE;
	uctx->req.bypass = true;
	/* Advisory; see the file header. */
	uctx->req.u.sync.offset = arg_COMMIT4->offset;
	uctx->req.u.sync.len = arg_COMMIT4->count;
	uctx->req.u.sync.flags = 0;

	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	desc = fsal_uring_fop_get(exp, uctx->req.fop);

	if (desc == NULL)
		return NFS4_URING_PREP_FALLBACK;

	st = desc->prep(&uctx->req, &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(st)) {
		/* prep() releases whatever it took before failing; this
		 * handler itself allocated nothing.
		 */
		LogDebug(COMPONENT_NFS_V4,
			 "uring commit: FSAL prep refused (%s), falling back",
			 fsal_err_txt(st));
		return NFS4_URING_PREP_FALLBACK;
	}

	return NFS4_URING_PREP_QUEUED;
}

/**
 * @brief Turn the SYNC CQE into an NFS4_OP_COMMIT response.
 *
 * Runs on a resume worker thread with a valid op_ctx, so it may take locks
 * and call back into the FSAL.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context
 * @param[out]    resp  Result slot for this operation
 * @param[in,out] uctx  Per-operation context
 *
 * @return NFS_REQ_OK or NFS_REQ_ERROR.
 */
enum nfs_req_result nfs4_op_commit_uring_finish(struct nfs_argop4 *op,
						compound_data_t *data,
						struct nfs_resop4 *resp,
						struct nfs4_uring_op_ctx *uctx)
{
	COMMIT4res *const res_COMMIT4 = &resp->nfs_resop4_u.opcommit;
	const struct fsal_uring_fop_desc *desc;
	struct fsal_export *exp;
	struct gsh_buffdesc verf_desc;

	resp->resop = NFS4_OP_COMMIT;

	/* The FSAL's own completion runs first: it turns the raw CQE into an
	 * fsal_status_t and releases the descriptor/state prep() acquired.
	 */
	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	desc = fsal_uring_fop_get(exp, uctx->req.fop);

	if (desc != NULL && desc->finish != NULL)
		(void)desc->finish(&uctx->req);

	/* This handler allocated no private state, but keep the contract. */
	if (uctx->priv != NULL) {
		gsh_free(uctx->priv, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
	}

	if (uctx->req.res < 0) {
		res_COMMIT4->status = nfs4_uring_cqe_to_nfsstat(uctx->req.res);

		LogDebug(COMPONENT_NFS_V4,
			 "uring commit failed, res = %" PRId32 ", status = %s",
			 uctx->req.res, nfsstat4_to_str(res_COMMIT4->status));

		return NFS_REQ_ERROR;
	}

	/* A commit can move ctime/mtime underneath MDCACHE, which never saw
	 * this fsync because it bypassed the stacked call chain.
	 */
	mdcache_uring_invalidate_attrs(data->current_obj);

	/* Exactly as nfs4_op_commit() does it: the write verifier comes from
	 * the export's get_write_verifier() method, written straight into the
	 * verifier4 in the response.
	 */
	verf_desc.addr = &res_COMMIT4->COMMIT4res_u.resok4.writeverf;
	verf_desc.len = sizeof(verifier4);

	op_ctx->fsal_export->exp_ops.get_write_verifier(op_ctx->fsal_export,
							&verf_desc);

	LogFullDebug(COMPONENT_NFS_V4, "uring commit verifier %d-%d",
		     ((int *)verf_desc.addr)[0], ((int *)verf_desc.addr)[1]);

	res_COMMIT4->status = NFS4_OK;

	return NFS_REQ_OK;
}

/**
 * @brief Release what prep took, for a chain that will never be submitted.
 *
 * Idempotent, and safe on a uctx whose prep never ran.
 *
 * @param[in,out] uctx  Per-operation context
 */
void nfs4_op_commit_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	const struct fsal_uring_fop_desc *desc;
	struct fsal_export *exp;

	exp = op_ctx != NULL ? op_ctx->fsal_export : NULL;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	desc = exp != NULL ? fsal_uring_fop_get(exp, uctx->req.fop) : NULL;

	if (desc != NULL && desc->release != NULL)
		desc->release(&uctx->req);

	if (uctx->priv != NULL) {
		gsh_free(uctx->priv, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
	}
}

#endif /* USE_FSAL_URING */
