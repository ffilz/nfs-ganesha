// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright (C) 2026, IBM
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
 */

/**
 * @file    FSAL/fsal_uring.c
 * @brief   FSAL Layer 2 adapter for the io_uring engine
 *
 * This file is intentionally thin.  All ring management, buffer pooling,
 * coalescing, SQPOLL and reaper-thread logic lives in
 * src/io_uring/core/uring_engine.c.  This file contains only:
 *
 * I/O path (read / write):
 *   - struct fsal_uring_engine      FSAL-level wrapper (core + pers registry)
 *   - struct fsal_uring_rw_ctx      per-op context passed as sqe_pre_arg
 *   - fsal_uring_rw_prep()          sqe_pre: fills SQE + stamps personality
 *   - fsal_uring_fsync_prep()       sqe_pre2: linked fsync + personality
 *   - fsal_uring_log_wrapper()      routes engine log calls to LogUring
 *   - fsal_uring_engine_create/destroy
 *   - fsal_uring_buf_get/put/max_size/release  (thin wrappers over uring_buf_*)
 *   - fsal_uring_read_buffer        zero-copy read buffer provisioning
 *   - fsal_uring_io_start           FSAL I/O submission
 *   - fsal_uring_io_done            CQE callback (static)
 *   - fsal_uring_io_finish          tail of completion (static)
 *   - fsal_uring_io_resume          FSAL_CLOSEFD resume
 *
 * Metadata async path (Sub-Task 7):
 *   - fsal_uring_meta_done()        generic CQE callback for metadata ops
 *   - fsal_uring_meta_start()       generic submit entry-point for metadata ops
 *   - fsal_uring_sync_range_prep()  SYNC_FILE_RANGE sqe_pre (vfs_commit2)
 *   - fsal_uring_falloc_prep()      FALLOCATE sqe_pre (vfs_fallocate)
 *   - fsal_uring_openat_prep()      OPENAT sqe_pre (vfs_open2 create)
 *   - fsal_uring_close_prep()       CLOSE sqe_pre (fire-and-forget fd close)
 *   - fsal_uring_statx_prep()       STATX sqe_pre (getattr hot path)
 *   - fsal_uring_rename_prep()      RENAMEAT sqe_pre (renamefile)
 *   - fsal_uring_unlink_prep()      UNLINKAT sqe_pre (file_unlink)
 *   - fsal_uring_mkdir_prep()       MKDIRAT sqe_pre (makedir)
 *   - fsal_uring_symlink_prep()     SYMLINKAT sqe_pre (makesymlink)
 *   - fsal_uring_linkat_prep()      LINKAT sqe_pre (linkfile)
 *
 * See include/FSAL/fsal_uring.h and include/FSAL/fsal_uring_meta.h for
 * the public API.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <liburing.h>
#undef UNUSED

#include "abstract_atomic.h"
#include "abstract_mem.h"
#include "common_utils.h"
#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
#include "FSAL/fsal_uring_meta.h"
#include "FSAL_URING/uring_pers.h"
#include "gsh_rpc.h" /* svc_resume_defer_begin()/_end() */
#include "io_uring/core/uring_engine.h"
#include "log.h"

/* -------------------------------------------------------------------------
 * FSAL-level engine wrapper
 * -------------------------------------------------------------------------
 */

/**
 * @brief FSAL-level engine wrapper
 *
 * Keeps the opaque core engine pointer and the optional credential registry
 * together.  The definition is not exposed in the public header — callers use
 * the fsal_uring_engine_* API only.
 */
struct fsal_uring_engine {
	/** Opcode-agnostic core engine. */
	struct uring_engine *core;
	/** FSAL credential registry, or NULL if init failed / not needed. */
	struct uring_pers_registry *pers;
	/** Name copy for log messages (avoids a round-trip through core). */
	char name[24];
};

/* -------------------------------------------------------------------------
 * Per-op FSAL context (sqe_pre_arg)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Context bundled into sqe_pre_arg for FSAL read/write ops.
 *
 * Stack-allocated in fsal_uring_io_start() and lives until
 * uring_engine_submit() returns.  At that point the SQE has been prepared
 * (sqe_pre was already called) and the engine no longer reads this struct.
 */
struct fsal_uring_rw_ctx {
	int fd;
	uint64_t offset;
	const struct iovec *iov;
	int iov_count;
	struct uring_buf *fixed_buf;
	size_t fixed_len;
	bool is_write;
	bool datasync;
	struct uring_pers *pers; /* may be NULL on fallback path */
};

/* -------------------------------------------------------------------------
 * Pluggable log callback wired at engine-create time
 * -------------------------------------------------------------------------
 */

/**
 * @brief Route engine log messages to Ganesha's COMPONENT_URING log.
 *
 * The engine uses the same integer severity constants as Ganesha's NIV_*
 * enum (URING_LOG_CRIT == NIV_CRIT etc.), so a direct cast is correct.
 */
static void fsal_uring_log_wrapper(void *ctx, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void fsal_uring_log_wrapper(void *ctx, int level, const char *fmt, ...)
{
	va_list ap;

	(void)ctx;

	if (!isLevel(COMPONENT_URING, (log_levels_t)level))
		return;

	va_start(ap, fmt);
	display_log_component_level(COMPONENT_URING, __FILE__, __LINE__,
				    __func__, (log_levels_t)level, fmt, ap);
	va_end(ap);
}

/* -------------------------------------------------------------------------
 * sqe_pre functions — SQE prep + personality stamping
 * -------------------------------------------------------------------------
 */

/**
 * @brief sqe_pre callback for FSAL reads and writes.
 *
 * Called by the core engine inside submit_lock with the already-allocated
 * SQE slot and the index of the ring it belongs to.  Uses the fixed-buffer
 * opcode when a registered buffer is available, otherwise falls back to the
 * scatter/gather opcode.  Stamps sqe->personality via uring_pers_attach().
 */
static void fsal_uring_rw_prep(struct io_uring_sqe *sqe, void *arg,
			       unsigned int ring_idx)
{
	struct fsal_uring_rw_ctx *ctx = arg;

	if (ctx->is_write) {
		if (ctx->fixed_buf != NULL)
			io_uring_prep_write_fixed(sqe, ctx->fd,
						  ctx->fixed_buf->addr,
						  ctx->fixed_len, ctx->offset,
						  ctx->fixed_buf->index);
		else
			io_uring_prep_writev(sqe, ctx->fd, ctx->iov,
					     ctx->iov_count, ctx->offset);
	} else {
		if (ctx->fixed_buf != NULL)
			io_uring_prep_read_fixed(sqe, ctx->fd,
						 ctx->fixed_buf->addr,
						 ctx->fixed_len, ctx->offset,
						 ctx->fixed_buf->index);
		else
			io_uring_prep_readv(sqe, ctx->fd, ctx->iov,
					    ctx->iov_count, ctx->offset);
	}

	/* Stamp the client credential into the SQE. ctx->pers is never NULL
	 * here: fsal_uring_io_start() refuses to submit without one rather
	 * than let the SQE run as the daemon, so there is no "personality 0"
	 * case to reason about on this path. */
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}

/**
 * @brief sqe_pre2 callback for the optional linked fsync.
 *
 * Called for the second SQE of a stable write.  The engine has already set
 * IOSQE_IO_LINK on the first SQE before invoking this.
 */
static void fsal_uring_fsync_prep(struct io_uring_sqe *sqe, void *arg,
				  unsigned int ring_idx)
{
	struct fsal_uring_rw_ctx *ctx = arg;

	io_uring_prep_fsync(sqe, ctx->fd,
			    ctx->datasync ? IORING_FSYNC_DATASYNC : 0);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}

/* -------------------------------------------------------------------------
 * Engine lifecycle
 * -------------------------------------------------------------------------
 */

int fsal_uring_engine_create(const struct fsal_uring_params *params,
			     struct fsal_uring_engine **engine_out)
{
	struct fsal_uring_params defaults = { 0 };
	struct fsal_uring_engine *engine;
	struct uring_engine_params base;
	int rc;

	if (params == NULL)
		params = &defaults;

	/* Copy the base params and wire the FSAL log callback. */
	base = params->base;
	base.log_fn = fsal_uring_log_wrapper;
	base.log_ctx = NULL;
	if (base.mem_comp == 0)
		base.mem_comp = MEM_COMP_FSAL;

	/*
	 * Batch the hand-off of completed requests back to the RPC worker
	 * pool. Every completion on this path ends in svc_resume(), and
	 * unbatched that is a queue mutex plus a futex wake per CQE, on a
	 * thread that routinely harvests several CQEs per wakeup. Bracketed
	 * like this, one harvest costs one of each.
	 *
	 * Wired here rather than in each FSAL because it is a property of
	 * how Ganesha dispatches requests, not of any filesystem. The core
	 * engine knows nothing about it; see uring_engine_params::reap_begin.
	 *
	 * Only available when libntirpc exposes the batch svc_resume API.
	 * Without it the engine leaves reap_begin/reap_end NULL and each
	 * svc_resume() submits individually, which is correct but unoptimised.
	 */
#ifdef HAVE_SVC_RESUME_BATCH
	base.reap_begin = svc_resume_defer_begin;
	base.reap_end = svc_resume_defer_end;
#endif

	engine = gsh_calloc(1, sizeof(*engine), MEM_COMP_FSAL);

	(void)snprintf(engine->name, sizeof(engine->name), "%s",
		       base.name != NULL ? base.name : "fsal_uring");

	rc = uring_engine_create(&base, &engine->core);
	if (rc != 0) {
		gsh_free(engine, MEM_COMP_FSAL);
		return rc;
	}

	if (uring_pers_create(engine->core, params->pers_max, &engine->pers) !=
	    0) {
		LogUring(NIV_WARN,
			 "%s: personality registry init failed; client I/O"
			 " cannot be stamped with a credential, so every"
			 " submission will be declined and served"
			 " synchronously instead",
			 engine->name);
		engine->pers = NULL;
	}

	*engine_out = engine;
	return 0;
}

void fsal_uring_engine_destroy(struct fsal_uring_engine *engine)
{
	if (engine == NULL)
		return;

	if (engine->pers != NULL) {
		uring_pers_log_stats(engine->pers);
		uring_pers_destroy(engine->pers);
		engine->pers = NULL;
	}

	uring_engine_destroy(engine->core);
	engine->core = NULL;

	gsh_free(engine, MEM_COMP_FSAL);
}

/* -------------------------------------------------------------------------
 * Buffer pool wrappers
 * -------------------------------------------------------------------------
 */

size_t fsal_uring_buf_max_size(struct fsal_uring_engine *engine)
{
	return uring_buf_max_size(engine->core);
}

struct uring_buf *fsal_uring_buf_get(struct fsal_uring_engine *engine,
				     size_t len)
{
	return uring_buf_get(engine->core, len);
}

void fsal_uring_buf_put(struct uring_buf *buf)
{
	uring_buf_put(buf);
}

void fsal_uring_buf_release(void *release_data)
{
	uring_buf_put(release_data);
}

/** iov_release for the plain heap fallback of fsal_uring_read_buffer(). */
static void fsal_uring_heap_release(void *release_data)
{
	gsh_free(release_data, MEM_COMP_IO_BUFFER);
}

/* -------------------------------------------------------------------------
 * Stats
 * -------------------------------------------------------------------------
 */

unsigned int fsal_uring_poll(struct fsal_uring_engine *engine,
			     unsigned int ring_idx)
{
	return uring_engine_poll(engine->core, ring_idx);
}

unsigned int fsal_uring_nr_rings(struct fsal_uring_engine *engine)
{
	return engine != NULL ? uring_engine_nr_rings(engine->core) : 0;
}

void fsal_uring_log_stats(struct fsal_uring_engine *engine)
{
	struct uring_engine_stats stats;

	uring_engine_get_stats(engine->core, &stats);

	LogUring(NIV_INFO,
		 "%s: submitted=%" PRIu64 " completed=%" PRIu64
		 " failed=%" PRIu64 " sq_full=%" PRIu64 " buf_empty=%" PRIu64,
		 engine->name, stats.sqe_submitted, stats.cqe_completed,
		 stats.cqe_failed, stats.sq_full_rejections,
		 stats.buf_pool_misses);
}

/* -------------------------------------------------------------------------
 * Zero-copy read buffer provisioning
 * -------------------------------------------------------------------------
 */

struct uring_buf *fsal_uring_read_buffer(struct fsal_uring_engine *engine,
					 struct fsal_io_arg *read_arg)
{
	struct uring_buf *buf;
	size_t len;

	/* The caller supplied a buffer (RDMA, or a non zero-copy caller), or
	 * this is a scatter read. Either way the iovec is already usable and
	 * its release, if any, belongs to somebody else.
	 */
	if (read_arg->iov_count != 1 || read_arg->iov[0].iov_base != NULL) {
		LogUring(NIV_FULL_DEBUG,
			 "%s: caller supplied %d iovec(s), using them as-is",
			 engine->name, read_arg->iov_count);
		return NULL;
	}

	len = read_arg->iov[0].iov_len;

	/* Served from the smallest size class that fits, so a small read does
	 * not tie up a buffer sized for the largest one.
	 */
	buf = uring_buf_get(engine->core, len);

	if (buf != NULL) {
		read_arg->iov[0].iov_base = buf->addr;
		read_arg->iov_release = fsal_uring_buf_release;
		read_arg->release_data = buf;

		LogUring(NIV_FULL_DEBUG,
			 "%s: read of %zu bytes uses fixed buffer %u"
			 " (%zu byte class)",
			 engine->name, len, buf->index, buf->len);
		return buf;
	}

	/* No registered buffer for this one: fall back to an ordinary heap
	 * buffer. The read still goes through io_uring, just without a
	 * pre-pinned buffer.
	 */
	read_arg->iov[0].iov_base = gsh_malloc(len, MEM_COMP_IO_BUFFER);
	read_arg->iov_release = fsal_uring_heap_release;
	read_arg->release_data = read_arg->iov[0].iov_base;

	LogUring(NIV_FULL_DEBUG, "%s: read of %zu bytes uses heap buffer %p",
		 engine->name, len, read_arg->iov[0].iov_base);

	return NULL;
}

/* -------------------------------------------------------------------------
 * Layer 2 FSAL I/O adapter
 * -------------------------------------------------------------------------
 */

/**
 * @brief Tail of an FSAL I/O: fsal_complete_io, release share, call done_cb
 *
 * Runs with an op_ctx already pushed by the caller, which it releases.
 * Frees @a req, and with it the FSAL's enclosing per-request state.
 */
static void fsal_uring_io_finish(struct fsal_uring_io_req *req)
{
	fsal_status_t status2;

	status2 = fsal_complete_io(req->obj_hdl, req->out_fd);

	LogUring(NIV_FULL_DEBUG, "req %p: fsal_complete_io returned %s", req,
		 fsal_err_txt(status2));

	if (req->share != NULL) {
		/* I/O was done without a state, so release the temporary
		 * share reservation.
		 */
		update_share_counters_locked(req->obj_hdl, req->share,
					     req->share_access, FSAL_O_CLOSED);
	}

	LogUring(NIV_DEBUG,
		 "req %p: %s done, io_amount=%zu status=%s, calling done_cb %p",
		 req, req->is_write ? "write" : "read", req->io_arg->io_amount,
		 fsal_err_txt(req->status), (void *)(uintptr_t)req->done_cb);

	req->done_cb(req->obj_hdl, req->status, req->io_arg, req->caller_arg);

	release_op_context();

	/* Release the personality reference taken at io_start().
	 * Safe from the reaper thread: uring_pers_put() is lock-free once
	 * refcnt is still > 0 after the decrement. */
	uring_pers_put(req->pers);
	req->pers = NULL;

	gsh_free(req, MEM_COMP_FSAL);
}

/**
 * @brief Core engine callback for an FSAL read/write
 *
 * Runs on a reaper thread, which has no op_ctx of its own.  The export
 * reference taken here is safe because the protocol request that started
 * this I/O cannot retire before done_cb() runs, so its own op_ctx — and
 * the export reference it holds — is still alive.
 */
static void fsal_uring_io_done(void *cb_arg, int res, int fsync_res)
{
	struct fsal_uring_io_req *req = cb_arg;
	struct fsal_io_arg *io_arg = req->io_arg;
	struct req_op_context ctx;

	LogUring(NIV_DEBUG,
		 "req %p: %s completed on reaper, res=%d fsync_res=%d"
		 " offset=%" PRIu64 " requested=%zu",
		 req, req->is_write ? "write" : "read", res, fsync_res,
		 io_arg->offset, io_arg->io_request);

	get_gsh_export_ref(req->exp);
	init_op_context_simple(&ctx, req->exp, req->fsal_export);
	op_ctx->creds = req->creds;

	if (res < 0) {
		req->status = posix2fsal_status(-res);
		LogUring(NIV_WARN, "req %p: %s failed: %s (%s)", req,
			 req->is_write ? "write" : "read", strerror(-res),
			 fsal_err_txt(req->status));
	} else {
		io_arg->io_amount = res;

		if (req->is_write) {
			if (io_arg->fsal_stable && fsync_res < 0) {
				/* Same contract as the synchronous path: a
				 * failed flush downgrades the write to
				 * unstable rather than failing it.
				 */
				io_arg->fsal_stable = false;
				LogUring(NIV_WARN,
					 "req %p: linked fsync failed: %s"
					 " - downgrading the write to unstable",
					 req, strerror(-fsync_res));
			}
		} else {
			io_arg->end_of_file = (res == 0);

			if (io_arg->end_of_file) {
				LogUring(NIV_FULL_DEBUG,
					 "req %p: read hit end of file", req);
			} else if ((size_t)res < io_arg->io_request) {
				LogUring(NIV_FULL_DEBUG,
					 "req %p: short read, %d of %zu bytes",
					 req, res, io_arg->io_request);
			}
		}
	}

	if (req->out_fd->close_on_complete) {
		/* Completing here would mean closing the fd on the reaper
		 * thread. Hand the request back to a worker thread instead;
		 * it will call fsal_uring_io_resume().
		 */
		LogUring(NIV_DEBUG,
			 "req %p: fd %p must be closed, asking for a"
			 " FSAL_CLOSEFD resume on a worker thread",
			 req, req->out_fd);

		io_arg->fsal_resume = FSAL_CLOSEFD;
		io_arg->cbi = req;
		req->done_cb(req->obj_hdl, req->status, io_arg,
			     req->caller_arg);
		release_op_context();
		return;
	}

	fsal_uring_io_finish(req);
}

void fsal_uring_io_resume(struct fsal_io_arg *io_arg, fsal_async_cb done_cb,
			  void *caller_arg)
{
	struct fsal_uring_io_req *req = io_arg->cbi;
	struct req_op_context ctx;

	assert(io_arg->fsal_resume == FSAL_CLOSEFD);

	LogUring(NIV_DEBUG, "req %p: resumed on a worker thread to close fd %p",
		 req, req->out_fd);

	io_arg->fsal_resume = FSAL_NORESUME;
	io_arg->cbi = NULL;

	/* The resuming call supplies a fresh callback pair. The original one
	 * may have been an intermediate wrapper that has already run and freed
	 * its caller_arg, so the recorded pair must not be replayed.
	 */
	req->done_cb = done_cb;
	req->caller_arg = caller_arg;

	get_gsh_export_ref(req->exp);
	init_op_context_simple(&ctx, req->exp, req->fsal_export);
	op_ctx->creds = req->creds;

	/* req->status was recorded by fsal_uring_io_done() before it asked
	 * for the resume, so the error is not lost across the hand-off.
	 */
	fsal_uring_io_finish(req);
}

int fsal_uring_io_start(struct fsal_uring_engine *engine,
			struct fsal_uring_io_req *req,
			const struct fsal_uring_io *io)
{
	struct uring_op core_op = { 0 };
	struct fsal_uring_rw_ctx ctx = { 0 };
	struct fsal_io_arg *io_arg = io->io_arg;
	int rc;

	req->obj_hdl = io->obj_hdl;
	req->out_fd = io->out_fd;
	req->io_arg = io_arg;
	req->caller_arg = io->caller_arg;
	req->done_cb = io->done_cb;
	req->exp = op_ctx->ctx_export;
	req->fsal_export = op_ctx->fsal_export;
	req->share = io->share;
	req->share_access = io->share_access;
	req->buf = io->buf;
	req->is_write = io->is_write;
	req->status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	req->creds = op_ctx->creds;

	/*
	 * Resolve the io_uring personality for the caller's credentials.
	 * fsal_uring_pers_get_ops() is the one place that knows how: it
	 * serves the request-scoped op_ctx cache (an NFSv4 COMPOUND's second
	 * READ costs a refcount, not a lookup), and on a miss interns the
	 * identity — a registry rdlock plus an AVL hit, ~100 ns, or a real
	 * io_uring_register_personality() on first use.
	 *
	 * @a io->cred_ops is how the identity is assumed for that one
	 * registration, and it has to come from the FSAL: FSAL_VFS must use
	 * vfs_set_credentials(), which honours the export's only_one_user
	 * mode and can legitimately refuse, while FSAL_GPFS uses the generic
	 * pair. See struct uring_pers_cred_ops.
	 *
	 * A NULL result is refused, not worked around. Submitting anyway
	 * would leave sqe->personality at 0, which means the SQE executes
	 * under whichever thread reaches io_uring_submit() -- the Ganesha
	 * daemon, i.e. root. Impersonating around the submit does not fix
	 * that honestly either: with SQPOLL the kernel poller thread submits,
	 * and with coalescing enabled the SQE is flushed later, possibly by
	 * an unrelated worker, so the identity the kernel snapshots is not
	 * the one we assumed. Failing here instead sends the caller to its
	 * synchronous handler, which impersonates on the thread that makes
	 * the syscall and is therefore correct by construction.
	 *
	 * This is the same policy fsal_uring_pers_caller() applies to the
	 * compound planner; the two paths must not disagree about who a
	 * client's I/O runs as.
	 */
	req->pers = fsal_uring_pers_get_ops(engine, io->cred_ops);

	if (req->pers == NULL) {
		LogUring(NIV_DEBUG,
			 "req %p: no personality for uid=%u gid=%u, refusing to"
			 " submit; caller falls back to synchronous I/O",
			 req, op_ctx->creds.caller_uid,
			 op_ctx->creds.caller_gid);
		return -EPERM;
	}

	/* Build the per-op FSAL context that sqe_pre will read. */
	ctx.fd = io->fd;
	ctx.offset = io_arg->offset;
	ctx.iov = io_arg->iov;
	ctx.iov_count = io_arg->iov_count;
	ctx.is_write = io->is_write;
	ctx.pers = req->pers; /* never NULL, see above */

	if (io->buf != NULL) {
		/* fsal_uring_read_buffer() only hands out a registered buffer
		 * for a single segment request.
		 */
		ctx.fixed_buf = io->buf;
		ctx.fixed_len = io_arg->iov[0].iov_len;
	}

	/* Wire the core op. */
	core_op.sqe_pre = fsal_uring_rw_prep;
	core_op.sqe_pre_arg = &ctx;
	core_op.cb = fsal_uring_io_done;
	core_op.cb_arg = req;

	if (io->is_write && io->stable) {
		/* Link an fsync behind the write for NFS stable writes. */
		ctx.datasync = false;
		core_op.sqe_pre2 = fsal_uring_fsync_prep;
		core_op.sqe_pre2_arg = &ctx;
	}

	io_arg->io_amount = 0;

	LogUring(NIV_DEBUG,
		 "req %p: starting %s fd=%d offset=%" PRIu64
		 " len=%zu iovcnt=%d fixed_buf=%d stable=%s state=%s",
		 req, io->is_write ? "write" : "read", io->fd, io_arg->offset,
		 io_arg->io_request, io_arg->iov_count,
		 io->buf != NULL ? (int)io->buf->index : -1,
		 core_op.sqe_pre2 != NULL ? "yes" : "no",
		 io_arg->state != NULL ? "yes" : "no");

	rc = uring_engine_submit(engine->core, &core_op);

	if (rc != 0) {
		LogUring(NIV_WARN,
			 "req %p: submit failed (%s), caller falls back to"
			 " synchronous I/O",
			 req, strerror(-rc));

		/* Nothing is in flight, so the personality reference taken
		 * above has no completion to release it. The op_ctx cache
		 * keeps its own ref and is unaffected.
		 */
		uring_pers_put(req->pers);
		req->pers = NULL;
	}

	return rc;
}

/* =========================================================================
 * Generic metadata async framework — Sub-Task 7
 * =========================================================================
 *
 * fsal_uring_meta_done()  — CQE callback for all metadata ops (reaper thread)
 * fsal_uring_meta_start() — single submit entry-point for all metadata ops
 *
 * Thread model mirrors the I/O path exactly:
 *   submit thread  -> calls fsal_uring_meta_start()
 *   reaper thread  -> calls fsal_uring_meta_done() -> rebuilds op_ctx
 *                     -> calls done_cb() -> release_op_context()
 *                     -> uring_pers_put() -> gsh_free(outer_req)
 *
 * The outer struct embeds fsal_uring_meta_req as its first member so that
 * the single gsh_free() at the end of meta_done() frees the entire thing.
 */

/**
 * @brief CQE callback for all metadata ops.
 *
 * Runs on a reaper thread.  Rebuilds op_ctx from the saved export reference,
 * maps the kernel result to fsal_status_t, calls done_cb(), then tears down.
 * The outer struct (which embeds the base as its first member) is freed here.
 *
 * @param cb_arg  Pointer to struct fsal_uring_meta_req (== outer struct base).
 * @param res     Primary CQE result (negative errno on failure,
 *                >= 0 on success).
 * @param res2    Secondary result (unused for metadata ops; always 0).
 */
static void fsal_uring_meta_done(void *cb_arg, int res, int res2)
{
	struct fsal_uring_meta_req *req = cb_arg;
	struct req_op_context ctx;

	(void)res2; /* Metadata ops have no linked second SQE. */

	LogUring(NIV_DEBUG, "meta req %p: completed on reaper, res=%d", req,
		 res);

	get_gsh_export_ref(req->exp);
	init_op_context_simple(&ctx, req->exp, req->fsal_export);

	if (res < 0) {
		req->status = posix2fsal_status(-res);
		LogUring(NIV_WARN, "meta req %p: failed: %s (%s)", req,
			 strerror(-res), fsal_err_txt(req->status));
	} else {
		req->status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	LogUring(NIV_DEBUG, "meta req %p: status=%s, calling done_cb %p", req,
		 fsal_err_txt(req->status), (void *)(uintptr_t)req->done_cb);

	req->done_cb(req->obj_hdl, req->status, NULL, req->caller_arg);

	release_op_context();

	/* Release the personality reference taken at meta_start().
	 * Safe from the reaper thread — refcnt is still > 0 after decrement
	 * while this req is in-flight.  NULL-safe. */
	uring_pers_put(req->pers);
	req->pers = NULL;

	/* Free the entire outer struct.  The base is always at offset 0
	 * so casting to void* is pointer-equivalent to the outer struct
	 * pointer. */
	gsh_free(req, MEM_COMP_FSAL);
}

int fsal_uring_meta_start(struct fsal_uring_engine *engine,
			  struct fsal_uring_meta_req *req,
			  void (*sqe_pre)(struct io_uring_sqe *sqe, void *arg,
					  unsigned int ring_idx),
			  void *sqe_pre_arg, struct fsal_obj_handle *obj_hdl,
			  fsal_async_cb done_cb, void *caller_arg)
{
	struct uring_op core_op = { 0 };
	int rc;

	req->obj_hdl = obj_hdl;
	req->done_cb = done_cb;
	req->caller_arg = caller_arg;
	req->exp = op_ctx->ctx_export;
	req->fsal_export = op_ctx->fsal_export;
	req->status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	req->pers = NULL;

	/*
	 * Personality lookup — same logic as the I/O path:
	 *   op_ctx->uring_pers != NULL  → reuse the cached ref (+ extra ref)
	 *   op_ctx->uring_pers == NULL  → lookup / register +
	 *                                  cache the result
	 *   NULL return from lookup     → sqe->personality stays 0 (fallback)
	 */
	if (engine->pers != NULL) {
		if (op_ctx->uring_pers != NULL) {
			uring_pers_ref(op_ctx->uring_pers);
			req->pers = op_ctx->uring_pers;
		} else {
			req->pers = uring_pers_for_req(engine->pers, NULL);
			if (req->pers != NULL) {
				uring_pers_ref(req->pers);
				op_ctx->uring_pers = req->pers;
			} else {
				LogUring(NIV_WARN,
					 "meta req %p: personality unavailable"
					 " (uid=%u gid=%u) — falling back"
					 " to impersonation path",
					 req, op_ctx->creds.caller_uid,
					 op_ctx->creds.caller_gid);
			}
		}
	}

	/* Wire the core op — no sqe_pre2 for metadata (no linked SQE). */
	core_op.sqe_pre = sqe_pre;
	core_op.sqe_pre_arg = sqe_pre_arg;
	core_op.cb = fsal_uring_meta_done;
	core_op.cb_arg = req;

	LogUring(NIV_DEBUG, "meta req %p: submitting sqe_pre=%p obj_hdl=%p",
		 req, (void *)(uintptr_t)sqe_pre, (void *)obj_hdl);

	rc = uring_engine_submit(engine->core, &core_op);

	if (rc != 0) {
		LogUring(NIV_WARN,
			 "meta req %p: submit failed (%s), caller falls back"
			 " to synchronous syscall",
			 req, strerror(-rc));
		/*
		 * Submit failed — undo the personality ref we took, since
		 * fsal_uring_meta_done() will never be called.
		 */
		uring_pers_put(req->pers);
		req->pers = NULL;
	}

	return rc;
}

/* =========================================================================
 * Per-op sqe_pre functions — fill the SQE and stamp personality
 *
 * Each function casts arg to its op-specific context struct, calls the
 * matching io_uring_prep_*() helper, and calls uring_pers_attach().
 * They are static — callers pass them by pointer to fsal_uring_meta_start().
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * Op 1 — SYNC_FILE_RANGE  (vfs_commit2)
 * Minimum kernel: Linux 5.2
 * -------------------------------------------------------------------------
 */
void fsal_uring_sync_range_prep(struct io_uring_sqe *sqe, void *arg,
				unsigned int ring_idx)
{
	struct fsal_uring_sync_ctx *ctx = arg;

	io_uring_prep_sync_file_range(sqe, ctx->fd, ctx->nbytes, ctx->offset,
				      ctx->flags);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}

/* -------------------------------------------------------------------------
 * Op 2 — FALLOCATE  (vfs_fallocate)
 * Minimum kernel: Linux 6.0
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_FALLOCATE
void fsal_uring_falloc_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx)
{
	struct fsal_uring_falloc_ctx *ctx = arg;

	io_uring_prep_fallocate(sqe, ctx->fd, ctx->mode, (off_t)ctx->offset,
				(off_t)ctx->len);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_FALLOCATE */

/* -------------------------------------------------------------------------
 * Op 3 — OPENAT  (vfs_open2 create-by-name path)
 * Minimum kernel: Linux 5.6
 * -------------------------------------------------------------------------
 */
void fsal_uring_openat_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx)
{
	struct fsal_uring_openat_ctx *ctx = arg;

	io_uring_prep_openat(sqe, ctx->dir_fd, ctx->name, ctx->flags,
			     ctx->mode);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}

/* -------------------------------------------------------------------------
 * Op 4 — CLOSE  (vfs_close_my_fd fire-and-forget)
 * Minimum kernel: Linux 5.6
 * No personality — close(2) has no credential check in the kernel.
 * -------------------------------------------------------------------------
 */
void fsal_uring_close_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx)
{
	struct fsal_uring_close_ctx *ctx = arg;

	(void)ring_idx; /* no personality stamp for close */
	io_uring_prep_close(sqe, ctx->fd);
}

/* -------------------------------------------------------------------------
 * Op 5 — STATX  (fetch_attrs / getattr hot path)
 * Minimum kernel: Linux 5.13
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_STATX
void fsal_uring_statx_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx)
{
	struct fsal_uring_statx_ctx *ctx = arg;

	io_uring_prep_statx(sqe, ctx->dirfd, ctx->pathname, ctx->flags,
			    ctx->mask, ctx->buf);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_STATX */

/* -------------------------------------------------------------------------
 * Op 6 — RENAMEAT2  (renamefile)
 * Minimum kernel: Linux 5.11
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_RENAMEAT
void fsal_uring_rename_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx)
{
	struct fsal_uring_rename_ctx *ctx = arg;

	io_uring_prep_renameat(sqe, ctx->oldfd, ctx->old_name, ctx->newfd,
			       ctx->new_name, ctx->flags);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_RENAMEAT */

/* -------------------------------------------------------------------------
 * Op 7 — UNLINKAT  (file_unlink and error-path cleanup)
 * Minimum kernel: Linux 5.11
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_UNLINKAT
void fsal_uring_unlink_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx)
{
	struct fsal_uring_unlink_ctx *ctx = arg;

	io_uring_prep_unlinkat(sqe, ctx->dfd, ctx->name, ctx->flags);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_UNLINKAT */

/* -------------------------------------------------------------------------
 * Op 8 — MKDIRAT  (makedir)
 * Minimum kernel: Linux 5.15
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_MKDIRAT
void fsal_uring_mkdir_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx)
{
	struct fsal_uring_mkdir_ctx *ctx = arg;

	io_uring_prep_mkdirat(sqe, ctx->dfd, ctx->name, ctx->mode);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_MKDIRAT */

/* -------------------------------------------------------------------------
 * Op 9 — SYMLINKAT  (makesymlink)
 * Minimum kernel: Linux 5.11
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_SYMLINKAT
void fsal_uring_symlink_prep(struct io_uring_sqe *sqe, void *arg,
			     unsigned int ring_idx)
{
	struct fsal_uring_symlink_ctx *ctx = arg;

	io_uring_prep_symlinkat(sqe, ctx->target, ctx->dfd, ctx->linkpath);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_SYMLINKAT */

/* -------------------------------------------------------------------------
 * Op 10 — LINKAT  (linkfile via AT_EMPTY_PATH)
 * Minimum kernel: Linux 5.11
 * -------------------------------------------------------------------------
 */
#ifdef HAVE_IO_URING_PREP_LINKAT
void fsal_uring_linkat_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx)
{
	struct fsal_uring_linkat_ctx *ctx = arg;

	io_uring_prep_linkat(sqe, ctx->oldfd, ctx->oldname, ctx->newfd,
			     ctx->newname, ctx->flags);
	uring_pers_attach(sqe, ctx->pers, ring_idx);
}
#endif /* HAVE_IO_URING_PREP_LINKAT */

/* -------------------------------------------------------------------------
 * Pluggable per-operation vector (see FSAL/fsal_uring_fops.h)
 * -------------------------------------------------------------------------
 * FSAL modules are loaded against libganesha_nfsd and cannot link the core
 * engine directly, so these thin wrappers are the exported seam. They are
 * listed in MainNFSD/libganesha_nfsd.ver.
 */

int fsal_uring_submit_chain(struct fsal_uring_engine *engine,
			    struct uring_chain_req *chain)
{
	if (engine == NULL)
		return -ESHUTDOWN;

	return uring_engine_submit_chain(engine->core, chain);
}

bool fsal_uring_opcode_supported(struct fsal_uring_engine *engine,
				 unsigned int opcode)
{
	if (engine == NULL)
		return false;

	return uring_engine_op_supported(engine->core, opcode);
}

const struct fsal_uring_fop_desc *fsal_uring_fop_get(struct fsal_export *exp,
						     enum fsal_uring_fop fop)
{
	const struct fsal_uring_fops *fops;
	const struct fsal_uring_fop_desc *desc;

	if (exp == NULL || fop >= FSAL_URING_FOP_LAST)
		return NULL;

	fops = exp->uring_fops;

	if (fops == NULL || fops->engine == NULL)
		return NULL;

	desc = fops->desc[fop];

	/* Both halves must be present: an operation that can be submitted
	 * but not completed would strand the compound.
	 */
	if (desc == NULL || desc->prep == NULL || desc->finish == NULL)
		return NULL;

	/*
	 * The kernel probe only speaks for operations expressed as a generic
	 * opcode. An FSAL driving its own IORING_OP_URING_CMD command set
	 * records FSAL_URING_OPCODE_NONE and answers via op_supported().
	 */
	if (desc->kernel_opcode != FSAL_URING_OPCODE_NONE &&
	    !uring_engine_op_supported(fops->engine->core, desc->kernel_opcode))
		return NULL;

	if (fops->op_supported != NULL && !fops->op_supported(exp, fop))
		return NULL;

	return desc;
}

bool fsal_uring_fop_can_batch(struct fsal_export *exp, enum fsal_uring_fop fop)
{
	return fsal_uring_fop_get(exp, fop) != NULL;
}

struct uring_pers *fsal_uring_pers_get(struct fsal_uring_engine *engine)
{
	return fsal_uring_pers_get_ops(engine, NULL);
}

struct uring_pers *
fsal_uring_pers_get_ops(struct fsal_uring_engine *engine,
			const struct uring_pers_cred_ops *cred_ops)
{
	struct uring_pers *p;

	if (engine == NULL || engine->pers == NULL)
		return NULL;

	/*
	 * The personality registry is private to this file, so the protocol
	 * layer cannot resolve a credential itself. op_ctx caches the one
	 * already resolved for this request; clean_credentials() and
	 * nfs_req_creds() in support/nfs_creds.c drop that reference when
	 * the credentials change or the request ends.
	 */
	if (op_ctx != NULL && op_ctx->uring_pers != NULL) {
		uring_pers_ref(op_ctx->uring_pers);
		return op_ctx->uring_pers;
	}

	/* Returns with a reference held for us. */
	p = uring_pers_for_req(engine->pers, cred_ops);

	if (p == NULL) {
		/* Registry full or registration failed. The caller must fall
		 * back rather than submit under the daemon credential.
		 */
		return NULL;
	}

	if (op_ctx != NULL) {
		/* A second reference, owned by op_ctx. */
		uring_pers_ref(p);
		op_ctx->uring_pers = p;
	}

	return p;
}

void fsal_uring_pers_release(struct uring_pers *pers)
{
	uring_pers_put(pers);
}

/* -------------------------------------------------------------------------
 * Per-SQE credential resolution
 * -------------------------------------------------------------------------
 */

/** Bottom-most storage export under a possibly stacked one. */
static struct fsal_export *fsal_uring_storage_exp(struct fsal_export *exp)
{
	while (exp != NULL && exp->sub_export != NULL)
		exp = exp->sub_export;

	return exp;
}

fsal_status_t fsal_uring_pers_caller(struct fsal_uring_fop_req *req,
				     unsigned int sqe_idx,
				     struct uring_pers **pers_out)
{
	struct fsal_export *exp;
	const struct fsal_uring_fops *fops;

	*pers_out = NULL;

	exp = fsal_uring_storage_exp(op_ctx != NULL ? op_ctx->fsal_export
						    : NULL);

	if (exp == NULL || exp->uring_fops == NULL)
		return fsalstat(ERR_FSAL_INVAL, 0);

	fops = exp->uring_fops;

	*pers_out = fsal_uring_pers_get_ops(fops->engine, fops->cred_ops);

	if (*pers_out == NULL) {
		/*
		 * No personality means the SQE would execute as the Ganesha
		 * daemon, i.e. with more privilege than the client. Refuse,
		 * so the planner falls back to the synchronous handler,
		 * which impersonates properly.
		 */
		LogUring(NIV_DEBUG,
			 "fop %d sqe %u: no personality for caller, refusing to batch",
			 (int)req->fop, sqe_idx);
		return fsalstat(ERR_FSAL_PERM, 0);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t fsal_uring_pers_none(struct fsal_uring_fop_req *req,
				   unsigned int sqe_idx,
				   struct uring_pers **pers_out)
{
	*pers_out = NULL;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t fsal_uring_fop_resolve_creds(struct fsal_export *exp,
					   struct fsal_uring_fop_req *req)
{
	const struct fsal_uring_fop_desc *desc;
	fsal_uring_pers_fn resolve;
	fsal_status_t status;
	unsigned int i;

	if (req->nr_sqe == 0)
		req->nr_sqe = 1;

	if (req->nr_sqe > FSAL_URING_FOP_MAX_SQE) {
		LogUring(NIV_CRIT, "fop %d declared %u SQEs, max is %d",
			 (int)req->fop, req->nr_sqe, FSAL_URING_FOP_MAX_SQE);
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	desc = fsal_uring_fop_get(exp, req->fop);

	if (desc == NULL)
		return fsalstat(ERR_FSAL_INVAL, 0);

	/* An FSAL that states no policy gets the client's credential, which
	 * is the safe default: it is never more privilege than the caller
	 * already has.
	 */
	resolve = desc->pers_resolve != NULL ? desc->pers_resolve
					     : fsal_uring_pers_caller;

	for (i = 0; i < req->nr_sqe; i++) {
		req->pers[i] = NULL;

		status = resolve(req, i, &req->pers[i]);

		if (FSAL_IS_ERROR(status)) {
			/* Undo the ones already taken. */
			fsal_uring_fop_release_creds(req);
			return status;
		}
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

void fsal_uring_fop_release_creds(struct fsal_uring_fop_req *req)
{
	unsigned int i;

	for (i = 0; i < FSAL_URING_FOP_MAX_SQE; i++) {
		if (req->pers[i] != NULL) {
			uring_pers_put(req->pers[i]);
			req->pers[i] = NULL;
		}
	}
}

#endif /* USE_FSAL_URING */
