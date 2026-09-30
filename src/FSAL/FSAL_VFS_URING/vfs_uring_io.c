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
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_io.c
 * @brief READ, WRITE and SYNC handlers of the FSAL_VFS_URING fops vector.
 *
 * One prep()/finish() pair per operation, as described in
 * FSAL/fsal_uring_fops.h.  prep() runs on a worker thread while the chain is
 * being planned and only fills in the chain slot; the sqe_pre callback it
 * installs runs later, inside the engine's submit_lock, and is the single
 * place where the SQE layout and the io_uring personality are decided.
 *
 * The sqe_pre callbacks themselves take no lock, allocate nothing, and never
 * touch op_ctx; everything they need has been resolved by the time they run.
 * prep() and finish() are under no such restriction -- both run on a worker
 * thread -- and they are where the file descriptor is acquired and released,
 * through the helpers in vfs_uring_fd.c.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

/* liburing.h has to come before any Ganesha header: some versions define
 * UNUSED as an object-like macro, and common_utils.h only installs the
 * function-like one under an #ifndef guard. Same dance as FSAL/fsal_uring.c.
 */
#include <liburing.h>
#undef UNUSED

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
/* uring_pers_attach() is a static inline, so including this header costs no
 * link-time dependency on the (non-exported) uring_pers_* functions.
 */
#include "../FSAL_URING/uring_pers.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/* -------------------------------------------------------------------------
 * sqe_pre callbacks -- run inside the engine's submit_lock
 * -------------------------------------------------------------------------
 */

/**
 * @brief May this transfer use the fixed-buffer opcode?
 *
 * The fixed opcodes take exactly one address and one length, so a registered
 * buffer is only usable for a single-segment transfer.  A multi-segment
 * request that happens to carry one falls back to the scatter/gather opcode;
 * the buffer is deliberately not cleared, because it belongs to the protocol
 * layer, which has to release it either way.
 */
static inline bool vfs_uring_rw_fixed(const struct fsal_uring_fop_req *req)
{
	return req->u.rw.buf != NULL && req->u.rw.iovcnt == 1;
}

/**
 * @brief Fill a READ SQE.
 *
 * Uses the fixed-buffer opcode when the protocol layer handed us a
 * registered buffer, otherwise the scatter/gather opcode.  The transfer
 * length for the fixed case is iov[0].iov_len and not buf->len: the pool is
 * size-classed, so the buffer is generally larger than the read.
 */
static void vfs_uring_read_sqe(struct io_uring_sqe *sqe, void *arg,
			       unsigned int ring_idx)
{
	struct fsal_uring_fop_req *req = arg;

	if (vfs_uring_rw_fixed(req))
		io_uring_prep_read_fixed(sqe, req->fd, req->u.rw.buf->addr,
					 (unsigned int)req->u.rw.iov[0].iov_len,
					 req->u.rw.offset,
					 (int)req->u.rw.buf->index);
	else
		io_uring_prep_readv(sqe, req->fd, req->u.rw.iov,
				    (unsigned int)req->u.rw.iovcnt,
				    req->u.rw.offset);

	/* Stamp the client credential. A NULL pers leaves sqe->personality at
	 * 0, which means "run under the submitting thread's credential" -- the
	 * fallback the protocol layer takes when the registry could not intern
	 * the identity, and which it pairs with fsal_set_credentials().
	 */
	uring_pers_attach(sqe, req->pers[0], ring_idx);
}

/**
 * @brief Fill a WRITE SQE.  Mirror image of vfs_uring_read_sqe().
 */
static void vfs_uring_write_sqe(struct io_uring_sqe *sqe, void *arg,
				unsigned int ring_idx)
{
	struct fsal_uring_fop_req *req = arg;

	if (vfs_uring_rw_fixed(req))
		io_uring_prep_write_fixed(
			sqe, req->fd, req->u.rw.buf->addr,
			(unsigned int)req->u.rw.iov[0].iov_len,
			req->u.rw.offset, (int)req->u.rw.buf->index);
	else
		io_uring_prep_writev(sqe, req->fd, req->u.rw.iov,
				     (unsigned int)req->u.rw.iovcnt,
				     req->u.rw.offset);

	uring_pers_attach(sqe, req->pers[0], ring_idx);
}

/**
 * @brief Fill a SYNC SQE.
 *
 * This is a full fsync() of the descriptor, deliberately, even though
 * struct fsal_uring_fop_req carries an offset and a length:
 *
 *  - vfs_commit2() is an fsync() today, and IORING_OP_SYNC_FILE_RANGE is not
 *    an asynchronous equivalent of it.  sync_file_range() writes back page
 *    cache but flushes neither the inode's metadata nor the device's write
 *    cache, so on its own it cannot satisfy what an NFS COMMIT promises.
 *  - io_uring_prep_sync_file_range() only appeared in liburing 2.2 while
 *    this tree builds against liburing >= 2.0.
 *
 * An fsync is a superset of the requested range, so ignoring offset/len is
 * conservative rather than wrong.  See the report notes if a ranged
 * writeback is wanted as a separate, non-COMMIT operation.
 */
static void vfs_uring_sync_sqe(struct io_uring_sqe *sqe, void *arg,
			       unsigned int ring_idx)
{
	struct fsal_uring_fop_req *req = arg;

	io_uring_prep_fsync(sqe, req->fd, 0);
	uring_pers_attach(sqe, req->pers[0], ring_idx);
}

/* -------------------------------------------------------------------------
 * prep handlers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Common slot wiring and descriptor acquisition for the read/write
 *        pair.
 *
 * The buffer arguments are validated before anything is acquired, so the
 * only failure after vfs_uring_acquire_fd() would have to come from the
 * acquisition itself, which cleans up after itself.  There is therefore no
 * release on the error path here; an operation that returns an error has
 * taken nothing.
 */
static fsal_status_t vfs_uring_rw_slot(struct fsal_uring_fop_req *req,
				       struct uring_chain_op *slot,
				       void (*sqe_pre)(struct io_uring_sqe *,
						       void *, unsigned int))
{
	fsal_status_t status;

	if (req->u.rw.iov == NULL || req->u.rw.iovcnt <= 0) {
		LogUring(NIV_DEBUG,
			 "VFS_URING not batching fop %d, iov=%p iovcnt=%d",
			 (int)req->fop, req->u.rw.iov, req->u.rw.iovcnt);
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	status = vfs_uring_acquire_fd(req);

	if (FSAL_IS_ERROR(status))
		return status;

	slot->sqe_pre = sqe_pre;
	slot->sqe_pre_arg = req;
	/* IOSQE_IO_LINK is the engine's business, not ours. */
	slot->sqe_flags = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t vfs_uring_read_prep(struct fsal_uring_fop_req *req,
				  struct uring_chain_op *slot)
{
	return vfs_uring_rw_slot(req, slot, vfs_uring_read_sqe);
}

fsal_status_t vfs_uring_write_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot)
{
	return vfs_uring_rw_slot(req, slot, vfs_uring_write_sqe);
}

fsal_status_t vfs_uring_sync_prep(struct fsal_uring_fop_req *req,
				  struct uring_chain_op *slot)
{
	fsal_status_t status = vfs_uring_acquire_fd(req);

	if (FSAL_IS_ERROR(status))
		return status;

	slot->sqe_pre = vfs_uring_sync_sqe;
	slot->sqe_pre_arg = req;
	slot->sqe_flags = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* -------------------------------------------------------------------------
 * finish handlers
 *
 * Each one consumes req->res, sets req->status and its own outputs, logs
 * while req->fd is still meaningful, and only then hands the descriptor back
 * with vfs_uring_release_fd().
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_read_finish(struct fsal_uring_fop_req *req)
{
	req->status = vfs_uring_cqe_status(req);

	if (FSAL_IS_ERROR(req->status)) {
		req->u.rw.amount = 0;
		req->u.rw.eof = false;
	} else {
		req->u.rw.amount = (uint64_t)req->res;
		/* A short read is not end of file; only a zero-length one at
		 * the requested offset is.
		 */
		req->u.rw.eof = (req->res == 0);
	}

	LogUring(NIV_FULL_DEBUG,
		 "VFS_URING read fd=%d off=%" PRIu64 " res=%" PRId32
		 " amount=%" PRIu64 " eof=%s",
		 req->fd, req->u.rw.offset, req->res, req->u.rw.amount,
		 req->u.rw.eof ? "yes" : "no");

	vfs_uring_release_fd(req);

	return req->status;
}

fsal_status_t vfs_uring_write_finish(struct fsal_uring_fop_req *req)
{
	req->status = vfs_uring_cqe_status(req);

	if (FSAL_IS_ERROR(req->status))
		req->u.rw.amount = 0;
	else
		req->u.rw.amount = (uint64_t)req->res;

	/* eof is meaningless for a write; leave it as the caller set it. */

	LogUring(NIV_FULL_DEBUG,
		 "VFS_URING write fd=%d off=%" PRIu64 " res=%" PRId32
		 " amount=%" PRIu64,
		 req->fd, req->u.rw.offset, req->res, req->u.rw.amount);

	vfs_uring_release_fd(req);

	return req->status;
}

fsal_status_t vfs_uring_sync_finish(struct fsal_uring_fop_req *req)
{
	req->status = vfs_uring_cqe_status(req);

	LogUring(NIV_FULL_DEBUG, "VFS_URING sync fd=%d res=%" PRId32, req->fd,
		 req->res);

	vfs_uring_release_fd(req);

	return req->status;
}

#endif /* USE_FSAL_URING */
