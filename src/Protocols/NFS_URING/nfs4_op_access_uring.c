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
 * @file  nfs4_op_access_uring.c
 * @brief io_uring batched NFS4_OP_ACCESS.
 *
 * ACCESS is a permission decision, so this file is written to be a provably
 * exact replay of the synchronous path rather than an approximation.  The
 * synchronous path is:
 *
 *   nfs4_op_access()            Protocols/NFS/nfs4_op_access.c
 *     -> nfs_access_op()        support/nfs_creds.c
 *          builds a mode+ACE4 access_mask and a granted_mask from the
 *          requested ACCESS4_* bits and the object type,
 *        -> obj->obj_ops->test_access()   == mdcache_test_access()
 *             -> fsal_test_access()       FSAL/access_check.c
 *                  getattrs(ATTRS_CREDS | ATTR_MODE [| ATTR_ACL])
 *                  -> fsal_check_access_acl()     if attrs.acl != NULL
 *                  -> fsal_check_access_no_acl()  otherwise
 *          maps the returned `allowed` flags back onto ACCESS4_* bits.
 *
 * A statx(2) yields POSIX mode, uid, gid and file type and nothing else, so
 * it can reproduce fsal_check_access_no_acl() exactly and cannot reproduce
 * fsal_check_access_acl() at all.  uring_access_needs_acl_eval() below is the
 * guard that keeps us out of the second case; everything after it mirrors
 * nfs_access_op() plus fsal_check_access_no_acl() line for line.
 *
 * Mapping onto the storage layer is FSAL_URING_FOP_STATX, which the planner
 * has already stamped into uctx->req.fop before prep runs.  Note that
 * ACCESS4_* and ACCESS3_* have identical numeric values, so the ACCESS4_*
 * spellings used here are the same bits nfs_access_op() tests as ACCESS3_*.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <linux/stat.h> /* for struct statx / STATX_* */

#include "abstract_mem.h"
#include "fsal.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_exports.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"

#include "nfs4_uring_internal.h"

/** Attributes a statx must return for the decision below to be valid. */
#define URING_ACCESS_STATX_MASK \
	(STATX_MODE | STATX_UID | STATX_GID | STATX_TYPE)

/**
 * @brief Could an NFSv4 ACL take part in this export's access decision?
 *
 * fsal_test_access() asks for ATTR_ACL only when the export's FSAL claims to
 * support it:
 *
 *     fsal_prepare_attrs(&attrs,
 *         op_ctx->fsal_export->exp_ops.fs_supported_attrs(op_ctx->fsal_export)
 *         & (ATTRS_CREDS | ATTR_MODE | ATTR_ACL));
 *
 * and it then takes the ACL branch only when the resulting attrs.acl is
 * non-NULL.  So if ATTR_ACL is not in fs_supported_attrs, attrs.acl is
 * necessarily NULL and the mode-only branch is guaranteed.  Testing the exact
 * same expression fsal_test_access() tests is what makes that guarantee hold
 * for every FSAL rather than just for the ones we happen to have read.
 *
 * Deliberately *not* consulted: EXPORT_OPTION_DISABLE_ACL.  An FSAL may
 * interpret that flag as "do not expose ACLs to the client" while still
 * evaluating them internally, so using it to permit batching would be a
 * guess.  Refusing on fs_supported_attrs alone costs us ACL-capable exports
 * and nothing else — plain FSAL_VFS does not report ATTR_ACL unless it is
 * built with ENABLE_VFS_ACL.
 *
 * ENABLE_RFC_ACL is belt and braces.  Its live effects are all inside
 * fsal_check_access_acl() (and fsal_remove_access()), which this path never
 * reaches, but it is a build-wide "ACL semantics are in force" switch and a
 * permission check is the wrong place to be clever.
 *
 * @return true when the operation must not be batched.
 */
static bool uring_access_needs_acl_eval(void)
{
#ifdef ENABLE_RFC_ACL
	return true;
#else
	attrmask_t supported;

	if (op_ctx == NULL || op_ctx->fsal_export == NULL)
		return true;

	supported = op_ctx->fsal_export->exp_ops.fs_supported_attrs(
		op_ctx->fsal_export);

	return (supported & ATTR_ACL) != 0;
#endif /* ENABLE_RFC_ACL */
}

/**
 * @brief Build the mode-only access mask and the granted mask.
 *
 * This is the first half of nfs_access_op(), with the FSAL_ACE_PERM_* bits
 * dropped: fsal_test_access() passes FSAL_MODE_MASK(access_type) — that is,
 * access_type & FSAL_MODE_BIT_MASK, only FSAL_R_OK/W_OK/X_OK — into
 * fsal_check_access_no_acl(), so the ACE4 half of the mask is dead on the
 * mode-only branch and reproducing it would only invite divergence.
 *
 * @param[in]  requested_access  ACCESS4_* bits the client asked about
 * @param[in]  is_dir            Object type is DIRECTORY
 * @param[out] granted_mask_out  Bits that are meaningful for this file type
 *
 * @return The FSAL_{R,W,X}_OK mask to test, or 0 if nothing need be tested.
 */
static fsal_accessflags_t uring_access_build_mask(uint32_t requested_access,
						  bool is_dir,
						  uint32_t *granted_mask_out)
{
	fsal_accessflags_t access_mask = 0;
	uint32_t granted_mask = requested_access;

	if (requested_access & (ACCESS4_READ | ACCESS4_XAREAD | ACCESS4_XALIST))
		access_mask |= FSAL_R_OK;

	if (requested_access & ACCESS4_LOOKUP) {
		if (is_dir)
			access_mask |= FSAL_X_OK;
		else
			granted_mask &= ~ACCESS4_LOOKUP;
	}

	/* MODIFY and XAWRITE want write permission for both file types; only
	 * the ACE4 half of nfs_access_op() distinguishes them.
	 */
	if (requested_access & (ACCESS4_MODIFY | ACCESS4_XAWRITE))
		access_mask |= FSAL_W_OK;

	if (requested_access & ACCESS4_EXTEND)
		access_mask |= FSAL_W_OK;

	if (requested_access & ACCESS4_DELETE) {
		if (is_dir)
			access_mask |= FSAL_W_OK;
		else
			granted_mask &= ~ACCESS4_DELETE;
	}

	if (requested_access & ACCESS4_EXECUTE) {
		if (!is_dir)
			access_mask |= FSAL_X_OK;
		else
			granted_mask &= ~ACCESS4_EXECUTE;
	}

	*granted_mask_out = granted_mask;

	return access_mask;
}

/**
 * @brief Replay fsal_check_access_no_acl() against a statx result.
 *
 * @param[in] stx          Kernel-filled statx of the object
 * @param[in] access_mask  FSAL_{R,W,X}_OK bits to test
 *
 * @return The subset of @access_mask that is allowed.
 */
static fsal_accessflags_t uring_access_allowed(const struct statx *stx,
					       fsal_accessflags_t access_mask)
{
	const struct user_cred *creds = &op_ctx->creds;
	uint32_t mode = stx->stx_mode;
	fsal_accessflags_t mask;
	unsigned int i;

	/* Root handling, verbatim from fsal_check_access_no_acl(). Note it
	 * uses the *attribute* type, which here is the statx type, not the
	 * (identical) fsal_obj_handle type.
	 */
	if (op_ctx->fsal_export->exp_ops.is_superuser(op_ctx->fsal_export,
						      creds)) {
		if (S_ISDIR(stx->stx_mode))
			return access_mask;

		/* Root gets execute only if somebody does. */
		if ((access_mask & FSAL_X_OK) == 0 ||
		    (stx->stx_mode & (S_IXOTH | S_IXUSR | S_IXGRP)) != 0)
			return access_mask;

		return access_mask & ~FSAL_X_OK;
	}

	/* Owner bits win, then primary group, then any supplementary group,
	 * then "other" falls out of the bottom with no shift at all.
	 */
	if (creds->caller_uid == stx->stx_uid) {
		mode >>= 6;
	} else if (creds->caller_gid == stx->stx_gid) {
		mode >>= 3;
	} else {
		for (i = 0; i < creds->caller_glen; i++) {
			if (creds->caller_garray[i] == stx->stx_gid) {
				mode >>= 3;
				break;
			}
		}
	}

	mask = ((mode & S_IROTH) ? FSAL_R_OK : 0) |
	       ((mode & S_IWOTH) ? FSAL_W_OK : 0) |
	       ((mode & S_IXOTH) ? FSAL_X_OK : 0);

	return mask & access_mask;
}

/**
 * @brief Plan an NFS4_OP_ACCESS into the chain.
 *
 * Runs on a worker thread with a valid op_ctx, before submission.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context; current_obj is the target
 * @param[in,out] uctx  Per-operation context to fill in
 *
 * @return One of enum nfs4_uring_prep_result.
 */
enum nfs4_uring_prep_result
nfs4_op_access_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			  struct nfs4_uring_op_ctx *uctx)
{
	ACCESS4args *const arg_ACCESS4 = &op->nfs_argop4_u.opaccess;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	ACCESS4res *const res_ACCESS4 = &resp->nfs_resop4_u.opaccess;
	uint32_t max_access = (ACCESS4_READ | ACCESS4_LOOKUP | ACCESS4_MODIFY |
			       ACCESS4_EXTEND | ACCESS4_DELETE |
			       ACCESS4_EXECUTE);
	uint32_t granted_mask;
	fsal_accessflags_t access_mask;
	struct fsal_obj_handle *sub;
	struct fsal_export *exp;
	const struct fsal_uring_fop_desc *desc;
	struct statx *stx;
	fsal_status_t st;

	/* xattrs are a v4.2+ feature. */
	if (data->minorversion >= 2)
		max_access |= ACCESS4_XAREAD | ACCESS4_XAWRITE | ACCESS4_XALIST;

	res_ACCESS4->ACCESS4res_u.resok4.supported = 0;
	res_ACCESS4->ACCESS4res_u.resok4.access = 0;
	resp->resop = NFS4_OP_ACCESS;
	res_ACCESS4->status = NFS4_OK;

	res_ACCESS4->status = nfs4_sanity_check_FH(data, NO_FILE_TYPE, false);

	if (res_ACCESS4->status != NFS4_OK)
		return NFS4_URING_PREP_ERROR;

	if (arg_ACCESS4->access > max_access) {
		res_ACCESS4->status = NFS4ERR_INVAL;
		return NFS4_URING_PREP_ERROR;
	}

	/* A statx cannot decide an ACL. */
	if (uring_access_needs_acl_eval()) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring access: ACLs in play, not batching");
		return NFS4_URING_PREP_FALLBACK;
	}

	access_mask = uring_access_build_mask(arg_ACCESS4->access,
					      data->current_obj->type ==
						      DIRECTORY,
					      &granted_mask);

	/* fsal_test_access() short-circuits an empty mask before it even
	 * calls getattrs(), so the synchronous answer in this case cannot
	 * fail and does no I/O. Rather than reproduce that asymmetry (a
	 * failed statx would turn an unconditional NFS4_OK into an error),
	 * hand the degenerate case to the synchronous handler.
	 */
	if (access_mask == 0)
		return NFS4_URING_PREP_FALLBACK;

	sub = mdcache_uring_sub_handle(data->current_obj);

	if (sub == NULL)
		return NFS4_URING_PREP_FALLBACK;

	/* The kernel writes into this after prep returns, so it must outlive
	 * the stack frame; finish/abort release it.
	 */
	stx = gsh_malloc(sizeof(struct statx), MEM_COMP_PROTOCOL);
	memset(stx, 0, sizeof(*stx));

	uctx->priv = stx;

	uctx->req.obj = sub;
	uctx->req.state = NULL;
	/* Sentinel. The FSAL's prep() resolves the real descriptor; leaving
	 * this at the memset(0) value would name fd 0.
	 */
	uctx->req.fd = -1;
	uctx->req.openflags = FSAL_O_READ;
	uctx->req.bypass = true;
	uctx->req.u.statx.mask = URING_ACCESS_STATX_MASK;
	uctx->req.u.statx.stx = stx;

	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	desc = fsal_uring_fop_get(exp, uctx->req.fop);

	if (desc == NULL) {
		gsh_free(stx, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
		uctx->req.u.statx.stx = NULL;
		return NFS4_URING_PREP_FALLBACK;
	}

	st = desc->prep(&uctx->req, &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(st)) {
		LogDebug(COMPONENT_NFS_V4,
			 "uring access: FSAL prep refused (%s), falling back",
			 fsal_err_txt(st));
		gsh_free(stx, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
		uctx->req.u.statx.stx = NULL;
		return NFS4_URING_PREP_FALLBACK;
	}

	return NFS4_URING_PREP_QUEUED;
}

/**
 * @brief Turn the STATX CQE into an NFS4_OP_ACCESS response.
 *
 * Runs on a resume worker thread with a valid op_ctx.
 *
 * @param[in]     op    Argument union for this operation
 * @param[in,out] data  Compound context
 * @param[out]    resp  Result slot for this operation
 * @param[in,out] uctx  Per-operation context
 *
 * @return NFS_REQ_OK or NFS_REQ_ERROR.
 */
enum nfs_req_result nfs4_op_access_uring_finish(struct nfs_argop4 *op,
						compound_data_t *data,
						struct nfs_resop4 *resp,
						struct nfs4_uring_op_ctx *uctx)
{
	ACCESS4args *const arg_ACCESS4 = &op->nfs_argop4_u.opaccess;
	ACCESS4res *const res_ACCESS4 = &resp->nfs_resop4_u.opaccess;
	struct statx *stx = uctx->priv;
	const struct fsal_uring_fop_desc *desc;
	struct fsal_export *exp;
	fsal_accessflags_t access_mask;
	fsal_accessflags_t allowed;
	uint32_t granted_mask;
	uint32_t granted = 0;

	resp->resop = NFS4_OP_ACCESS;
	res_ACCESS4->ACCESS4res_u.resok4.supported = 0;
	res_ACCESS4->ACCESS4res_u.resok4.access = 0;

	/* The FSAL's own completion runs first: it turns the raw CQE into an
	 * fsal_status_t and releases the descriptor/state prep() acquired.
	 */
	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	desc = fsal_uring_fop_get(exp, uctx->req.fop);

	if (desc != NULL && desc->finish != NULL)
		(void)desc->finish(&uctx->req);

	if (uctx->req.res < 0) {
		res_ACCESS4->status = nfs4_uring_cqe_to_nfsstat(uctx->req.res);

		LogDebug(COMPONENT_NFS_V4,
			 "uring access failed, res = %" PRId32 ", status = %s",
			 uctx->req.res, nfsstat4_to_str(res_ACCESS4->status));

		goto out;
	}

	if (stx == NULL || (stx->stx_mask & URING_ACCESS_STATX_MASK) !=
				   URING_ACCESS_STATX_MASK) {
		/* No mode/uid/gid means no decision. Refusing is the only
		 * safe answer; granting anything here would be a guess.
		 */
		LogWarn(COMPONENT_NFS_V4,
			"uring access: statx returned mask 0x%x, need 0x%x",
			stx != NULL ? stx->stx_mask : 0,
			(unsigned int)URING_ACCESS_STATX_MASK);

		res_ACCESS4->status = NFS4ERR_IO;
		goto out;
	}

	/* uctx->req.obj is the storage handle prep() resolved from
	 * data->current_obj, and a stacked handle always mirrors its sub
	 * handle's type, so this is the same value nfs_access_op() would read
	 * off data->current_obj — but tied to the object this statx actually
	 * described rather than to wherever the compound's current filehandle
	 * has got to by now.
	 */
	access_mask = uring_access_build_mask(arg_ACCESS4->access,
					      uctx->req.obj->type == DIRECTORY,
					      &granted_mask);

	allowed = uring_access_allowed(stx, access_mask);

	/* Second half of nfs_access_op(). Only the mode branches can fire:
	 * fsal_check_access_no_acl() never sets an FSAL_ACE_PERM_* bit in
	 * `allowed`, so the ACE4 branches of nfs_access_op() are unreachable
	 * here and are omitted rather than written as dead code.
	 */
	if (allowed & FSAL_R_OK)
		granted |= ACCESS4_READ | ACCESS4_XAREAD | ACCESS4_XALIST;

	if (allowed & FSAL_W_OK)
		granted |= ACCESS4_MODIFY | ACCESS4_EXTEND | ACCESS4_DELETE |
			   ACCESS4_XAWRITE;

	if (allowed & FSAL_X_OK)
		granted |= ACCESS4_LOOKUP | ACCESS4_EXECUTE;

	/* Allow only read if the client has read-only access on this share. */
	if (!(op_ctx->export_perms.options & EXPORT_OPTION_WRITE_ACCESS))
		granted &= ~(ACCESS4_EXTEND | ACCESS4_MODIFY | ACCESS4_DELETE |
			     ACCESS4_XAWRITE);

	/* Don't allow any bits that weren't requested or that the file type
	 * does not support.
	 */
	granted &= granted_mask;

	res_ACCESS4->ACCESS4res_u.resok4.access = granted;
	res_ACCESS4->ACCESS4res_u.resok4.supported = granted_mask;

	/* nfs4_op_access() maps both ERR_FSAL_NO_ERROR and ERR_FSAL_ACCESS to
	 * NFS4_OK — a denial is reported in the access bitmask, not in the
	 * operation status — and those are the only two outcomes
	 * fsal_check_access_no_acl() can produce once the attributes are in
	 * hand. So there is nothing left to map.
	 */
	res_ACCESS4->status = NFS4_OK;

	LogFullDebug(COMPONENT_NFS_V4,
		     "uring access: requested=0x%x supported=0x%x access=0x%x",
		     arg_ACCESS4->access, granted_mask, granted);

out:

	if (uctx->priv != NULL) {
		gsh_free(uctx->priv, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
	}

	uctx->req.u.statx.stx = NULL;

	return res_ACCESS4->status == NFS4_OK ? NFS_REQ_OK : NFS_REQ_ERROR;
}

/**
 * @brief Release what prep took, for a chain that will never be submitted.
 *
 * Idempotent, and safe on a uctx whose prep never ran.
 *
 * @param[in,out] uctx  Per-operation context
 */
void nfs4_op_access_uring_abort(struct nfs4_uring_op_ctx *uctx)
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

	uctx->req.u.statx.stx = NULL;
}

#endif /* USE_FSAL_URING */
