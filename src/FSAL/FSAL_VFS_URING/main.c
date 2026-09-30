// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2026, IBM. All rights reserved.
 *
 * Derived from FSAL/FSAL_VFS/vfs/main-c.in.cmake
 *   Copyright (C) Panasas Inc., 2011
 *   Author: Jim Lieb jlieb@panasas.com
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
 * @file  FSAL/FSAL_VFS_URING/main.c
 * @brief FSAL_VFS_URING module core functions.
 *
 * FSAL_VFS_URING is FSAL_VFS with an io_uring engine bolted on.  It is built
 * from the same sources with a different main.c, the way FSAL_LUSTRE and
 * FSAL_DUMMYLUSTRE already are, so that plain FSAL_VFS carries no io_uring
 * state and no io_uring code at all.
 *
 * Unlike those two, this is a hand-written main.c rather than a
 * configure_file() template: there is exactly one flavour of it, and it has
 * config items and an engine lifecycle that the shared template does not.
 */

#include "config.h"

#include <libgen.h> /* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>

/* vfs_uring_methods.h pulls in liburing.h transitively. Pulling it in first
 * and dropping the UNUSED it may define keeps common_utils.h free to install
 * Ganesha's function-like version; see FSAL/fsal_uring.c.
 */
#include <liburing.h>
#undef UNUSED

#include "gsh_list.h"
#include "fsal.h"
#include "FSAL/fsal_init.h"
#include "fsal_handle_syscalls.h"
#include "vfs_methods.h"
#include "vfs_uring_methods.h"

/* defined the set of attributes supported with POSIX */
#ifndef ENABLE_VFS_ACL
#define VFS_URING_SUPPORTED_ATTRIBUTES \
	((const attrmask_t)(ATTRS_POSIX | ATTR4_XATTR | ATTR4_FS_LOCATIONS))
#else
#define VFS_URING_SUPPORTED_ATTRIBUTES                             \
	((const attrmask_t)(ATTRS_POSIX | ATTR_ACL | ATTR4_XATTR | \
			    ATTR4_FS_LOCATIONS))
#endif

static const char myname[] = "VFS_URING";

/* my module private storage
 */

struct vfs_uring_fsal_module VFS_URING = {
	.vfs = {
		.module = {
			.fs_info = {
				.maxfilesize = INT64_MAX,
				.maxlink = _POSIX_LINK_MAX,
				.maxnamelen = 1024,
				.maxpathlen = 1024,
				.no_trunc = true,
				.chown_restricted = true,
				.case_insensitive = false,
				.case_preserving = true,
				.lock_support = false,
				.lock_support_async_block = false,
				.named_attr = true,
				.unique_handles = true,
				.acl_support = FSAL_ACLSUPPORT_ALLOW,
				.homogenous = true,
				.supported_attrs =
					VFS_URING_SUPPORTED_ATTRIBUTES,
				.maxread = FSAL_MAXIOSIZE,
				.maxwrite = FSAL_MAXIOSIZE,
				.link_supports_permission_checks = false,
				.expire_time_parent = -1,
				.xattr_support = true,
			}
		},
		.only_one_user = false
	}
};

static struct config_item vfs_uring_params[] = {
	CONF_ITEM_BOOL("link_support", true, vfs_uring_fsal_module,
		       vfs.module.fs_info.link_support),
	CONF_ITEM_BOOL("symlink_support", true, vfs_uring_fsal_module,
		       vfs.module.fs_info.symlink_support),
	CONF_ITEM_BOOL("cansettime", true, vfs_uring_fsal_module,
		       vfs.module.fs_info.cansettime),
	CONF_ITEM_UI64("maxread", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       vfs_uring_fsal_module, vfs.module.fs_info.maxread),
	CONF_ITEM_UI64("maxwrite", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       vfs_uring_fsal_module, vfs.module.fs_info.maxwrite),
	CONF_ITEM_MODE("umask", 0, vfs_uring_fsal_module,
		       vfs.module.fs_info.umask),
	CONF_ITEM_BOOL("auth_xdev_export", false, vfs_uring_fsal_module,
		       vfs.module.fs_info.auth_exportpath_xdev),
	CONF_ITEM_BOOL("only_one_user", false, vfs_uring_fsal_module,
		       vfs.only_one_user),
	/* Use the shared io_uring engine instead of the synchronous
	 * syscalls. Left off, this FSAL is byte-for-byte FSAL_VFS.
	 */
	CONF_ITEM_BOOL("io_uring", false, vfs_uring_fsal_module, uring_enable),
	CONF_ITEM_UI32("io_uring_queue_depth", 8, 4096, 256,
		       vfs_uring_fsal_module, uring_queue_depth),
	CONF_ITEM_UI32("io_uring_rings", 1, 512, 1, vfs_uring_fsal_module,
		       uring_rings),
	/* Allocate one ring per online CPU and direct submissions by CPU. */
	CONF_ITEM_BOOL("io_uring_per_cpu_rings", false, vfs_uring_fsal_module,
		       uring_per_cpu_rings),
	/* Registered ("fixed") read buffers. The pool is size-classed: it
	 * holds power-of-two classes from io_uring_buffer_min up to
	 * io_uring_buffer_max, and a read is served from the smallest class
	 * that fits it, so a small read does not tie up a buffer sized for
	 * the largest one.
	 *
	 * io_uring_pool_mb is the whole pinned budget and is the knob to
	 * turn; the engine divides it across the classes. The memory is
	 * locked for the life of the server, so it is bounded by
	 * RLIMIT_MEMLOCK. Set io_uring_buffer_min to 0 to run with no pool,
	 * in which case reads use ordinary heap buffers.
	 *
	 * Reads larger than io_uring_buffer_max still work, they just use a
	 * heap buffer and the non-fixed opcode. Set io_uring_buffer_max to
	 * match the largest rsize clients actually negotiate.
	 */
	CONF_ITEM_UI64("io_uring_buffer_min", 0, 1048576, 4096,
		       vfs_uring_fsal_module, uring_buffer_min),
	CONF_ITEM_UI64("io_uring_buffer_max", 4096, FSAL_MAXIOSIZE, 1048576,
		       vfs_uring_fsal_module, uring_buffer_max),
	CONF_ITEM_UI32("io_uring_pool_mb", 1, 65536, 64, vfs_uring_fsal_module,
		       uring_pool_mb),
	/* Move submission onto a kernel poller thread. io_uring_submit()
	 * stops being a syscall, and the page cache lookup, readahead and
	 * bio submission the kernel does on the way in stop running on the
	 * worker thread that issued the read. Costs a kernel thread that
	 * spins for io_uring_sqpoll_idle_ms after the last SQE.
	 *
	 * Needs CAP_SYS_NICE; without it the engine logs and runs without.
	 */
	CONF_ITEM_BOOL("io_uring_sqpoll", false, vfs_uring_fsal_module,
		       uring_sqpoll),
	CONF_ITEM_UI32("io_uring_sqpoll_idle_ms", 1, 60000, 100,
		       vfs_uring_fsal_module, uring_sqpoll_idle_ms),
	/* Poller threads to run, capped at io_uring_rings. Rings share
	 * pollers, so this is not one per ring. One poller does every
	 * attached ring's submission work and so tops out at a single CPU;
	 * if "iou-sqp" sits at 100% in top -H, raise this.
	 */
	CONF_ITEM_UI32("io_uring_sqpoll_groups", 1, 512, 1,
		       vfs_uring_fsal_module, uring_sqpoll_groups),
	/* First CPU of the run reserved for pollers; poller n lands on
	 * io_uring_sqpoll_cpu + n. Keep it clear of the worker threads or
	 * they compete for a core. -1 leaves placement to the scheduler.
	 */
	CONF_ITEM_I32("io_uring_sqpoll_cpu", -1, 4095, -1,
		      vfs_uring_fsal_module, uring_sqpoll_cpu),
	/* Diagnostic. Tags every SQE with IOSQE_ASYNC so the kernel hands it
	 * to an io-wq worker instead of trying it inline. Left off, the
	 * kernel runs whatever cannot block inline and defers only the wait,
	 * which is cheaper; turned on, a page cache hit costs a thread
	 * handoff too. Useful for measuring how much of the read path is
	 * running on the worker threads, not for production.
	 */
	CONF_ITEM_BOOL("io_uring_force_async", false, vfs_uring_fsal_module,
		       uring_force_async),
	/* Cap io-wq workers per ring. 0 keeps the kernel default of
	 * min(nr_cpus * 4, RLIMIT_NPROC / 2), which is usually far more
	 * than a file server wants to let loose.
	 */
	CONF_ITEM_UI32("io_uring_iowq_max_workers", 0, 4096, 0,
		       vfs_uring_fsal_module, uring_iowq_max_workers),
	/* Adaptive submission coalescing: hold an SQE briefly so several
	 * threads' I/Os ride into the kernel on one io_uring_submit().
	 *
	 * Off by default, and deliberately its own switch rather than a
	 * tuning value. Setting io_uring_coalesce_bucket to 1 does NOT
	 * disable it: above 4 * cif_threshold outstanding the bucket is
	 * recomputed as base + outstanding/(2 * cif), which grows whatever
	 * the base is, so submission would start being held back exactly
	 * when the ring is busiest.
	 *
	 * Turning it on trades latency for syscall count, and the waiting
	 * is done by spinning on sched_yield() in the submitting thread, so
	 * measure before enabling it.
	 */
	CONF_ITEM_BOOL("io_uring_coalesce", false, vfs_uring_fsal_module,
		       uring_coalesce),
	CONF_ITEM_UI32("io_uring_coalesce_bucket", 1, 4096, 1,
		       vfs_uring_fsal_module, uring_coalesce_bucket),
	CONF_ITEM_UI32("io_uring_coalesce_max_bucket", 1, 4096, 64,
		       vfs_uring_fsal_module, uring_coalesce_max_bucket),
	CONF_ITEM_UI32("io_uring_coalesce_timeout_us", 1, 1000000, 100,
		       vfs_uring_fsal_module, uring_coalesce_timeout_us),
	CONF_ITEM_UI32("io_uring_coalesce_outstanding_max", 1, 65536, 128,
		       vfs_uring_fsal_module, uring_coalesce_outstanding_max),
	CONF_ITEM_UI32("io_uring_coalesce_cif_threshold", 1, 4096, 4,
		       vfs_uring_fsal_module, uring_coalesce_cif_threshold),
	CONFIG_EOL
};

struct config_block vfs_uring_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.vfs_uring",
	.blk_desc.name = "VFS_URING",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = vfs_uring_params,
	.blk_desc.u.blk.commit = noop_conf_commit,
	.mem_comp = MEM_COMP_CONFIG
};

/* Module methods
 */

/**
 * @brief Create an export and publish the io_uring vector on it.
 *
 * vfs_create_export() is shared with FSAL_VFS and knows nothing about
 * io_uring, so the vector is attached here.  On success it has left the new
 * export in op_ctx->fsal_export; on failure there is nothing to attach it
 * to.
 */
static fsal_status_t vfs_uring_create_export(
	struct fsal_module *fsal_hdl, void *parse_node,
	struct config_error_type *err_type, const struct fsal_up_vector *up_ops)
{
	fsal_status_t status;

	status = vfs_create_export(fsal_hdl, parse_node, err_type, up_ops);

	if (FSAL_IS_ERROR(status))
		return status;

	vfs_uring_export_set_fops(op_ctx->fsal_export);

	return status;
}

/* init_config
 * must be called with a reference taken (via lookup_fsal)
 */

static fsal_status_t init_config(struct fsal_module *fsal_hdl,
				 config_file_t config_struct,
				 struct config_error_type *err_type)
{
	struct vfs_uring_fsal_module *uring_module = vfs_uring_module(fsal_hdl);
	struct vfs_fsal_module *vfs_module = &uring_module->vfs;
	int prev_errors = err_type->errors;

#ifdef F_OFD_GETLK
	int fd, rc;
	struct flock lock;
	char *temp_name;
#endif

#ifdef F_OFD_GETLK
	/* If on a system that might support OFD locks, verify them.
	 * Only if they exist will we declare lock support.
	 */
	LogInfo(COMPONENT_FSAL, "FSAL_VFS_URING testing OFD Locks");
	temp_name = gsh_strdup("/tmp/ganesha.nfsd.locktestXXXXXX",
			       MEM_COMP_TRANSIENT);
	fd = mkstemp(temp_name);
	if (fd >= 0) {
		lock.l_whence = SEEK_SET;
		lock.l_type = F_RDLCK;
		lock.l_start = 0;
		lock.l_len = 0;
		lock.l_pid = 0;

		rc = fcntl(fd, F_OFD_GETLK, &lock);

		if (rc == 0)
			vfs_module->module.fs_info.lock_support = true;
		else
			LogInfo(COMPONENT_FSAL, "Could not use OFD locks");

		close(fd);
		unlink(temp_name);
	} else {
		LogCrit(COMPONENT_FSAL,
			"Could not create file %s to test OFD locks",
			temp_name);
	}
	gsh_free(temp_name, MEM_COMP_TRANSIENT);
#endif

	if (vfs_module->module.fs_info.lock_support)
		LogInfo(COMPONENT_FSAL, "FSAL_VFS_URING enabling OFD Locks");
	else
		LogInfo(COMPONENT_FSAL,
			"FSAL_VFS_URING disabling lock support");

	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes default = 0x%" PRIx64,
		     vfs_module->module.fs_info.supported_attrs);

	(void)load_config_from_parse(config_struct, &vfs_uring_param,
				     uring_module, true, err_type);

	/* Check for actual errors in vfs_uring_param parsing.
	 * err_type could have errors from previous block
	 * parsing also.
	 */
	if ((err_type->errors > prev_errors) &&
	    !config_error_is_harmless(err_type))
		return fsalstat(ERR_FSAL_INVAL, 0);

	if (uring_module->uring_enable && uring_module->uring_engine == NULL) {
		/* The tunables the core engine shares with every other
		 * consumer live in the nested base struct; pers_max is the
		 * only FSAL-level knob and is left at its default. Note the
		 * nesting: flat designators would land in the wrong fields.
		 */
		struct fsal_uring_params uring_params = {
			.base = {
				.name = myname,
				.queue_depth = uring_module->uring_queue_depth,
				.nr_rings = uring_module->uring_rings,
				.per_cpu_rings =
					uring_module->uring_per_cpu_rings,
				.buf_min = uring_module->uring_buffer_min,
				.buf_max = uring_module->uring_buffer_max,
				.pool_bytes =
					(size_t)uring_module->uring_pool_mb *
					1024 * 1024,
				.sqpoll = uring_module->uring_sqpoll,
				.sqpoll_idle_ms =
					uring_module->uring_sqpoll_idle_ms,
				.sqpoll_groups =
					uring_module->uring_sqpoll_groups,
				.sqpoll_cpu = uring_module->uring_sqpoll_cpu,
				.force_async = uring_module->uring_force_async,
				.iowq_max_workers =
					uring_module->uring_iowq_max_workers,
				/* Off unless explicitly configured; the
				 * tunables below are inert while it is.
				 */
				.coalesce_enable =
					uring_module->uring_coalesce,
				.coalesce_bucket_io_count =
					uring_module->uring_coalesce_bucket,
				.coalesce_max_bucket_io_count =
					uring_module->uring_coalesce_max_bucket,
				.coalesce_timeout_us =
					uring_module->uring_coalesce_timeout_us,
				.coalesce_outstanding_max =
					uring_module
					->uring_coalesce_outstanding_max,
				.coalesce_cif_threshold =
					uring_module
						->uring_coalesce_cif_threshold,
			},
		};
		int rc;

		rc = fsal_uring_engine_create(&uring_params,
					      &uring_module->uring_engine);

		if (rc < 0) {
			LogUring(NIV_WARN,
				 "%s: io_uring requested but unavailable (%s), staying on synchronous I/O",
				 myname, strerror(-rc));
			uring_module->uring_engine = NULL;
		} else {
			/* Reads can now be filled with registered buffers, so
			 * ask the core to stop pre-allocating one for us. See
			 * fsal_read2() and fsal_uring_read_buffer().
			 *
			 * This is only safe because vfs_uring_read2() is
			 * installed over vfs_read2() for every handle this
			 * module creates: the flag makes fsal_read2() hand
			 * read2() an iovec whose base is NULL, and plain
			 * vfs_read2() would pass that straight to preadv()
			 * and get EFAULT. Keep the two together.
			 *
			 * It is set here rather than at MODULE_INIT because
			 * it only holds once the engine exists -- without one
			 * vfs_uring_read2() delegates to vfs_read2(), which
			 * needs the core to allocate as it always did.
			 */
			vfs_module->module.fs_info.allocate_own_read_buffer =
				true;

			/* The vector can only be built once the engine it
			 * submits through exists.
			 */
			vfs_uring_fops_init(uring_module);

			/* Advertise that this FSAL can batch operations into
			 * a linked SQE chain. The NFSv4 compound engine gates
			 * on this via fsal_supports(fso_uring_batch); without
			 * it nfs4_uring_op_batchable() refuses everything and
			 * every compound silently runs the legacy sequential
			 * path, however the rest is configured.
			 */
			vfs_module->module.fs_info.uring_batch = true;

			LogUring(NIV_EVENT,
				 "%s: io_uring enabled,"
				 " queue_depth=%u rings=%u per_cpu=%s"
				 " buffer classes %" PRIu64
				 " to %" PRIu64
				 " bytes in %u MiB,"
				 " sqpoll=%s force_async=%s",
				 myname, uring_module->uring_queue_depth,
				 fsal_uring_nr_rings(
				 uring_module->uring_engine),
				 uring_module->uring_per_cpu_rings ? "yes"
								   : "no",
				 uring_module->uring_buffer_min,
				 uring_module->uring_buffer_max,
				 uring_module->uring_pool_mb,
				 uring_module->uring_sqpoll ? "yes" : "no",
				 uring_module->uring_force_async ? "yes"
								 : "no");
		}
	}

	display_fsinfo(&vfs_module->module);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes constant = 0x%" PRIx64,
		     VFS_URING_SUPPORTED_ATTRIBUTES);
	LogDebug(COMPONENT_FSAL,
		 "FSAL INIT: Supported attributes mask = 0x%" PRIx64,
		 vfs_module->module.fs_info.supported_attrs);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

MODULE_INIT void vfs_uring_init(void)
{
	int retval;
	struct fsal_module *myself = &VFS_URING.vfs.module;

	/*
	 * FSAL_ID_NO_PNFS rather than FSAL_ID_VFS: the id's only use is
	 * pnfs_fsal[id], and claiming FSAL_ID_VFS here would overwrite the
	 * entry FSAL_VFS installed when both modules are loaded. Neither
	 * module implements pNFS, so the slot is better left alone.
	 */
	retval = register_fsal(myself, myname, FSAL_MAJOR_VERSION,
			       FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS);
	if (retval != 0) {
		fprintf(stderr, "VFS_URING module failed to register");
		return;
	}
	myself->m_ops.create_export = vfs_uring_create_export;
	myself->m_ops.update_export = vfs_update_export;
	myself->m_ops.init_config = init_config;

	/* This module owns its own copy of the handle ops (alloc_handle()
	 * takes it from the module the export belongs to), so overriding
	 * here cannot affect handles created by FSAL_VFS.
	 */
	vfs_handle_ops_init(&VFS_URING.vfs.handle_ops);
	vfs_uring_handle_ops_override(&VFS_URING.vfs.handle_ops);
}

MODULE_FINI void vfs_uring_unload(void)
{
	int retval;

	retval = unregister_fsal(&VFS_URING.vfs.module);
	if (retval != 0) {
		fprintf(stderr, "VFS_URING module failed to unregister");
		return;
	}

	/* Unregistering the FSAL means no more I/O can be started, so it is
	 * safe to stop the reapers and give the pinned buffers back.
	 */
	if (VFS_URING.uring_engine != NULL) {
		LogUring(NIV_EVENT, "%s: shutting down the io_uring engine",
			 myname);
		fsal_uring_engine_destroy(VFS_URING.uring_engine);
		VFS_URING.uring_engine = NULL;
		VFS_URING.fops.engine = NULL;
	}
}
