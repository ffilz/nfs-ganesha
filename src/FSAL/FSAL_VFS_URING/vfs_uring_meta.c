// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2026, IBM. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/**
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_meta.c
 * @brief STATX, CLOSE and FALLOCATE handlers of the FSAL_VFS_URING vector.
 *
 * Same shape as vfs_uring_io.c: a prep() that acquires the descriptor and
 * wires the chain slot, an sqe_pre callback that owns the SQE layout, and a
 * finish() that gives the descriptor back.
 *
 * CLOSE is the exception and is refused; see vfs_uring_close_prep() for the
 * reasoning.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <linux/stat.h>

/* liburing.h before any Ganesha header; see vfs_uring_io.c. */
#include <liburing.h>
#undef UNUSED

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
/* uring_pers_attach() is a static inline; see vfs_uring_io.c. */
#include "../FSAL_URING/uring_pers.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/* -------------------------------------------------------------------------
 * sqe_pre callbacks -- run inside the engine's submit_lock
 * -------------------------------------------------------------------------
 */

/**
 * @brief Fill a STATX SQE.
 *
 * statx() against an already-open descriptor is spelled as an empty path
 * plus AT_EMPTY_PATH, which is the exact equivalent of fstatat(fd, "", ...)
 * that FSAL_VFS uses on the synchronous path.  The string literal has static
 * storage duration, so it outlives the SQE without any copy.
 */
static void vfs_uring_statx_sqe(struct io_uring_sqe *sqe, void *arg,
				unsigned int ring_idx)
{
	struct fsal_uring_fop_req *req = arg;

	io_uring_prep_statx(sqe, req->fd, "", AT_EMPTY_PATH, req->u.statx.mask,
			    req->u.statx.stx);
	uring_pers_attach(sqe, req->pers[0], ring_idx);
}

/*
 * There is deliberately no vfs_uring_close_sqe().  CLOSE is refused in
 * prep(), so nothing would ever install it; see vfs_uring_close_prep().
 */

/**
 * @brief Fill a FALLOCATE SQE.
 *
 * mode carries the FALLOC_FL_* bits, so this covers both NFSv4.2 ALLOCATE
 * (mode 0) and DEALLOCATE (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE).
 */
static void vfs_uring_falloc_sqe(struct io_uring_sqe *sqe, void *arg,
				 unsigned int ring_idx)
{
	struct fsal_uring_fop_req *req = arg;

	io_uring_prep_fallocate(sqe, req->fd, req->u.falloc.mode,
				req->u.falloc.offset, req->u.falloc.len);
	uring_pers_attach(sqe, req->pers[0], ring_idx);
}

/* -------------------------------------------------------------------------
 * prep handlers
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_statx_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot)
{
	fsal_status_t status;

	if (req->u.statx.stx == NULL) {
		LogUring(NIV_DEBUG, "VFS_URING not batching STATX, no buffer");
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	status = vfs_uring_acquire_fd(req);

	if (FSAL_IS_ERROR(status))
		return status;

	slot->sqe_pre = vfs_uring_statx_sqe;
	slot->sqe_pre_arg = req;
	slot->sqe_flags = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Refuse to batch CLOSE, so the planner runs the synchronous handler.
 *
 * This is not a stub: it is the only answer that is provably correct with
 * the descriptor contract this module implements, and the reasons are worth
 * recording because the entry looks trivially implementable.
 *
 * IORING_OP_CLOSE asks the *kernel* to close the descriptor.  Every other
 * operation here borrows an fd through fsal_start_io() and gives it back
 * through fsal_complete_io(); CLOSE cannot, because the thing it is given
 * back would already be gone.  Calling fsal_complete_io() on it would close
 * the fd a second time -- and by then the number may well have been reused
 * by another thread's open(), so the second close would silently destroy an
 * unrelated descriptor.  That rules out vfs_uring_acquire_fd() outright.
 *
 * Bypassing the helper and taking the descriptor straight out of the state
 * (container_of(state, struct vfs_state_fd, state)->vfs_fd) does not rescue
 * it either, because relinquishing a VFS descriptor is more than a close(2).
 * close_fsal_fd() in FSAL/commonlib.c, which is what vfs_close2() calls:
 *
 *   - takes the fd's work state with fsal_start_fd_work(), which *waits* for
 *     outstanding I/O on that fd to drain, then holds it across the close;
 *   - invokes obj_ops->close_func() (vfs_close_my_fd()), which both calls
 *     close(2) and clears my_fd->fd and my_fd->fsal_fd.openflags;
 *   - removes the fd from the global fd LRU and its accounting;
 *   - releases the work state and wakes anyone waiting to reclaim.
 *
 * Doing that asynchronously means splitting it in half: prep() would have to
 * hold the fd work state and steal the descriptor number out of the handle
 * so nothing else can close it, and finish() would have to complete the LRU
 * and work-state bookkeeping.  Each half is writable, but between them sit
 * two paths this module cannot make safe on its own:
 *
 *   - the abandoned-chain path.  If the chain is never submitted, release()
 *     has to undo a half-finished close, and the planner then re-runs the
 *     operation through vfs_close2(), which would find an fd slot prep() had
 *     already emptied and a share reservation prep() had already dropped.
 *     Getting that right needs the protocol-side CLOSE handler to cooperate,
 *     and that file is not ours to write.
 *   - NFSv4 CLOSE is not close2().  It drives the SAL open-state machine and
 *     may free the state_t itself, so the descriptor's owner can disappear
 *     while the SQE is still queued.
 *
 * The cost of refusing is small and bounded: a CLOSE simply terminates the
 * current chain and runs sequentially, which is what the planner does for
 * every category C operation anyway.  vfs_uring_op_supported() also answers
 * false for FSAL_URING_FOP_CLOSE, so in practice the planner never gets as
 * far as calling this; the check here is the backstop.
 */
fsal_status_t vfs_uring_close_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot)
{
	(void)slot;

	LogUring(NIV_DEBUG,
		 "VFS_URING CLOSE is not batched, running it synchronously");

	req->fd = -1;

	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

fsal_status_t vfs_uring_falloc_prep(struct fsal_uring_fop_req *req,
				    struct uring_chain_op *slot)
{
	fsal_status_t status;

	if (req->u.falloc.len == 0) {
		LogUring(NIV_DEBUG,
			 "VFS_URING not batching FALLOCATE, zero length");
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	status = vfs_uring_acquire_fd(req);

	if (FSAL_IS_ERROR(status))
		return status;

	slot->sqe_pre = vfs_uring_falloc_sqe;
	slot->sqe_pre_arg = req;
	slot->sqe_flags = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* -------------------------------------------------------------------------
 * finish handlers
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_statx_finish(struct fsal_uring_fop_req *req)
{
	req->status = vfs_uring_cqe_status(req);

	/* The kernel filled req->u.statx.stx directly; stx->stx_mask tells
	 * the caller which of the requested attributes it actually got, so
	 * there is nothing to copy out here.
	 */
	LogUring(NIV_FULL_DEBUG, "VFS_URING statx fd=%d mask=0x%x res=%" PRId32,
		 req->fd, req->u.statx.mask, req->res);

	vfs_uring_release_fd(req);

	return req->status;
}

/**
 * @brief Unreachable: vfs_uring_close_prep() never queues an SQE.
 *
 * Kept so the CLOSE descriptor has both halves and fsal_uring_fop_get() does
 * not reject it for the wrong reason -- the refusal must be the deliberate
 * one in prep()/op_supported(), not an incidental NULL.
 */
fsal_status_t vfs_uring_close_finish(struct fsal_uring_fop_req *req)
{
	LogUring(NIV_CRIT, "VFS_URING close completion reached, res=%" PRId32,
		 req->res);

	req->status = fsalstat(ERR_FSAL_NOTSUPP, 0);

	vfs_uring_release_fd(req);

	return req->status;
}

fsal_status_t vfs_uring_falloc_finish(struct fsal_uring_fop_req *req)
{
	req->status = vfs_uring_cqe_status(req);

	LogUring(NIV_FULL_DEBUG,
		 "VFS_URING fallocate fd=%d mode=0x%x off=%" PRIu64
		 " len=%" PRIu64 " res=%" PRId32,
		 req->fd, (unsigned int)req->u.falloc.mode,
		 req->u.falloc.offset, req->u.falloc.len, req->res);

	vfs_uring_release_fd(req);

	return req->status;
}

#endif /* USE_FSAL_URING */
