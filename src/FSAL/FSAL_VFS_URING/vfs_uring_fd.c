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
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_fd.c
 * @brief Descriptor ownership for the FSAL_VFS_URING fops vector.
 *
 * Every SQE this module builds acts on a POSIX file descriptor, and getting
 * one is not something the protocol layer can do: it means picking between
 * the state's fd, the handle's global fd and a freshly opened temporary one,
 * honouring the share reservations recorded on the handle, and possibly
 * reopening in a wider mode.  All of that is fsal_start_io(), whose arguments
 * are the VFS-private struct vfs_fd inside struct vfs_fsal_obj_handle.  So
 * the FSAL acquires the descriptor in prep() and gives it back in finish().
 *
 * The complication, and the reason this lives in its own file rather than
 * inside each handler, is that the acquisition straddles an asynchronous gap.
 * fsal_start_io() may open into a caller-supplied "temporary" fsal_fd; on the
 * synchronous path that struct is a stack local of the operation, but here
 * the I/O outlives the prep() frame, so it has to be heap allocated and
 * carried on the request until the completion runs.  struct vfs_uring_fd_ref
 * is that carrier, and it is also where we remember whether a temporary share
 * reservation has to be undone.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <stdbool.h>
#include <stddef.h>

/* liburing.h has to come before any Ganesha header -- FSAL/fsal_uring_fops.h
 * pulls it in transitively through io_uring/core/uring_engine.h, and some
 * liburing versions define UNUSED as an object-like macro while
 * common_utils.h only installs the function-like one under an #ifndef guard.
 * Same dance as vfs_uring_io.c.
 */
#include <liburing.h>
#undef UNUSED

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/**
 * @brief The access mode an operation needs when the caller did not state one.
 *
 * struct fsal_uring_fop_req::openflags is filled in by the protocol layer,
 * which knows the stateid the client presented.  A zeroed field is
 * FSAL_O_CLOSED, which is not a mode fsal_start_io() can act on -- it would
 * ask check_share_conflict() to reserve nothing and wait_to_start_io() to
 * accept any already-open fd.  Rather than fail the batch, fall back to the
 * weakest mode that still makes the SQE legal, which is a property of the
 * operation and therefore genuinely ours to decide:
 *
 *  - READ needs the file readable, WRITE and FALLOCATE need it writable;
 *  - FSYNC and STATX need only an open descriptor, which is FSAL_O_ANY, the
 *    same mode the synchronous vfs_commit2() asks for.
 *
 * FSAL_O_ANY is deliberately not a no-op choice: fsal_start_global_io() skips
 * the share reservation entirely for it, so an operation defaulted to
 * FSAL_O_ANY also skips the matching release in vfs_uring_release_fd().
 */
static fsal_openflags_t vfs_uring_default_openflags(enum fsal_uring_fop fop)
{
	switch (fop) {
	case FSAL_URING_FOP_READ:
		return FSAL_O_READ;
	case FSAL_URING_FOP_WRITE:
	case FSAL_URING_FOP_FALLOCATE:
		return FSAL_O_WRITE;
	case FSAL_URING_FOP_SYNC:
	case FSAL_URING_FOP_STATX:
	case FSAL_URING_FOP_CLOSE:
	case FSAL_URING_FOP_LAST:
		break;
	}

	return FSAL_O_ANY;
}

fsal_status_t vfs_uring_acquire_fd(struct fsal_uring_fop_req *req)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_uring_fd_ref *ref;
	fsal_openflags_t openflags;
	fsal_status_t status;

	if (req->obj == NULL) {
		LogUring(NIV_DEBUG, "VFS_URING fop %d has no handle",
			 (int)req->fop);
		req->fd = -1;
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	/*
	 * struct vfs_fsal_obj_handle keeps its per-type state in a union, and
	 * alloc_handle() only initialises the u.file arm for a regular file;
	 * for a directory the fsal_fd and the fsal_share below are raw zeroed
	 * memory that was never passed to init_fsal_fd().  This is a real
	 * case and not a paranoia check: GETATTR rides FSAL_URING_FOP_STATX
	 * and its current filehandle is very often a directory.
	 */
	if (req->obj->type != REGULAR_FILE) {
		LogUring(NIV_DEBUG, "VFS_URING fop %d on a %s, not batching",
			 (int)req->fop,
			 object_file_type_to_str(req->obj->type));
		req->fd = -1;
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	/*
	 * Same cross-FSAL check the synchronous handlers make. Refusing the
	 * batch rather than reporting EXDEV here is deliberate: the fallback
	 * runs vfs_read2()/vfs_write2(), which produce exactly that error
	 * with the logging the rest of the tree expects.
	 */
	if (req->obj->fs != NULL && req->obj->fsal != req->obj->fs->fsal) {
		LogUring(NIV_DEBUG,
			 "VFS_URING fop %d: handle on FSAL %s, not batching",
			 (int)req->fop, req->obj->fs->fsal->name);
		req->fd = -1;
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	/*
	 * One descriptor per request, not per SQE.  A stable WRITE occupies
	 * two chain slots -- the WRITEV and the fsync linked behind it -- and
	 * both are built from this one request, so whichever prep() runs
	 * second must reuse what the first took rather than acquire a second
	 * reference that nothing would ever release.
	 */
	if (req->fsal_priv != NULL) {
		LogUring(NIV_FULL_DEBUG, "VFS_URING fop %d reusing fd=%d",
			 (int)req->fop, req->fd);
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	myself = container_of(req->obj, struct vfs_fsal_obj_handle, obj_handle);

	openflags = (req->openflags != FSAL_O_CLOSED)
			    ? req->openflags
			    : vfs_uring_default_openflags(req->fop);

	/* The I/O outlives this frame, so unlike the synchronous handlers the
	 * temporary fd cannot live on the stack.
	 */
	ref = gsh_calloc(1, sizeof(*ref), MEM_COMP_FSAL);

	init_fsal_fd(&ref->temp_fd.fsal_fd, FSAL_FD_TEMP, op_ctx->fsal_export);
	ref->temp_fd.fd = -1;

	status = fsal_start_io(&ref->out_fd, req->obj,
			       &myself->u.file.fd.fsal_fd,
			       &ref->temp_fd.fsal_fd, req->state, openflags,
			       false, NULL, req->bypass, &myself->u.file.share);

	if (FSAL_IS_ERROR(status)) {
		/* fsal_start_io() has already released anything it took,
		 * including the share reservation, and left out_fd NULL.
		 */
		LogUring(NIV_DEBUG, "VFS_URING fop %d fsal_start_io failed: %s",
			 (int)req->fop, fsal_err_txt(status));
		gsh_free(ref, MEM_COMP_FSAL);
		req->fd = -1;
		return status;
	}

	req->fd = container_of(ref->out_fd, struct vfs_fd, fsal_fd)->fd;

	/*
	 * fsal_start_io() only takes a temporary share reservation when it
	 * reached the global-fd path with no state and a real mode: it passes
	 * share through as "state == NULL ? share : NULL", and
	 * fsal_start_global_io() then ignores it for FSAL_O_ANY.  Mirror that
	 * test exactly, because recording a reservation that was never taken
	 * would make vfs_uring_release_fd() drive the share counters negative.
	 */
	if (req->state == NULL && openflags != FSAL_O_ANY) {
		ref->share = &myself->u.file.share;
		ref->share_access = openflags;
	}

	req->fsal_priv = ref;

	LogUring(NIV_FULL_DEBUG,
		 "VFS_URING fop %d acquired fd=%d openflags=0x%x temp=%s",
		 (int)req->fop, req->fd, (unsigned int)openflags,
		 ref->out_fd == &ref->temp_fd.fsal_fd ? "yes" : "no");

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

void vfs_uring_release_fd(struct fsal_uring_fop_req *req)
{
	struct vfs_uring_fd_ref *ref = req->fsal_priv;
	fsal_status_t status;

	/* Idempotent: a prep() that failed, or a finish() running after a
	 * release(), both arrive here with nothing to give back.
	 */
	if (ref == NULL) {
		req->fd = -1;
		return;
	}

	/*
	 * fsal_complete_io() can close(2) the descriptor.  When
	 * fsal_start_io() could not use the handle's global fd it opened our
	 * temporary one instead and set close_on_complete on it, and that is
	 * the case fsal_complete_io() turns straight into fsal_close_fd().
	 *
	 * That is precisely what the per-operation path cannot do.  There the
	 * completion runs on the ring's reaper thread, which has no op_ctx
	 * and no FSAL thread state, so a completion that needs to close has
	 * to be bounced back to a worker -- the FSAL_CLOSEFD flag and the
	 * fsal_uring_io_resume() round trip that vfs_uring_read2() and
	 * vfs_uring_write2() handle at the top of their bodies.
	 *
	 * None of that is needed here.  Every caller of this function is
	 * already on a worker thread: finish() is called from
	 * nfs4_compound_uring_resume(), after resume_op_context() has
	 * restored op_ctx, and release() is called from the planner, which
	 * never left the worker thread at all.  On this path the reaper's
	 * entire job is svc_resume().  So the close can simply happen
	 * inline, here, and the chain path has no deferred-close protocol.
	 */
	if (ref->out_fd != NULL) {
		status = fsal_complete_io(req->obj, ref->out_fd);

		if (FSAL_IS_ERROR(status))
			LogUring(NIV_DEBUG,
				 "VFS_URING fsal_complete_io returned %s",
				 fsal_err_txt(status));
	}

	if (ref->share != NULL) {
		/* The operation ran without a state, so fsal_start_io() took a
		 * temporary share reservation on our behalf. Give it back.
		 */
		update_share_counters_locked(req->obj, ref->share,
					     ref->share_access, FSAL_O_CLOSED);
	}

	gsh_free(ref, MEM_COMP_FSAL);

	req->fsal_priv = NULL;
	req->fd = -1;
}

#endif /* USE_FSAL_URING */
