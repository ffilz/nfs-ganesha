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
 * @file  FSAL/FSAL_VFS_URING/vfs_uring_methods.h
 * @brief Private declarations for the FSAL_VFS_URING module.
 *
 * FSAL_VFS_URING is FSAL_VFS plus an io_uring engine.  It is built from the
 * very same sources as FSAL_VFS (export.c, handle.c, file.c, ...) with a
 * different main.c, exactly the way FSAL_LUSTRE and FSAL_DUMMYLUSTRE are.
 * Only the module-level object differs:
 *
 *	struct vfs_uring_fsal_module {
 *		struct vfs_fsal_module vfs;	<- MUST be first
 *		... io_uring state ...
 *	};
 *
 * Because the plain VFS module struct is the first member, every shared
 * source file can keep doing
 *
 *	container_of(fsal_module, struct vfs_fsal_module, module)
 *
 * and reach the right object whichever of the two modules is loaded, while
 * plain FSAL_VFS carries no io_uring state at all and stays synchronous.
 *
 * The io_uring side of this module is a vector of per-operation handlers
 * (struct fsal_uring_fops, see FSAL/fsal_uring_fops.h) published on every
 * export this module creates.  The compound planner then drives it without
 * knowing anything about VFS:
 *
 *	vfs_uring_io.c		READ, WRITE, SYNC
 *	vfs_uring_meta.c	STATX, CLOSE, FALLOCATE
 *	vfs_uring_fd.c		descriptor acquisition and release
 *	vfs_uring_fops.c	the descriptors, the vector, the overrides
 */

#ifndef VFS_URING_METHODS_H
#define VFS_URING_METHODS_H

#include "config.h"

#ifdef USE_FSAL_URING

#include <stdbool.h>
#include <stdint.h>

#include "fsal_api.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
#include "vfs_methods.h"

/**
 * @brief FSAL_VFS_URING module private storage.
 */
struct vfs_uring_fsal_module {
	/**
	 * Plain VFS module state.  MUST be the first member: the sources
	 * shared with FSAL_VFS reach it with
	 * container_of(fsal_module, struct vfs_fsal_module, module).
	 */
	struct vfs_fsal_module vfs;

	/** Config: use the shared io_uring engine for read/write. */
	bool uring_enable;
	/** Config: SQ entries per ring. */
	uint32_t uring_queue_depth;
	/** Config: number of rings, each with its own completion thread. */
	uint32_t uring_rings;
	/** Config: create one ring per online CPU and route submits by CPU. */
	bool uring_per_cpu_rings;
	/** Config: smallest buffer size class, 0 disables the pool. */
	uint64_t uring_buffer_min;
	/** Config: largest buffer size class. */
	uint64_t uring_buffer_max;
	/** Config: total pinned budget for the buffer pool, in MiB. */
	uint32_t uring_pool_mb;
	/** Config: run submission on a kernel poller thread. */
	bool uring_sqpoll;
	/** Config: poller spin time before it parks, in ms. */
	uint32_t uring_sqpoll_idle_ms;
	/** Config: how many poller threads to run, capped at uring_rings. */
	uint32_t uring_sqpoll_groups;
	/** Config: CPU to pin the poller to, -1 to leave it unpinned. */
	int32_t uring_sqpoll_cpu;
	/** Config: force every I/O onto an io-wq worker (diagnostic). */
	bool uring_force_async;
	/** Config: cap on io-wq workers per ring, 0 for the kernel default. */
	uint32_t uring_iowq_max_workers;
	/** Config: hold SQEs back to batch them. Off by default. */
	bool uring_coalesce;
	/** Config: base SQE bucket size when coalescing is on. */
	uint32_t uring_coalesce_bucket;
	/** Config: adaptive ceiling for the bucket size. */
	uint32_t uring_coalesce_max_bucket;
	/** Config: age of the oldest held SQE before a forced flush, us. */
	uint32_t uring_coalesce_timeout_us;
	/** Config: hold new SQEs once outstanding I/O reaches this. */
	uint32_t uring_coalesce_outstanding_max;
	/** Config: minimum outstanding I/O before batching activates. */
	uint32_t uring_coalesce_cif_threshold;

	/** The engine, NULL when io_uring is off or could not be set up. */
	struct fsal_uring_engine *uring_engine;

	/**
	 * Per-operation vector published on every export this module
	 * creates.  Filled in by vfs_uring_fops_init() once the engine is
	 * up; left all-zero (and never published) when it is not.
	 */
	struct fsal_uring_fops fops;
};

/** The one and only instance, defined in main.c. */
extern struct vfs_uring_fsal_module VFS_URING;

/**
 * @brief Recover the module object from any struct fsal_module *
 *
 * Two hops, because the shared VFS sources only ever see the inner
 * struct vfs_fsal_module.
 */
static inline struct vfs_uring_fsal_module *
vfs_uring_module(struct fsal_module *fsal_hdl)
{
	return container_of(container_of(fsal_hdl, struct vfs_fsal_module,
					 module),
			    struct vfs_uring_fsal_module, vfs);
}

/**
 * @brief The module's io_uring engine, or NULL if io_uring is not in use
 */
static inline struct fsal_uring_engine *
vfs_uring_engine(struct fsal_module *fsal_hdl)
{
	return vfs_uring_module(fsal_hdl)->uring_engine;
}

/* -------------------------------------------------------------------------
 * vfs_uring_fops.c
 * -------------------------------------------------------------------------
 */

/**
 * @brief Turn a harvested CQE result into an fsal_status_t
 *
 * Shared by every finish() in this module so that the error mapping -- in
 * particular the -ECANCELED a linked SQE gets when an earlier SQE in the
 * chain failed -- is stated exactly once.
 */
fsal_status_t vfs_uring_cqe_status(const struct fsal_uring_fop_req *req);

/**
 * @brief Per-FSAL support override, see fsal_uring_fops::op_supported
 */
bool vfs_uring_op_supported(struct fsal_export *exp, enum fsal_uring_fop fop);

/**
 * @brief How this FSAL impersonates a client, for the io_uring paths
 *
 * VFS must use its own pair rather than the generic fsal_set_credentials()
 * one; the reasoning is recorded next to the definition. NULL before the
 * engine is up.
 */
const struct uring_pers_cred_ops *vfs_uring_cred_ops_get(void);

/**
 * @brief Build the operation vector on @a module once its engine is up
 */
void vfs_uring_fops_init(struct vfs_uring_fsal_module *module);

/**
 * @brief Publish this module's operation vector on @a exp
 */
void vfs_uring_export_set_fops(struct fsal_export *exp);

/**
 * @brief Replace the handle ops that FSAL_VFS_URING drives asynchronously
 */
void vfs_uring_handle_ops_override(struct fsal_obj_ops *ops);

/* -------------------------------------------------------------------------
 * vfs_uring_fd.c -- descriptor ownership across the asynchronous gap
 * -------------------------------------------------------------------------
 */

/**
 * @brief What one prepped operation holds on the handle until it completes.
 *
 * Allocated by vfs_uring_acquire_fd(), hung off
 * struct fsal_uring_fop_req::fsal_priv, and freed by vfs_uring_release_fd().
 * Opaque to everything outside this module.
 */
struct vfs_uring_fd_ref {
	/**
	 * Whatever fsal_start_io() settled on: the state's fd, the handle's
	 * global fd, or &temp_fd.fsal_fd below. This, not the raw int, is
	 * what fsal_complete_io() has to be given back.
	 */
	struct fsal_fd *out_fd;

	/**
	 * Heap home for a temporary descriptor. fsal_start_io() opens into
	 * this when neither the state fd nor the global fd is usable; the
	 * synchronous handlers can keep the equivalent on the stack, but an
	 * io_uring operation outlives the prep() frame.
	 */
	struct vfs_fd temp_fd;

	/**
	 * Share reservation to release on completion, or NULL when none was
	 * taken. Non-NULL only for a stateless operation that asked for a
	 * real access mode; see vfs_uring_acquire_fd().
	 */
	struct fsal_share *share;

	/** Mode @a share was reserved with, needed to undo it. */
	fsal_openflags_t share_access;
};

/**
 * @brief Resolve @a req->fd, taking whatever references that implies.
 *
 * Uses req->state, req->openflags and req->bypass the way the synchronous
 * handlers use their own arguments. On success req->fd is a usable
 * descriptor and req->fsal_priv owns the state needed to give it back; on
 * failure both are left clear and the operation must not be batched.
 *
 * Idempotent: an operation that fills more than one chain slot (a stable
 * WRITE, which is a WRITEV plus a linked fsync) may call this once per slot
 * and gets the same descriptor back, acquired once.
 */
fsal_status_t vfs_uring_acquire_fd(struct fsal_uring_fop_req *req);

/**
 * @brief Give back everything vfs_uring_acquire_fd() took. Idempotent.
 *
 * Doubles as the descriptors' release() hook: the signature is already
 * void (*)(struct fsal_uring_fop_req *).
 */
void vfs_uring_release_fd(struct fsal_uring_fop_req *req);

/* -------------------------------------------------------------------------
 * vfs_uring_io.c -- READ, WRITE, SYNC
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_read_prep(struct fsal_uring_fop_req *req,
				  struct uring_chain_op *slot);
fsal_status_t vfs_uring_read_finish(struct fsal_uring_fop_req *req);

fsal_status_t vfs_uring_write_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot);
fsal_status_t vfs_uring_write_finish(struct fsal_uring_fop_req *req);

fsal_status_t vfs_uring_sync_prep(struct fsal_uring_fop_req *req,
				  struct uring_chain_op *slot);
fsal_status_t vfs_uring_sync_finish(struct fsal_uring_fop_req *req);

/* -------------------------------------------------------------------------
 * vfs_uring_rw.c -- asynchronous read2()/write2()
 *
 * The per-operation entry points, as opposed to the chain handlers above:
 * these serve every request the NFSv4 compound planner did not batch, which
 * is all of NFSv3, READ_PLUS, and any COMPOUND the planner refused. Both
 * fall back to vfs_read2()/vfs_write2() when the module has no engine.
 * -------------------------------------------------------------------------
 */

void vfs_uring_read2(struct fsal_obj_handle *obj_hdl, bool bypass,
		     fsal_async_cb done_cb, struct fsal_io_arg *read_arg,
		     void *caller_arg);

void vfs_uring_write2(struct fsal_obj_handle *obj_hdl, bool bypass,
		      fsal_async_cb done_cb, struct fsal_io_arg *write_arg,
		      void *caller_arg);

/* -------------------------------------------------------------------------
 * vfs_uring_meta.c -- STATX, CLOSE, FALLOCATE
 * -------------------------------------------------------------------------
 */

fsal_status_t vfs_uring_statx_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot);
fsal_status_t vfs_uring_statx_finish(struct fsal_uring_fop_req *req);

fsal_status_t vfs_uring_close_prep(struct fsal_uring_fop_req *req,
				   struct uring_chain_op *slot);
fsal_status_t vfs_uring_close_finish(struct fsal_uring_fop_req *req);

fsal_status_t vfs_uring_falloc_prep(struct fsal_uring_fop_req *req,
				    struct uring_chain_op *slot);
fsal_status_t vfs_uring_falloc_finish(struct fsal_uring_fop_req *req);

#endif /* USE_FSAL_URING */
#endif /* VFS_URING_METHODS_H */
