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
 * @file  nfs4_op_read_uring.c
 * @brief NFS4_OP_READ planned into an io_uring chain.
 *
 * The split against nfs4_op_read.c is: everything that decides *whether* the
 * read may happen, and everything that formats the reply, is duplicated here
 * line for line from nfs4_read()/nfs4_complete_read(); only the "call
 * fsal_read2() and wait for the callback" middle is replaced by one SQE.
 * Validation is deliberately not factored out into a shared helper, because
 * nfs4_read() interleaves it with the synchronous I/O setup and cannot be
 * cut apart without touching the legacy path, which must stay bit-for-bit
 * identical when io_uring is off.
 *
 * Buffer ownership is the one place the two paths genuinely differ, and is
 * documented at nfs4_uring_read_io_data_release() below.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>

#include "abstract_atomic.h"
#include "abstract_mem.h"
#include "export_mgr.h"
#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_uring.h" /* fsal_uring_buf_get()/_put()/_release() */
#include "gsh_rpc.h"
#include "log.h"
#include "nfs_convert.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "sal_functions.h"
#include "server_stats.h"

/* src/FSAL is on the include path (see include_directories() in
 * src/CMakeLists.txt), src/FSAL/Stackable_FSALs is not, so the MDCACHE
 * io_uring helpers have to be reached through the Stackable_FSALs component
 * directory.
 */
#include "Stackable_FSALs/FSAL_MDCACHE/mdcache_int.h"

#include "nfs4_uring_internal.h"

/**
 * @brief What finish_cqe needs that prep_sqe already worked out.
 *
 * Mirrors the members of struct nfs4_read_data (nfs4_op_read.c) that
 * nfs4_complete_read() actually reads. The members that exist there only to
 * drive the fsal_read2() callback handshake -- flags, data, fsal_resume --
 * have no analogue here: the chain engine owns the async handshake.
 */
struct nfs4_uring_read_priv {
	/** Response being filled; same object as resp->nfs_resop4_u.opread. */
	READ4res *res_READ4;

	/** Owner reference taken for a v4.0 non-anonymous read, or NULL. */
	state_owner_t *owner;

	/** Reference returned by nfs4_Check_Stateid(), or NULL. */
	state_t *state;

	/** Stacked (MDCACHE) handle, for the post-read checks. */
	struct fsal_obj_handle *obj;

	/** Read buffer. NULL once handed on or freed. */
	void *buffer;

	/**
	 * Registered io_uring buffer backing @a buffer, or NULL when it came
	 * from the heap instead.
	 *
	 * Which of the two it is decides how the buffer is given back, so
	 * the two fields always move together: see
	 * nfs4_uring_read_drop_buffer().
	 */
	struct uring_buf *ubuf;

	/** Bytes asked of the FSAL, after MaxRead/MaxOffsetRead clamping. */
	uint64_t io_request;

	/** Offset the read was issued at. */
	uint64_t offset;

	/** data->op_resp_size this operation reserved during planning. */
	uint32_t resp_size;
};

/**
 * @brief Release the reply's read buffer.
 *
 * The synchronous path looks like it installs read4_io_data_release(), which
 * does nothing, but that is not where the buffer is freed. nfs4_read() hands
 * the FSAL an iovec with iov_base == NULL; fsal_read2() then allocates the
 * buffer and overwrites read_arg->iov_release/release_data with
 * fsal_iov_release()/the buffer, and nfs4_complete_read() copies that pair
 * over the placeholder into resok->data. So by the time the reply is encoded
 * the sync path really does carry a release that calls
 * gsh_free(buffer, MEM_COMP_IO_BUFFER).
 *
 * read4_io_data_release() is static in nfs4_op_read.c and is the wrong
 * function anyway, so this is the equivalent of the pair that actually ends
 * up installed. It is what xdr_io_data_uio_release() (RPCAL/rpc_tools.c)
 * invokes -- note that it only honours io_data::release when
 * io_data::release_data is also non-NULL, which is why both are always set
 * or both cleared together here.
 *
 * @param[in] release_data  The buffer, as stored in io_data::release_data
 */
static void nfs4_uring_read_io_data_release(void *release_data)
{
	gsh_free(release_data, MEM_COMP_IO_BUFFER);
}

/**
 * @brief Drop the read buffer before the reply layer ever sees it.
 *
 * Used on every path where resok is not going to be encoded with data in it:
 * an error status (the resok arm of the union is not encoded at all), a
 * zero-length read, or an abandoned chain. Clears the io_data release pair
 * as well, so a later teardown cannot free the same block twice.
 */
static void nfs4_uring_read_drop_buffer(struct nfs4_uring_read_priv *priv,
					READ4resok *resok)
{
	if (priv->buffer == NULL)
		return;

	if (priv->ubuf != NULL) {
		fsal_uring_buf_put(priv->ubuf);
		priv->ubuf = NULL;
	} else {
		gsh_free(priv->buffer, MEM_COMP_IO_BUFFER);
	}

	priv->buffer = NULL;

	resok->iov0.iov_base = NULL;
	resok->iov0.iov_len = 0;
	resok->data.release = NULL;
	resok->data.release_data = NULL;
}

/**
 * @brief The FSAL's READ handlers for the current export.
 *
 * data->current_obj normally lives in MDCACHE, which stacks on the storage
 * FSAL; the SQE layout is a property of the bottom export, so the stack has
 * to be walked down first.
 *
 * @return The descriptor, or NULL if this export cannot batch a READ.
 */
static const struct fsal_uring_fop_desc *
nfs4_uring_read_fop_desc(struct nfs4_uring_op_ctx *uctx)
{
	struct fsal_export *exp;

	if (op_ctx == NULL)
		return NULL;

	exp = op_ctx->fsal_export;

	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	if (exp == NULL)
		return NULL;

	return fsal_uring_fop_get(exp, uctx->req.fop);
}

/**
 * @brief Export access policy check, the copy of allow_read() from
 *        nfs4_op_read.c (where it is static).
 *
 * @param[in] obj     Object being read
 * @param[in] policy  ACCESS_CHECK_POLICY_PRE or ACCESS_CHECK_POLICY_POST
 */
static fsal_status_t nfs4_uring_allow_read(struct fsal_obj_handle *obj,
					   uint32_t policy)
{
	const uint32_t policies = atomic_fetch_uint32_t(
		&op_ctx->ctx_export->access_check_policies);
	const uint32_t read_policy =
		(policies >> READ_ACCESS_CHECK_POLICY_SHIFT) &
		ACCESS_CHECK_POLICY_ALL_MASK;
	fsal_status_t ret = { 0, 0 };

	if (read_policy & policy) {
		ret = obj->obj_ops->test_access(obj, FSAL_READ_ACCESS, NULL,
						NULL, true);

		if (ret.major == ERR_FSAL_ACCESS) {
			/* Test for execute permission */
			ret = fsal_access(
				obj, FSAL_MODE_MASK_SET(FSAL_X_OK) |
					     FSAL_ACE4_MASK_SET(
						     FSAL_ACE_PERM_EXECUTE));
		}
	}

	return ret;
}

/**
 * @brief Build-time reasons a READ can never be planned into a chain.
 *
 * Kept as a predicate rather than an #ifdef around the body so that the
 * whole handler is still compiled, and still type-checked, in every
 * configuration.
 */
static inline bool nfs4_uring_read_unbatchable(void)
{
#ifdef ENABLE_QOS
	/*
	 * QOS bandwidth control suspends the request before the I/O and
	 * resumes it from qos_process()'s own callback. The chain planner
	 * owns the suspend/resume handshake for a whole sub-batch and has no
	 * way to let one operation inside it suspend separately, so a QOS
	 * build never batches READ and the accounting stays exact.
	 */
	return true;
#else
	return false;
#endif
}

/**
 * @brief Plan NFS4_OP_READ into one chain slot.
 *
 * Runs on a worker thread with a valid op_ctx, before submission.
 *
 * @param[in]     op    READ arguments
 * @param[in,out] data  Compound context; current_obj is the file
 * @param[in,out] uctx  Per-operation context to fill in
 */
enum nfs4_uring_prep_result
nfs4_op_read_uring_prep(struct nfs_argop4 *op, compound_data_t *data,
			struct nfs4_uring_op_ctx *uctx)
{
	READ4args *const arg_READ4 = &op->nfs_argop4_u.opread;
	struct nfs_resop4 *resp = &data->resarray[uctx->oppos];
	READ4res *const res_READ4 = &resp->nfs_resop4_u.opread;
	READ4resok *resok = &res_READ4->READ4res_u.resok4;
	const struct fsal_uring_fop_desc *desc;
	struct nfs4_uring_read_priv *priv = NULL;
	struct fsal_obj_handle *obj;
	struct fsal_obj_handle *sub;
	state_t *state_found = NULL;
	state_t *state_open = NULL;
	state_owner_t *owner = NULL;
	fsal_status_t fsal_status;
	uint64_t MaxRead, MaxOffsetRead;
	uint64_t offset, size;
	uint32_t resp_size;
	void *buffer = NULL;
	struct uring_buf *ubuf = NULL;
	bool anonymous_started = false;
	bool bypass = false;
	enum nfs4_uring_prep_result result;

	if (nfs4_uring_read_unbatchable())
		return NFS4_URING_PREP_FALLBACK;

	/*
	 * READ_PLUS shares READ4args but overlays READ4res with READ_PLUS4res
	 * and needs a struct io_info the FSAL vector has no slot for. It is
	 * never reached today (optabv4_uring[] has no READ_PLUS row), but the
	 * check costs nothing and documents the restriction at the point it
	 * matters.
	 */
	if (data->opcode != NFS4_OP_READ)
		return NFS4_URING_PREP_FALLBACK;

	/* A pNFS DS read never reaches the FSAL object vector; it goes
	 * through op_ctx->ctx_pnfs_ds->s_ops.dsh_read() in op_dsread().
	 */
	if (data->minorversion > 0 && nfs4_Is_Fh_DSHandle(&data->currentFH))
		return NFS4_URING_PREP_FALLBACK;

	resp->resop = NFS4_OP_READ;
	res_READ4->status = NFS4_OK;

	/* Do basic checks on a filehandle Only files can be read */
	res_READ4->status = nfs4_sanity_check_FH(data, REGULAR_FILE, true);

	if (res_READ4->status != NFS4_OK)
		return NFS4_URING_PREP_ERROR;

	obj = data->current_obj;

	/* Check stateid correctness and get pointer to state (also
	 * checks for special stateids)
	 */
	res_READ4->status =
		nfs4_Check_Stateid(&arg_READ4->stateid, obj, &state_found, data,
				   STATEID_SPECIAL_ANY, 0, false, "READ");

	if (res_READ4->status != NFS4_OK) {
		/* Defensive: the sync path assumes a failed check yields no
		 * state, but the reference is ours either way.
		 */
		if (state_found != NULL)
			dec_state_t_ref(state_found);

		return NFS4_URING_PREP_ERROR;
	}

	/* NB: After this point, if state_found == NULL, then the
	 * stateid is all-0 or all-1
	 */

	if (state_found != NULL) {
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
				res_READ4->status = NFS4ERR_BAD_STATEID;
				result = NFS4_URING_PREP_ERROR;
				goto out;
			}
			break;

		case STATE_TYPE_DELEG:
			/* While doing read operation, we need not check
			 * for deleg state or share access. If share access
			 * is write or read or both, we will always allow
			 * reads as per page 112 in RFC 7530.
			 */
			state_open = NULL;
			break;

		default:
			res_READ4->status = NFS4ERR_BAD_STATEID;
			LogDebug(COMPONENT_STATE,
				 "READ with invalid statid of type %d",
				 state_found->state_type);
			result = NFS4_URING_PREP_ERROR;
			goto out;
		}

		/* This is a read operation, this means that the file
		 * MUST have been opened for reading
		 */
		if (state_open != NULL &&
		    (state_open->state_data.share.share_access &
		     OPEN4_SHARE_ACCESS_READ) == 0) {
			/* Even if file is open for write, the client
			 * may do accidentally read operation (caching).
			 * Because of this, READ is allowed if not
			 * explicitly denied.  See page 112 in RFC 7530
			 * for more details.
			 */
			if (state_open->state_data.share.share_deny &
			    OPEN4_SHARE_DENY_READ) {
				/* Bad open mode, return NFS4ERR_OPENMODE */
				res_READ4->status = NFS4ERR_OPENMODE;

				if (isDebug(COMPONENT_STATE)) {
					char str[LOG_BUFF_LEN] = "\0";
					struct display_buffer dspbuf = {
						sizeof(str), str, str
					};
					display_stateid(&dspbuf, state_found);
					LogDebug(
						COMPONENT_STATE,
						"READ %s doesn't have OPEN4_SHARE_ACCESS_READ",
						str);
				}

				result = NFS4_URING_PREP_ERROR;
				goto out;
			}
		}

		switch (state_found->state_type) {
		case STATE_TYPE_SHARE:
			if (!state_owner_confirmed(state_found)) {
				res_READ4->status = NFS4ERR_BAD_STATEID;
				result = NFS4_URING_PREP_ERROR;
				goto out;
			}
			break;
		case STATE_TYPE_LOCK:
		case STATE_TYPE_DELEG:
			break;
		default:
			/* Sanity check: all other types are illegal.
			 * we should not got that place (similar check
			 * above), anyway it costs nothing to add this
			 * test
			 */
			res_READ4->status = NFS4ERR_BAD_STATEID;
			result = NFS4_URING_PREP_ERROR;
			goto out;
		}
	} else {
		/* Special stateid, no open state, check to see if any share
		 * conflicts The stateid is all-0 or all-1
		 */
		state_open = NULL;

		bypass = arg_READ4->stateid.seqid != 0;

		/* Check for delegation conflict. */
		if (state_deleg_conflict(obj, false)) {
			res_READ4->status = NFS4ERR_DELAY;
			result = NFS4_URING_PREP_ERROR;
			goto out;
		}

		anonymous_started = true;
	}

	/* Perform permission checks before the FSAL does the read */
	fsal_status = nfs4_uring_allow_read(obj, ACCESS_CHECK_POLICY_PRE);

	if (FSAL_IS_ERROR(fsal_status)) {
		res_READ4->status = nfs4_Errno_status(fsal_status);
		result = NFS4_URING_PREP_ERROR;
		goto out;
	}

	MaxRead = atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxRead);
	MaxOffsetRead =
		atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxOffsetRead);

	/* Get the size and offset of the read operation */
	offset = arg_READ4->offset;
	size = arg_READ4->count;

	if (MaxOffsetRead < UINT64_MAX) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "Read offset=%" PRIu64 " size=%" PRIu64
			     " MaxOffSet=%" PRIu64,
			     offset, size, MaxOffsetRead);

		/* If the requested offset is at/after the maximum readable
		 * offset, treat as EOF per RFC 7530
		 * return NFS4_OK with 0 bytes and eof=TRUE.
		 */
		if (offset > MaxOffsetRead) {
			resok->eof = true;
			resok->data.data_len = 0;
			resok->data.iovcnt = 1;
			resok->data.iov = &resok->iov0;
			resok->iov0.iov_len = 0;
			resok->iov0.iov_base = NULL;
			res_READ4->status = NFS4_OK;
			result = NFS4_URING_PREP_INLINE;
			goto out;
		}
		/* Clamp size if the request would run past MaxOffsetRead */
		else if ((offset + size) > MaxOffsetRead) {
			size = MaxOffsetRead - offset;
		}
	}

	if (size > MaxRead) {
		/* the client asked for too much data, this should normally
		 * not happen because client will get FATTR4_MAXREAD value
		 * at mount time
		 */
		LogFullDebug(COMPONENT_NFS_V4,
			     "read requested size = %" PRIu64
			     " read allowed size = %" PRIu64,
			     size, MaxRead);
		size = MaxRead;
	}

	/* Now check response size.
	 * size + space for nfsstat4, eof, and data len
	 */
	resp_size = RNDUP(size) + sizeof(nfsstat4) + 2 * sizeof(uint32_t);

	res_READ4->status = check_resp_room(data, resp_size);

	if (res_READ4->status != NFS4_OK) {
		result = NFS4_URING_PREP_ERROR;
		goto out;
	}

	data->op_resp_size = resp_size;

	/* If size == 0, no I/O is to be made and everything is alright. No
	 * SQE is emitted and finish_cqe will not run for this operation, so
	 * the response is complete as it stands.
	 */
	if (size == 0) {
		/* A size = 0 can not lead to EOF */
		resok->eof = false;
		resok->data.data_len = 0;
		resok->data.iovcnt = 1;
		resok->data.iov = &resok->iov0;
		resok->iov0.iov_len = 0;
		resok->iov0.iov_base = NULL;
		res_READ4->status = NFS4_OK;
		result = NFS4_URING_PREP_INLINE;
		goto out;
	}

	/* The SQE acts on the storage handle, not on the MDCACHE wrapper. */
	sub = mdcache_uring_sub_handle(obj);

	if (sub == NULL) {
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	/* Some work is to be done */
	if (!anonymous_started && data->minorversion == 0) {
		owner = get_state_owner_ref(state_found);

		if (owner != NULL) {
			op_ctx->clientid =
				&owner->so_owner.so_nfs4_owner.so_clientid;
		}
	}

	/*
	 * Buffer. The synchronous path leaves iov[0].iov_base NULL and lets
	 * fsal_read2() negotiate the allocation (possibly an RDMA buffer, or
	 * none at all for a zero-copy FSAL). There is no such negotiation on
	 * the uring path -- the SQE has to name a destination at submit time
	 * -- so it is settled here and owned explicitly.
	 *
	 * Prefer a registered buffer from the engine's pool. Two reasons,
	 * and the second one is the bigger:
	 *
	 *  - the SQE can then be IORING_OP_READ_FIXED into already-pinned
	 *    memory, so the kernel skips the per-I/O page lookup;
	 *
	 *  - it takes the allocator off the data path entirely. This buffer
	 *    is allocated on the worker that plans the chain and released on
	 *    whichever worker encodes the reply, and a malloc/free pair that
	 *    straddles two threads defeats jemalloc's thread cache: the free
	 *    goes back to the owning arena, dirty pages accumulate, and
	 *    jemalloc periodically purges them with madvise(MADV_DONTNEED).
	 *    Each purge is a zap_page_range_single() plus a TLB shootdown
	 *    IPI to every CPU, which on a KVM guest is a vmexit per CPU.
	 *    Profiling a 4K read workload put 9.4% of all CPU in __madvise
	 *    and 7.6% in flush_tlb_mm_range -- a cost the synchronous path
	 *    does not pay, because there the malloc and the free happen on
	 *    the same thread and never leave the tcache.
	 *
	 * The pool is sized and pinned at startup, so get/put is a per-class
	 * mutex and a stack push. A miss (pool exhausted, or a read larger
	 * than the biggest size class) falls back to the heap, which is
	 * correct, just slower -- and if it happens often, io_uring_pool_mb
	 * or io_uring_buffer_max wants raising.
	 *
	 * last_iov_buf_size stays 0 either way, which makes
	 * xdr_io_data_encode() use its own 4 byte tail buffer for the XDR
	 * round-up rather than reading past the payload.
	 */
	ubuf = fsal_uring_buf_get(uctx->batch->engine, size);

	if (ubuf != NULL) {
		buffer = ubuf->addr;
		resok->data.release = fsal_uring_buf_release;
		resok->data.release_data = ubuf;
	} else {
		buffer = gsh_malloc(size, MEM_COMP_IO_BUFFER);
		resok->data.release = nfs4_uring_read_io_data_release;
		resok->data.release_data = buffer;
	}

	resok->data.data_len = size;
	resok->data.iovcnt = 1;
	resok->data.iov = &resok->iov0;
	resok->data.last_iov_buf_size = 0;
	resok->iov0.iov_base = buffer;
	resok->iov0.iov_len = size;

	priv = gsh_malloc(sizeof(*priv), MEM_COMP_PROTOCOL);
	priv->res_READ4 = res_READ4;
	priv->owner = owner;
	priv->state = state_found;
	priv->obj = obj;
	priv->buffer = buffer;
	priv->ubuf = ubuf;
	priv->io_request = size;
	priv->offset = offset;
	priv->resp_size = resp_size;

	uctx->priv = priv;

	uctx->req.obj = sub;
	uctx->req.state = state_found;
	uctx->req.openflags = FSAL_O_READ;
	uctx->req.bypass = bypass;
	/* Not a valid descriptor: the FSAL's prep() resolves one, and an FSAL
	 * that does not must not be handed fd 0 by accident.
	 */
	uctx->req.fd = -1;
	uctx->req.fsal_priv = NULL;
	uctx->req.u.rw.iov = resok->data.iov;
	uctx->req.u.rw.iovcnt = 1;
	uctx->req.u.rw.offset = offset;
	/* Non-NULL selects IORING_OP_READ_FIXED; vfs_uring_rw_fixed() also
	 * requires iovcnt == 1, which it is.
	 */
	uctx->req.u.rw.buf = ubuf;
	uctx->req.u.rw.amount = 0;
	uctx->req.u.rw.eof = false;

	desc = nfs4_uring_read_fop_desc(uctx);

	if (desc == NULL || desc->prep == NULL) {
		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	fsal_status =
		desc->prep(&uctx->req, &uctx->batch->chain.ops[uctx->slot]);

	if (FSAL_IS_ERROR(fsal_status)) {
		LogFullDebug(COMPONENT_NFS_V4,
			     "uring READ: FSAL prep failed (%s), not batching",
			     fsal_err_txt(fsal_status));

		if (desc->release != NULL)
			desc->release(&uctx->req);

		result = NFS4_URING_PREP_FALLBACK;
		goto out;
	}

	/* uctx->nr_slots is already 1, which is all a READ needs. */
	result = NFS4_URING_PREP_QUEUED;

out:

	if (state_open != NULL)
		dec_state_t_ref(state_open);

	if (result == NFS4_URING_PREP_QUEUED)
		return result;

	/*
	 * Nothing will be submitted for this operation and finish_cqe will
	 * not run, so everything taken above has to go back now. For a
	 * fallback the synchronous handler re-acquires all of it from
	 * scratch, so leaving any of it held would double-count.
	 */
	if (priv != NULL) {
		nfs4_uring_read_drop_buffer(priv, resok);
		gsh_free(priv, MEM_COMP_PROTOCOL);
		uctx->priv = NULL;
	} else if (buffer != NULL) {
		if (ubuf != NULL)
			fsal_uring_buf_put(ubuf);
		else
			gsh_free(buffer, MEM_COMP_IO_BUFFER);

		resok->iov0.iov_base = NULL;
		resok->data.release = NULL;
		resok->data.release_data = NULL;
	}

	if (owner != NULL) {
		op_ctx->clientid = NULL;
		dec_state_owner_ref(owner);
	}

	/*
	 * nfs4_Check_Stateid() returns a reference. nfs4_read() only drops it
	 * from nfs4_complete_read(), i.e. not on its own early-out paths;
	 * here every path that does not queue an SQE drops it, because
	 * nothing downstream will.
	 */
	if (state_found != NULL)
		dec_state_t_ref(state_found);

	uctx->req.obj = NULL;
	uctx->req.state = NULL;

	return result;
}

/**
 * @brief Turn the READ CQE into the READ4res.
 *
 * Runs on a resume worker thread with a valid op_ctx, so it may take locks
 * and call back into the FSAL.
 *
 * @param[in]     op    READ arguments
 * @param[in,out] data  Compound context
 * @param[out]    resp  Response to fill in
 * @param[in,out] uctx  Per-operation context from prep
 */
enum nfs_req_result nfs4_op_read_uring_finish(struct nfs_argop4 *op,
					      compound_data_t *data,
					      struct nfs_resop4 *resp,
					      struct nfs4_uring_op_ctx *uctx)
{
	struct nfs4_uring_read_priv *priv = uctx->priv;
	READ4res *const res_READ4 = priv->res_READ4;
	READ4resok *resok = &res_READ4->READ4res_u.resok4;
	const struct fsal_uring_fop_desc *desc;
	fsal_status_t status = { 0, 0 };
	uint64_t amount = 0;
	bool eof = false;

	resp->resop = NFS4_OP_READ;

	/*
	 * Every operation in the sub-batch ran nfs4_op_precheck() over the
	 * single data->op_resp_size field while planning, so the value this
	 * operation reserved has to be put back before complete_op() tallies
	 * it. nfs4_read() sets the same value, at the same point relative to
	 * the I/O.
	 */
	data->op_resp_size = priv->resp_size;

	desc = nfs4_uring_read_fop_desc(uctx);

	if (desc == NULL || desc->finish == NULL) {
		/* The export's vector cannot have gone away under a request
		 * holding an export reference, but failing closed costs
		 * nothing and keeps the FSAL state release unconditional.
		 */
		LogCrit(COMPONENT_NFS_V4,
			"uring READ: no FSAL completion handler, failing op");
		res_READ4->status = NFS4ERR_SERVERFAULT;
	} else {
		/*
		 * The FSAL completion runs first: it is what releases the
		 * descriptor and the share reservation that its prep()
		 * acquired, and what turns the raw CQE result into an
		 * fsal_status_t.
		 */
		status = desc->finish(&uctx->req);

		if (FSAL_IS_ERROR(status)) {
			res_READ4->status =
				nfs4_uring_cqe_to_nfsstat(uctx->req.res);

			/* A non-negative CQE with an FSAL error is not
			 * expected; do not report NFS4_OK for it.
			 */
			if (res_READ4->status == NFS4_OK)
				res_READ4->status = nfs4_Errno_status(status);
		} else {
			amount = uctx->req.u.rw.amount;
			eof = uctx->req.u.rw.eof;

			/* Same post-read permission check nfs4_read_cb()
			 * applies on the synchronous path.
			 */
			status =
				nfs4_uring_allow_read(priv->obj,
						      ACCESS_CHECK_POLICY_POST);
			res_READ4->status = nfs4_Errno_status(status);
		}
	}

	if (res_READ4->status == NFS4_OK) {
		if (nfs_param.core_param.getattrs_in_complete_read && !eof) {
			/*
			 * NFS requires to set the EOF flag for all reads that
			 * reach the EOF, i.e., even the ones returning data.
			 * Most FSALs don't set the flag in this case. The only
			 * client that cares about this is ESXi. Other clients
			 * will just see a short read and continue reading and
			 * then get the EOF flag as 0 bytes are returned.
			 */
			struct fsal_attrlist attrs;
			fsal_status_t attr_status;

			fsal_prepare_attrs(&attrs, ATTR_SIZE);
			attr_status =
				priv->obj->obj_ops->getattrs(priv->obj, &attrs);

			if (FSAL_IS_SUCCESS(attr_status)) {
				eof = (priv->offset + amount) >= attrs.filesize;
			}

			/* Done with the attrs */
			fsal_release_attrs(&attrs);
		}

		/* Is EOF met or not ? */
		resok->eof = eof;
		resok->data.data_len = amount;

		if (amount == 0) {
			/* Nothing was transferred, so there is no point
			 * carrying the buffer all the way to the encoder.
			 * Keep the length 1 iovec, with a zero length.
			 */
			nfs4_uring_read_drop_buffer(priv, resok);
			resok->data.iov[0].iov_len = 0;
		} else {
			/* The buffer now belongs to the reply, and is given
			 * back by whichever release io_data carries once the
			 * encoded reply is freed: fsal_uring_buf_release()
			 * for a pooled buffer, nfs4_uring_read_io_data_release()
			 * for a heap one. Drop both of our handles on it so
			 * that a later nfs4_uring_read_drop_buffer() cannot
			 * release it a second time.
			 */
			priv->buffer = NULL;
			priv->ubuf = NULL;
		}

		LogFullDebug(COMPONENT_NFS_V4,
			     "NFS4_OP_READ: offset = %" PRIu64
			     " read length = %" PRIu64 " eof=%u",
			     priv->offset, amount, (unsigned int)eof);

		/* The ring read the file behind MDCACHE's back. */
		mdcache_uring_touch_atime(data->current_obj);
	} else {
		/* The resok arm of the union is not encoded for a failed
		 * READ, so nothing else will ever free the buffer.
		 */
		nfs4_uring_read_drop_buffer(priv, resok);
	}

	server_stats_io_done(priv->io_request, amount,
			     res_READ4->status == NFS4_OK, false);

	if (priv->owner != NULL) {
		op_ctx->clientid = NULL;
		dec_state_owner_ref(priv->owner);
		priv->owner = NULL;
	}

	if (priv->state != NULL) {
		dec_state_t_ref(priv->state);
		priv->state = NULL;
	}

	uctx->req.obj = NULL;
	uctx->req.state = NULL;

	gsh_free(priv, MEM_COMP_PROTOCOL);
	uctx->priv = NULL;

	return nfsstat4_to_nfs_req_result(res_READ4->status);
}

/**
 * @brief Release what prep planned, for a chain that is never submitted.
 *
 * finish_cqe does all of this on the normal path, so this only ever runs
 * when no CQE will arrive: a later operation in the same sub-batch failed to
 * plan, or the submission itself failed. In the latter case the operation is
 * about to be re-run through the synchronous handler, which rebuilds the
 * response from scratch, so the response fields are reset too.
 *
 * Safe to call on a uctx whose prep failed part way, and safe to call once.
 *
 * @param[in,out] uctx  Per-operation context
 */
void nfs4_op_read_uring_abort(struct nfs4_uring_op_ctx *uctx)
{
	struct nfs4_uring_read_priv *priv = uctx->priv;
	const struct fsal_uring_fop_desc *desc;

	/* FSAL state first: its prep() may be holding a descriptor and a
	 * share reservation, which only its release() knows how to undo.
	 */
	desc = nfs4_uring_read_fop_desc(uctx);

	if (desc != NULL && desc->release != NULL)
		desc->release(&uctx->req);

	uctx->req.obj = NULL;
	uctx->req.state = NULL;

	if (priv == NULL)
		return;

	if (priv->res_READ4 != NULL) {
		READ4resok *resok = &priv->res_READ4->READ4res_u.resok4;

		nfs4_uring_read_drop_buffer(priv, resok);

		/*
		 * Leave a well formed zero length result behind rather than a
		 * NULL iovec. On the submit-failure path the synchronous
		 * handler overwrites all of this; on the planning-failure
		 * path it does not run at all, and the compound is truncated
		 * at the operation that failed, which is *after* this one --
		 * so this response is still encoded.
		 */
		resok->eof = false;
		resok->data.data_len = 0;
		resok->data.iovcnt = 1;
		resok->data.iov = &resok->iov0;
	}

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

	gsh_free(priv, MEM_COMP_PROTOCOL);
	uctx->priv = NULL;
}

#endif /* USE_FSAL_URING */
