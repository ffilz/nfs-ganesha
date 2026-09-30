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
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_fops.c
 * @brief The FSAL_VFS_URING io_uring operation vector.
 *
 * One descriptor per abstract operation, assembled into the
 * struct fsal_uring_fops that every export of this module publishes.  The
 * handlers themselves live next door in vfs_uring_io.c and vfs_uring_meta.c;
 * what is decided here is only which generic opcode each operation is gated
 * on, and whether this FSAL wants to override the kernel's answer.
 *
 * There are deliberately no "#ifdef IORING_OP_xxx" guards around the table.
 * IORING_OP_* are enumerators of enum io_uring_op, not preprocessor macros,
 * so such a guard is always false and would silently delete the entry.  The
 * right question -- "does the running kernel implement this opcode" -- is a
 * runtime one, and fsal_uring_fop_get() already asks it through the engine's
 * cached IORING_REGISTER_PROBE result before returning any descriptor.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <errno.h>
#include <stdbool.h>

/* liburing.h before any Ganesha header; see vfs_uring_io.c. */
#include <liburing.h>
#undef UNUSED

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
/* struct uring_pers_cred_ops is defined, not just declared, here: this file
 * statically initialises one.
 */
#include "../FSAL_URING/uring_pers.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/* -------------------------------------------------------------------------
 * CQE result translation
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_cqe_status(const struct fsal_uring_fop_req *req)
{
	if (req->res >= 0)
		return fsalstat(ERR_FSAL_NO_ERROR, 0);

	/*
	 * -ECANCELED is not a filesystem error.  The SQEs of a chain are
	 * hard-linked, so when one of them fails the kernel cancels every
	 * later SQE without ever attempting it.  Mapping it through
	 * posix2fsal_status() would both log a spurious LogCrit and report
	 * ERR_FSAL_SERVERFAULT for an operation that never ran.
	 *
	 * ERR_FSAL_DELAY is the honest answer at this layer: nothing was
	 * done and the work is safe to retry.  A planner that wants better
	 * than a client-visible NFS4ERR_DELAY should test for
	 * res == -ECANCELED itself and re-drive the operation through the
	 * synchronous handler, which is always correct because the
	 * cancelled SQE had no effect.
	 */
	if (req->res == -ECANCELED)
		return fsalstat(ERR_FSAL_DELAY, ECANCELED);

	return posix2fsal_status(-req->res);
}

/* -------------------------------------------------------------------------
 * Per-operation descriptors
 * -------------------------------------------------------------------------
 */

static const struct fsal_uring_fop_desc vfs_uring_desc_read = {
	.name = "READ",
	/*
	 * Gated on READV even though a request carrying a registered buffer
	 * is submitted as IORING_OP_READ_FIXED.  Both opcodes arrived in the
	 * same kernel (5.1) and the engine only hands out registered buffers
	 * when it managed to register them, so READV is the conservative and
	 * sufficient probe.
	 */
	.kernel_opcode = IORING_OP_READV,
	/* Ordinary POSIX op on a real descriptor: the kernel
	 * enforces permissions, so it must see the client.
	 */
	.pers_resolve = fsal_uring_pers_caller,
	.prep = vfs_uring_read_prep,
	.finish = vfs_uring_read_finish,
	/*
	 * The same function as finish() uses, because the signatures already
	 * match and "give the descriptor back" is the whole of both. finish()
	 * calls it on the completion path, the planner calls it directly when
	 * a planned operation is abandoned before submission and no CQE will
	 * ever arrive; it is idempotent, so the two can never double free.
	 */
	.release = vfs_uring_release_fd,
};

static const struct fsal_uring_fop_desc vfs_uring_desc_write = {
	.name = "WRITE",
	.kernel_opcode = IORING_OP_WRITEV,
	/* Ordinary POSIX op on a real descriptor: the kernel
	 * enforces permissions, so it must see the client.
	 */
	.pers_resolve = fsal_uring_pers_caller,
	.prep = vfs_uring_write_prep,
	.finish = vfs_uring_write_finish,
	.release = vfs_uring_release_fd,
};

static const struct fsal_uring_fop_desc vfs_uring_desc_sync = {
	.name = "SYNC",
	/*
	 * FSYNC, not SYNC_FILE_RANGE: see vfs_uring_sync_sqe().  An NFS
	 * COMMIT needs the durability guarantee that only fsync() gives.
	 */
	.kernel_opcode = IORING_OP_FSYNC,
	/* Ordinary POSIX op on a real descriptor: the kernel
	 * enforces permissions, so it must see the client.
	 */
	.pers_resolve = fsal_uring_pers_caller,
	.prep = vfs_uring_sync_prep,
	.finish = vfs_uring_sync_finish,
	.release = vfs_uring_release_fd,
};

static const struct fsal_uring_fop_desc vfs_uring_desc_statx = {
	.name = "STATX",
	.kernel_opcode = IORING_OP_STATX,
	/* Ordinary POSIX op on a real descriptor: the kernel
	 * enforces permissions, so it must see the client.
	 */
	.pers_resolve = fsal_uring_pers_caller,
	.prep = vfs_uring_statx_prep,
	.finish = vfs_uring_statx_finish,
	.release = vfs_uring_release_fd,
};

static const struct fsal_uring_fop_desc vfs_uring_desc_close = {
	.name = "CLOSE",
	.kernel_opcode = IORING_OP_CLOSE,
	/*
	 * Present but refused: prep() returns ERR_FSAL_NOTSUPP and
	 * vfs_uring_op_supported() answers false. See
	 * vfs_uring_close_prep() for why an asynchronous close cannot be
	 * made safe from inside this module.
	 */
	/*
	 * Daemon credentials. close(2) is authorised when the descriptor is
	 * opened, not when it is closed, so there is nothing for a client
	 * identity to decide here and registering one would be wasted work.
	 */
	.pers_resolve = fsal_uring_pers_none,
	.prep = vfs_uring_close_prep,
	.finish = vfs_uring_close_finish,
	.release = vfs_uring_release_fd,
};

static const struct fsal_uring_fop_desc vfs_uring_desc_fallocate = {
	.name = "FALLOCATE",
	.kernel_opcode = IORING_OP_FALLOCATE,
	/* Ordinary POSIX op on a real descriptor: the kernel
	 * enforces permissions, so it must see the client.
	 */
	.pers_resolve = fsal_uring_pers_caller,
	.prep = vfs_uring_falloc_prep,
	.finish = vfs_uring_falloc_finish,
	.release = vfs_uring_release_fd,
};

/* -------------------------------------------------------------------------
 * Credential primitives
 * -------------------------------------------------------------------------
 * Registering a personality means impersonating the client just long
 * enough for the kernel to snapshot current_cred().  VFS must do that
 * through its own pair, not the generic fsal_set_credentials():
 *
 *   - vfs_set_credentials() honours the export's only_one_user mode, in
 *     which impersonation goes through a different call entirely and can
 *     legitimately fail;
 *   - vfs_restore_ganesha_credentials() is a no-op in that mode, so an
 *     unconditional generic restore would be wrong.
 *
 * Using the generic pair here would snapshot an identity the synchronous
 * VFS path would never have assumed.
 */

static bool vfs_uring_cred_set(const struct user_cred *creds, void *arg)
{
	return vfs_set_credentials(creds, (const struct fsal_module *)arg);
}

static void vfs_uring_cred_restore(void *arg)
{
	vfs_restore_ganesha_credentials((const struct fsal_module *)arg);
}

static struct uring_pers_cred_ops vfs_uring_cred_ops = {
	.set = vfs_uring_cred_set,
	.restore = vfs_uring_cred_restore,
	/* .arg is the fsal_module, filled in by vfs_uring_fops_init(). */
};

const struct uring_pers_cred_ops *vfs_uring_cred_ops_get(void)
{
	/* .arg is only valid once vfs_uring_fops_init() has run, which
	 * happens exactly when the engine comes up. Every caller is on a
	 * path that already established that, so there is nothing to guard
	 * here, but returning NULL rather than a half-built pair is the
	 * safer answer if that ever stops being true.
	 */
	return vfs_uring_cred_ops.arg != NULL ? &vfs_uring_cred_ops : NULL;
}

/* -------------------------------------------------------------------------
 * Support override
 * -------------------------------------------------------------------------
 */

/**
 * @brief Per-FSAL support override for FSAL_VFS_URING.
 *
 * Every operation this module publishes is expressed as a plain generic
 * opcode against a plain POSIX descriptor, so for all but one of them there
 * is nothing the kernel probe in fsal_uring_fop_get() has not already
 * decided correctly.
 *
 * The exception is CLOSE, and it is exactly the case this hook exists for:
 * the kernel implements IORING_OP_CLOSE perfectly well, but this FSAL
 * cannot hand a descriptor to the kernel to close while the rest of its
 * descriptor bookkeeping assumes the descriptor comes back. Answering false
 * here keeps the planner from even reaching vfs_uring_close_prep(), which
 * refuses as well; the full reasoning is recorded there.
 *
 * The other direction of this hook, for reference: FSAL_GPFS drives its
 * operations as IORING_OP_URING_CMD with private OPENHANDLE_* commands,
 * records FSAL_URING_OPCODE_NONE in the descriptor so the generic probe is
 * skipped entirely, and returns true here for the commands its kernel
 * module actually implements.
 */
bool vfs_uring_op_supported(struct fsal_export *exp, enum fsal_uring_fop fop)
{
	(void)exp;

	switch (fop) {
	case FSAL_URING_FOP_READ:
	case FSAL_URING_FOP_WRITE:
	case FSAL_URING_FOP_SYNC:
	case FSAL_URING_FOP_STATX:
	case FSAL_URING_FOP_FALLOCATE:
		return true;
	case FSAL_URING_FOP_CLOSE:
	case FSAL_URING_FOP_LAST:
		break;
	}

	return false;
}

/* -------------------------------------------------------------------------
 * Vector assembly and publication
 * -------------------------------------------------------------------------
 */

void vfs_uring_fops_init(struct vfs_uring_fsal_module *module)
{
	struct fsal_uring_fops *fops = &module->fops;

	fops->desc[FSAL_URING_FOP_READ] = &vfs_uring_desc_read;
	fops->desc[FSAL_URING_FOP_WRITE] = &vfs_uring_desc_write;
	fops->desc[FSAL_URING_FOP_SYNC] = &vfs_uring_desc_sync;
	fops->desc[FSAL_URING_FOP_STATX] = &vfs_uring_desc_statx;
	fops->desc[FSAL_URING_FOP_CLOSE] = &vfs_uring_desc_close;
	fops->desc[FSAL_URING_FOP_FALLOCATE] = &vfs_uring_desc_fallocate;

	fops->op_supported = vfs_uring_op_supported;
	fops->engine = module->uring_engine;

	/* vfs_set_credentials() needs the module to read only_one_user. */
	vfs_uring_cred_ops.arg = &module->vfs.module;
	fops->cred_ops = &vfs_uring_cred_ops;
}

void vfs_uring_export_set_fops(struct fsal_export *exp)
{
	if (exp == NULL)
		return;

	/* No engine means no asynchronous path at all. Leaving uring_fops
	 * NULL is what tells the planner to stay on the synchronous handlers
	 * for this export, so publishing an engine-less vector would be
	 * strictly worse than publishing nothing.
	 */
	if (VFS_URING.fops.engine == NULL) {
		LogUring(NIV_DEBUG,
			 "VFS_URING no engine, export keeps the synchronous path");
		return;
	}

	exp->uring_fops = &VFS_URING.fops;

	LogUring(NIV_DEBUG, "VFS_URING published the io_uring fops vector");
}

/* -------------------------------------------------------------------------
 * Handle ops overrides
 * -------------------------------------------------------------------------
 */

/**
 * @brief Replace the handle ops FSAL_VFS_URING drives asynchronously.
 *
 * Called right after vfs_handle_ops_init(), on this module's own copy of
 * the ops table (each module gets its own; see alloc_handle() in
 * FSAL_VFS/handle.c), so anything replaced here affects only handles
 * created by FSAL_VFS_URING and never plain FSAL_VFS.
 *
 * read2() and write2() are the whole data path, and they are overridden
 * unconditionally even though this runs at MODULE_INIT, before init_config()
 * has had a chance to build the engine.  The handlers resolve the engine per
 * call and delegate straight back to vfs_read2()/vfs_write2() when there is
 * none, which keeps the one decision -- "is io_uring usable for this
 * export?" -- in one place and makes it answerable at the only time it is
 * actually known.
 *
 * The vector above stays as it was: it is reached through
 * struct fsal_export::uring_fops by the NFSv4 compound planner, which batches
 * whole chains of operations into a single submission.  The two paths are
 * complementary rather than alternative -- a COMPOUND the planner batches
 * never reaches read2() at all, and everything else now does so
 * asynchronously instead of falling back to a blocking syscall.
 *
 * commit2() is deliberately not overridden.  Its signature is synchronous
 * (it returns the status rather than taking a callback), so submitting the
 * fsync through the engine would only mean blocking the worker thread on a
 * CQE instead of on the syscall.  An NFS stable WRITE does not go through it
 * -- that is write2() with fsal_stable set, which does get its fsync linked
 * behind the data -- so the hot path is covered; only an explicit COMMIT
 * operation still blocks.
 */
void vfs_uring_handle_ops_override(struct fsal_obj_ops *ops)
{
	ops->read2 = vfs_uring_read2;
	ops->write2 = vfs_uring_write2;
}

#endif /* USE_FSAL_URING */
