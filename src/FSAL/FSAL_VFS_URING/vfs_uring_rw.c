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
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_rw.c
 * @brief Asynchronous read2()/write2() for FSAL_VFS_URING.
 *
 * This is the per-operation half of the module. The other half,
 * vfs_uring_io.c, plugs into the NFSv4 compound planner and only ever runs
 * for a COMPOUND whose operations the planner chose to batch into one linked
 * submission. Everything else -- NFSv3, READ_PLUS, a COMPOUND the planner
 * refused, a read issued by an upper layer rather than by a protocol op --
 * arrives here, through the ordinary obj_ops->read2()/write2() entry points.
 *
 * Both halves submit to the same engine. The difference is only in how the
 * submission is shaped: the planner builds a chain of SQEs and owns the
 * descriptor across it (vfs_uring_fd.c), while a single read2() hands one
 * request to the generic Layer-2 adapter in FSAL/fsal_uring.c, which already
 * implements the whole choreography -- rebuild op_ctx on the reaper thread,
 * map the CQE to an fsal_status_t, fsal_complete_io(), release the temporary
 * share reservation, honour the FSAL_CLOSEFD resume protocol, call done_cb().
 *
 * What is left for this file is the part that is genuinely VFS's:
 *
 *   - the checks the synchronous handlers make (READ_PLUS, cross-FSAL);
 *   - fsal_start_io(), which needs the VFS-private struct vfs_fd and which
 *     may open into a temporary descriptor that now has to outlive the
 *     calling frame and therefore lives on the heap with the request;
 *   - the read buffer, which the protocol layer no longer allocates for this
 *     FSAL (fso_allocate_own_read_buffer) so that a read can land straight
 *     in a registered io_uring buffer;
 *   - falling back to preadv()/pwritev() whenever any of that is not
 *     possible, with exactly the semantics of FSAL_VFS.
 *
 * ZERO-COPY READS
 *
 * FSAL_VFS_URING advertises fso_allocate_own_read_buffer whenever its engine
 * is up, which makes fsal_read2() hand us iov[0].iov_base == NULL and leave
 * the allocation to us. fsal_uring_read_buffer() then takes a registered
 * buffer from the engine's pool when one of the right size class is free --
 * so the read becomes IORING_OP_READ_FIXED into pinned memory -- and an
 * ordinary heap buffer when it is not. Either way it installs the matching
 * iov_release, the buffer travels into the reply, and the protocol layer
 * frees it when the reply is freed.
 *
 * Because that flag is a property of the export and not of the call, every
 * path out of this file has to cope with a NULL iov_base, including the
 * synchronous fallbacks. They do, because they all run
 * fsal_uring_read_buffer() first, which fills the iovec in before it decides
 * whether a registered buffer was available.
 *
 * CREDENTIALS
 *
 * The I/O runs under an io_uring personality, never under an impersonated
 * submitting thread. vfs_uring_cred_ops_get() supplies the pair VFS uses to
 * assume the client's identity for the one registration that interns it; on
 * the I/O path itself the credential is already in sqe->personality and
 * nothing is impersonated. If no personality can be had -- registry full,
 * registration refused, kernel without IORING_REGISTER_PERSONALITY --
 * fsal_uring_io_start() declines rather than submit under the daemon's
 * identity, and the I/O completes through the synchronous fallback below.
 * FSAL_URING/URING_IO_PATH.md section 7 has the full reasoning.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/uio.h>

/* liburing.h has to come before any Ganesha header: some versions define
 * UNUSED as an object-like macro, and common_utils.h only installs the
 * function-like one under an #ifndef guard. Same dance as vfs_uring_io.c.
 */
#include <liburing.h>
#undef UNUSED

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_uring.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/**
 * @brief Per-request state for one asynchronous read or write.
 *
 * The adapter frees the whole thing with gsh_free(req, MEM_COMP_FSAL) once
 * the I/O is done, which is why @a base must be first and why this must be
 * allocated with that component.
 */
struct vfs_uring_rw_req {
	/** Generic adapter state. MUST be the first member. */
	struct fsal_uring_io_req base;

	/**
	 * Heap home for a temporary descriptor.
	 *
	 * fsal_start_io() opens into this when neither the state's fd nor the
	 * handle's global fd is usable. The synchronous handlers keep the
	 * equivalent on the stack; an asynchronous I/O outlives the frame
	 * that started it, so it cannot.
	 */
	struct vfs_fd temp_fd;
};

/**
 * @brief Everything the two entry points resolve before they diverge.
 */
struct vfs_uring_rw_setup {
	struct vfs_uring_rw_req *req;
	struct vfs_fsal_obj_handle *myself;
	struct fsal_fd *out_fd;
	int fd;
};

/**
 * @brief Common preamble: allocate the request and acquire a descriptor.
 *
 * On error nothing has been acquired and nothing has to be released; the
 * caller reports @a status through done_cb() and is finished.
 */
static fsal_status_t vfs_uring_rw_begin(struct fsal_obj_handle *obj_hdl,
					bool bypass, struct state_t *state,
					fsal_openflags_t openflags,
					struct vfs_uring_rw_setup *setup)
{
	struct vfs_uring_rw_req *req;
	fsal_status_t status;

	setup->myself =
		container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	req = gsh_calloc(1, sizeof(*req), MEM_COMP_FSAL);

	init_fsal_fd(&req->temp_fd.fsal_fd, FSAL_FD_TEMP, op_ctx->fsal_export);
	req->temp_fd.fd = -1;

	status = fsal_start_io(&setup->out_fd, obj_hdl,
			       &setup->myself->u.file.fd.fsal_fd,
			       &req->temp_fd.fsal_fd, state, openflags, false,
			       NULL, bypass, &setup->myself->u.file.share);

	if (FSAL_IS_ERROR(status)) {
		/* fsal_start_io() has already released anything it took,
		 * including the share reservation.
		 */
		LogFullDebug(COMPONENT_FSAL,
			     "fsal_start_io failed returning %s",
			     fsal_err_txt(status));
		gsh_free(req, MEM_COMP_FSAL);
		setup->req = NULL;
		return status;
	}

	setup->req = req;
	setup->fd = container_of(setup->out_fd, struct vfs_fd, fsal_fd)->fd;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Tail shared by both synchronous fallbacks.
 *
 * Mirrors the out/exit tail of vfs_read2() and vfs_write2() exactly, and
 * additionally frees the request that would have carried the asynchronous
 * I/O. @a status is the result of the syscall and is what reaches done_cb().
 */
static void vfs_uring_rw_sync_end(struct fsal_obj_handle *obj_hdl,
				  struct vfs_uring_rw_setup *setup,
				  struct fsal_io_arg *io_arg,
				  fsal_openflags_t share_access,
				  fsal_async_cb done_cb, void *caller_arg,
				  fsal_status_t status)
{
	fsal_status_t status2;

	status2 = fsal_complete_io(obj_hdl, setup->out_fd);

	LogFullDebug(COMPONENT_FSAL, "fsal_complete_io returned %s",
		     fsal_err_txt(status2));

	if (io_arg->state == NULL) {
		/* The I/O ran without a state, so fsal_start_io() took a
		 * temporary share reservation. Release it.
		 */
		update_share_counters_locked(obj_hdl,
					     &setup->myself->u.file.share,
					     share_access, FSAL_O_CLOSED);
	}

	/* fsal_complete_io() may have closed temp_fd, so the request can only
	 * go now.
	 */
	gsh_free(setup->req, MEM_COMP_FSAL);

	done_cb(obj_hdl, status, io_arg, caller_arg);
}

/* -------------------------------------------------------------------------
 * read2
 * -------------------------------------------------------------------------
 */

/**
 * @brief Read data from a file, asynchronously when io_uring is available.
 *
 * Same contract as vfs_read2(); see fsal_api.h. The result is delivered
 * through @a done_cb, which for a submitted I/O runs on a reaper thread
 * rather than on this one.
 */
void vfs_uring_read2(struct fsal_obj_handle *obj_hdl, bool bypass,
		     fsal_async_cb done_cb, struct fsal_io_arg *read_arg,
		     void *caller_arg)
{
	struct fsal_uring_engine *engine;
	struct vfs_uring_rw_setup setup;
	struct fsal_uring_io io = { 0 };
	fsal_status_t status;
	ssize_t nb_read;
	int rc;

	/*
	 * Resume leg of the FSAL_CLOSEFD protocol: the I/O is already
	 * finished, but completing it meant closing a descriptor, which the
	 * reaper thread must not do. The adapter owns everything from here.
	 */
	if (read_arg->fsal_resume == FSAL_CLOSEFD) {
		fsal_uring_io_resume(read_arg, done_cb, caller_arg);
		return;
	}

	engine = vfs_uring_engine(obj_hdl->fsal);

	if (engine == NULL) {
		/* io_uring was switched off or could not be set up. The
		 * module then never advertised fso_allocate_own_read_buffer
		 * either, so the buffer is the protocol layer's and
		 * vfs_read2() is correct unchanged.
		 */
		vfs_read2(obj_hdl, bypass, done_cb, read_arg, caller_arg);
		return;
	}

	if (read_arg->info != NULL) {
		/*
		 * READ_PLUS. Answer exactly as vfs_read2() does and leave the
		 * iovec alone: fsal_read2()'s fallback turns this into a
		 * seek2() pair plus a second read2() call with info == NULL,
		 * and that call is the one that allocates.
		 */
		status = posix2fsal_status(ENOTSUP);
		goto exit;
	}

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(
			COMPONENT_FSAL,
			"FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		status = posix2fsal_status(EXDEV);
		goto exit;
	}

	status = vfs_uring_rw_begin(obj_hdl, bypass, read_arg->state,
				    FSAL_O_READ, &setup);

	if (FSAL_IS_ERROR(status))
		goto exit;

	/*
	 * Provide the buffer. This fills iov[0] in and installs its release
	 * whether or not a registered buffer was available, so every path
	 * below -- including the preadv() fallback -- has a usable iovec.
	 */
	io.buf = fsal_uring_read_buffer(engine, read_arg);

	io.obj_hdl = obj_hdl;
	io.out_fd = setup.out_fd;
	io.fd = setup.fd;
	io.is_write = false;
	io.stable = false;
	/* fsal_start_io() takes a temporary share reservation exactly when
	 * there is no state; FSAL_O_READ is never FSAL_O_ANY, so the second
	 * half of that condition is always true here.
	 */
	io.share = read_arg->state == NULL ? &setup.myself->u.file.share : NULL;
	io.share_access = FSAL_O_READ;
	io.done_cb = done_cb;
	io.io_arg = read_arg;
	io.caller_arg = caller_arg;
	io.cred_ops = vfs_uring_cred_ops_get();

	rc = fsal_uring_io_start(engine, &setup.req->base, &io);

	if (rc == 0) {
		/* In flight. The request, the iovec and the temporary
		 * descriptor now belong to the completion path.
		 */
		return;
	}

	/*
	 * Nothing was queued -- the ring refused it, or more usually there
	 * was no io_uring personality for this client and the adapter would
	 * not run the read as the daemon. Finish it here; the descriptor and
	 * the buffer are both already in hand, which makes this cheaper than
	 * unwinding and calling vfs_read2().
	 */
	LogFullDebug(COMPONENT_FSAL,
		     "io_uring submit declined (%s), falling back to preadv",
		     strerror(-rc));

	nb_read = preadv(setup.fd, read_arg->iov, read_arg->iov_count,
			 read_arg->offset);

	if (read_arg->offset == -1 || nb_read == -1) {
		status = posix2fsal_status(errno);
		LogFullDebug(COMPONENT_FSAL, "preadv failed returning %s",
			     fsal_err_txt(status));
	} else {
		read_arg->io_amount = nb_read;
		read_arg->end_of_file = (nb_read == 0);
		status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	vfs_uring_rw_sync_end(obj_hdl, &setup, read_arg, FSAL_O_READ, done_cb,
			      caller_arg, status);
	return;

exit:

	done_cb(obj_hdl, status, read_arg, caller_arg);
}

/* -------------------------------------------------------------------------
 * write2
 * -------------------------------------------------------------------------
 */

/**
 * @brief Write data to a file, asynchronously when io_uring is available.
 *
 * Same contract as vfs_write2(); see fsal_api.h. A stable write becomes a
 * WRITEV with an fsync linked behind it, so the flush costs no extra round
 * trip and never blocks a thread.
 */
void vfs_uring_write2(struct fsal_obj_handle *obj_hdl, bool bypass,
		      fsal_async_cb done_cb, struct fsal_io_arg *write_arg,
		      void *caller_arg)
{
	struct fsal_uring_engine *engine;
	struct vfs_uring_rw_setup setup;
	struct fsal_uring_io io = { 0 };
	fsal_status_t status, status2;
	ssize_t nb_written;
	int rc;

	if (write_arg->fsal_resume == FSAL_CLOSEFD) {
		fsal_uring_io_resume(write_arg, done_cb, caller_arg);
		return;
	}

	engine = vfs_uring_engine(obj_hdl->fsal);

	if (engine == NULL) {
		vfs_write2(obj_hdl, bypass, done_cb, write_arg, caller_arg);
		return;
	}

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(
			COMPONENT_FSAL,
			"FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		status = posix2fsal_status(EXDEV);
		goto exit;
	}

	status = vfs_uring_rw_begin(obj_hdl, bypass, write_arg->state,
				    FSAL_O_WRITE, &setup);

	if (FSAL_IS_ERROR(status))
		goto exit;

	io.obj_hdl = obj_hdl;
	io.out_fd = setup.out_fd;
	io.fd = setup.fd;
	io.is_write = true;
	/* A stable write gets an fsync linked behind the data, which is what
	 * the adapter's second SQE is for. Its result is reported back
	 * through fsal_stable, exactly as the synchronous fsync() is.
	 */
	io.stable = write_arg->fsal_stable;
	io.share = write_arg->state == NULL ? &setup.myself->u.file.share
					    : NULL;
	io.share_access = FSAL_O_WRITE;
	io.buf = NULL; /* The data is the client's, already in a buffer. */
	io.done_cb = done_cb;
	io.io_arg = write_arg;
	io.caller_arg = caller_arg;
	io.cred_ops = vfs_uring_cred_ops_get();

	rc = fsal_uring_io_start(engine, &setup.req->base, &io);

	if (rc == 0)
		return;

	/* As in read2: the ring refused it, or there was no personality for
	 * this client. Either way the write runs here, under the client's
	 * identity assumed on this thread.
	 */
	LogFullDebug(COMPONENT_FSAL,
		     "io_uring submit declined (%s), falling back to pwritev",
		     strerror(-rc));

	/* Impersonate the client for the duration of the pwritev call. */
	if (!vfs_set_credentials(&op_ctx->creds, obj_hdl->fsal)) {
		status = posix2fsal_status(EPERM);
		LogFullDebug(COMPONENT_FSAL,
			     "vfs_set_credentials failed returning %s",
			     fsal_err_txt(status));
		goto sync_end;
	}

	nb_written = pwritev(setup.fd, write_arg->iov, write_arg->iov_count,
			     write_arg->offset);

	if (nb_written == -1) {
		status = posix2fsal_status(errno);
		LogFullDebug(COMPONENT_FSAL, "pwritev failed returning %s",
			     fsal_err_txt(status));
	} else {
		status = fsalstat(ERR_FSAL_NO_ERROR, 0);
		write_arg->io_amount = nb_written;

		if (write_arg->fsal_stable && fsync(setup.fd) == -1) {
			status2 = posix2fsal_status(errno);
			write_arg->fsal_stable = false;
			LogFullDebug(COMPONENT_FSAL, "fsync returned %s",
				     fsal_err_txt(status2));
		}
	}

	vfs_restore_ganesha_credentials(obj_hdl->fsal);

sync_end:

	vfs_uring_rw_sync_end(obj_hdl, &setup, write_arg, FSAL_O_WRITE, done_cb,
			      caller_arg, status);
	return;

exit:

	done_cb(obj_hdl, status, write_arg, caller_arg);
}

#endif /* USE_FSAL_URING */
