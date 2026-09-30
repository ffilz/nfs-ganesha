/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
 * @file    FSAL/fsal_uring.h
 * @brief   Shared io_uring async I/O engine for FSALs
 *
 * This is a FSAL-agnostic layer. It is split in two, and an FSAL may adopt
 * either half on its own:
 *
 * LAYER 1 - the engine (fsal_uring_engine)
 *
 *   A thin FSAL wrapper around the opcode-agnostic uring_core engine
 *   (see io_uring/core/uring_engine.h).  It adds credential handling via
 *   the FSAL personality registry and integrates Ganesha's logging and
 *   memory components.  You hand it an fd, an offset and an iovec, and it
 *   calls you back on a reaper thread.
 *
 *   Supported operations: read, readv, write, writev and fsync - including
 *   a write (or writev) with an fsync linked behind it, which is what an
 *   NFS stable write needs and what a blocking fsync() on the completion
 *   thread must never be used for.
 *
 * LAYER 2 - the FSAL I/O adapter (fsal_uring_io_req)
 *
 *   Implements the read2()/write2() choreography that every FSAL with
 *   non-blocking I/O has to repeat: rebuild an op_ctx on the reaper thread,
 *   map the result to a fsal_status_t, fsal_complete_io(), release the
 *   temporary share reservation, honour the FSAL_CLOSEFD resume protocol,
 *   and finally call done_cb(). An FSAL only has to embed a
 *   struct fsal_uring_io_req at the top of its own per-request state.
 *
 * ZERO-COPY READS
 *
 *   An FSAL that advertises fso_allocate_own_read_buffer receives read
 *   requests with iov[0].iov_base == NULL and is expected to supply the
 *   buffer plus an iov_release()/release_data pair (see fsal_read2()).
 *   That is exactly the hook a registered io_uring buffer needs, so no new
 *   field is added to struct fsal_io_arg: fsal_uring_read_buffer() fills
 *   the iovec in and installs the matching release. The buffer is handed
 *   over to the reply and released when the reply is freed.
 *
 * THREADING
 *
 *   Submission is thread-safe and may be done from any Ganesha worker.
 *   Completion callbacks run on a reaper thread that has no op_ctx of its
 *   own, so they must not block. The layer-2 adapter builds a temporary
 *   op_ctx for you.
 */

#ifndef FSAL_URING_H
#define FSAL_URING_H

#include "config.h"

#ifdef USE_FSAL_URING

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#include "fsal_types.h"
#include "fsal_api.h"
#include "log.h"
#include "io_uring/core/uring_engine.h"

/**
 * @brief Logging for the whole io_uring path
 *
 * Every message the engine and its FSAL adapters emit goes through this, so
 * the io_uring path can be turned up on its own without drowning in the rest
 * of COMPONENT_FSAL:
 *
 * @code
 * LOG {
 *	COMPONENTS {
 *		URING = FULL_DEBUG;
 *	}
 * }
 * @endcode
 *
 * It is also settable at runtime over DBus, e.g.
 *
 * @code
 * dbus-send --print-reply --system --dest=org.ganesha.nfsd \
 *	/org/ganesha/nfsd/admin org.ganesha.nfsd.admin.set_log_level \
 *	string:COMPONENT_URING string:FULL_DEBUG
 * @endcode
 *
 * Rough guide to the levels used on this path:
 *   NIV_CRIT       engine is being fenced off, completions may be lost
 *   NIV_WARN       degraded but still correct (no SQE, no buffer, fallback)
 *   NIV_EVENT      engine up/down
 *   NIV_INFO       one-time setup detail and shutdown statistics
 *   NIV_DEBUG      per-request start/finish
 *   NIV_FULL_DEBUG per-SQE and per-CQE detail, buffer pool churn
 */
#define LogUring(level, fmt, ...) \
	LogAtLevel(COMPONENT_URING, level, "URING: " fmt, ##__VA_ARGS__)

/**
 * @brief Opaque FSAL-level engine wrapper.
 *
 * The definition lives in FSAL/fsal_uring.c.  Consumers use only the
 * fsal_uring_engine_* functions and the Layer-2 adapter below.
 */
struct fsal_uring_engine;

/* Forward declarations: the full definitions are in FSAL_URING/uring_pers.h.
 * struct uring_pers is used by the pers field of struct fsal_uring_io_req and
 * struct uring_pers_cred_ops by the cred_ops field of struct fsal_uring_io,
 * both below. */
struct uring_pers;
struct uring_pers_cred_ops;

/*
 * struct fsal_uring_buf is now an alias for struct uring_buf from the core
 * engine.  Existing FSALs that reference struct fsal_uring_buf continue to
 * compile unchanged.
 */
#define fsal_uring_buf uring_buf

/**
 * @brief Engine tunables
 *
 * The base ring/pool/coalescing configuration lives in
 * struct uring_engine_params (from io_uring/core/uring_engine.h).
 * fsal_uring_params embeds that struct as its first member so that a pointer
 * to fsal_uring_params can be cast to uring_engine_params * safely, and so
 * that the FSAL-only knob (pers_max) travels alongside the core tunables.
 *
 * Any field of the embedded @a base left 0 takes the built-in default,
 * except base.buf_min where 0 means "no buffer pool".
 */
struct fsal_uring_params {
	/**
	 * Base engine configuration — ring count, queue depth, buffer pool,
	 * SQPOLL, coalescing.  This MUST be the first member so that a pointer
	 * to fsal_uring_params can be cast to uring_engine_params * safely.
	 */
	struct uring_engine_params base;

	/**
	 * Maximum number of cached io_uring personalities (default 8192).
	 * 0 means use the default.  Each personality costs one
	 * io_uring_register_personality() call per ring on first use; the
	 * cap prevents unbounded growth when many distinct uid/gid triples
	 * are active simultaneously.
	 */
	uint32_t pers_max;
};

/*
 * ---------------------------------------------------------------------
 * Layer 1: engine
 * ---------------------------------------------------------------------
 */

/**
 * @brief Create an io_uring engine
 *
 * Starts the reaper thread(s). Safe to call with liburing present but
 * io_uring disabled by the kernel: it fails cleanly and the caller should
 * simply stay on the synchronous path.
 *
 * @param[in]  params  Tunables, may be NULL for all defaults
 * @param[out] engine  Receives the new engine
 *
 * @return 0 on success, -errno on failure (*engine untouched)
 */
int fsal_uring_engine_create(const struct fsal_uring_params *params,
			     struct fsal_uring_engine **engine);

/**
 * @brief Stop the reapers, unregister buffers and free the engine
 *
 * Must not be called while I/O is still in flight.
 */
void fsal_uring_engine_destroy(struct fsal_uring_engine *engine);

/**
 * @brief Largest transfer that can use a registered buffer
 *
 * @return Size of the largest class, or 0 if the engine has no pool
 */
size_t fsal_uring_buf_max_size(struct fsal_uring_engine *engine);

/**
 * @brief Take a registered buffer able to hold @a len bytes
 *
 * Served from the smallest size class that fits, so the returned buffer's
 * len is >= @a len but generally not equal to it.
 *
 * A miss is not an error, it just costs the zero-copy path: the caller
 * should use an ordinary heap buffer and the non-fixed opcode.
 *
 * @return The buffer, or NULL if there is no pool, @a len exceeds the
 *         largest class, or that class is empty
 */
struct uring_buf *fsal_uring_buf_get(struct fsal_uring_engine *engine,
				     size_t len);

/**
 * @brief Return a registered buffer to the pool
 */
void fsal_uring_buf_put(struct uring_buf *buf);

/**
 * @brief fsal_io_arg::iov_release for a registered buffer
 *
 * @a release_data must be the struct uring_buf *.
 */
void fsal_uring_buf_release(void *release_data);

/**
 * @brief Log the engine's counters at NIV_INFO
 */
void fsal_uring_log_stats(struct fsal_uring_engine *engine);

/**
 * @brief Harvest and run whatever has already completed on one ring.
 *
 * Non-blocking and safe against the reaper; see uring_engine_poll(). The
 * point is that a thread which has just submitted can collect its own
 * completion instead of suspending and being handed it back by a reaper.
 * When the kernel satisfies the I/O inline during io_uring_enter() -- a
 * page cache read, for instance -- that removes two context switches per
 * request.
 *
 * @param ring_idx  Ring the work went to, i.e. uring_chain_req::ring_idx.
 * @return Number of completions processed.
 */
unsigned int fsal_uring_poll(struct fsal_uring_engine *engine,
			     unsigned int ring_idx);

/**
 * @brief Return the number of rings in the engine.
 */
unsigned int fsal_uring_nr_rings(struct fsal_uring_engine *engine);

/*
 * ---------------------------------------------------------------------
 * Layer 2: generic FSAL read2()/write2() adapter
 * ---------------------------------------------------------------------
 */

/**
 * @brief Per-request state for the generic FSAL I/O adapter
 *
 * An FSAL that needs extra per-request storage (typically a temporary
 * struct fsal_fd, which can no longer live on the caller's stack once the
 * I/O is asynchronous) embeds this as the FIRST member of its own struct.
 * The adapter frees the whole thing with
 * gsh_free(req, MEM_COMP_FSAL) when the request is done, so the FSAL must
 * allocate it with that component.
 *
 * All fields are filled in by fsal_uring_io_start(); the FSAL never writes
 * them directly.
 */
struct fsal_uring_io_req {
	struct fsal_obj_handle *obj_hdl;
	struct fsal_fd *out_fd;
	struct fsal_io_arg *io_arg;
	void *caller_arg;
	fsal_async_cb done_cb;
	/** Export reference taken at submit, released at completion. */
	struct gsh_export *exp;
	struct fsal_export *fsal_export;
	/** Share to release on completion, NULL for state-based I/O. */
	struct fsal_share *share;
	fsal_openflags_t share_access;
	/** Registered buffer backing iov[0], NULL if none. */
	struct uring_buf *buf;
	bool is_write;
	/** Status carried across a FSAL_CLOSEFD resume. */
	fsal_status_t status;
	/**
	 * Caller's credentials, replayed into the op_ctx the completion
	 * builds on the reaper thread.
	 *
	 * init_op_context_simple() leaves creds zeroed, which would make a
	 * done_cb that still does permission work — nfs4_read_cb() under
	 * Read_Access_Check_Policy = post, for one — judge the client as
	 * uid 0. This is a shallow copy, so caller_garray is borrowed rather
	 * than owned; that is safe for exactly the same reason the export
	 * reference is, namely that the request which started the I/O cannot
	 * retire before done_cb() has run.
	 */
	struct user_cred creds;
	/**
	 * io_uring personality snapshot taken at fsal_uring_io_start().
	 * Carries the refcount across the async gap so the reaper thread
	 * can call uring_pers_put() without touching op_ctx.
	 * NULL when personalities are not in use (fallback path).
	 */
	struct uring_pers *pers;
};

/**
 * @brief Everything the adapter needs to know about one I/O
 */
struct fsal_uring_io {
	struct fsal_obj_handle *obj_hdl;
	/** fd handed back by fsal_start_io(). */
	struct fsal_fd *out_fd;
	/** The POSIX descriptor inside @a out_fd. */
	int fd;
	bool is_write;
	/** Write only: link an fsync behind the data. */
	bool stable;
	/** Non-NULL when the I/O had no state and took a temporary share. */
	struct fsal_share *share;
	fsal_openflags_t share_access;
	/** Registered buffer from fsal_uring_read_buffer(), or NULL. */
	struct uring_buf *buf;
	fsal_async_cb done_cb;
	struct fsal_io_arg *io_arg;
	void *caller_arg;
	/**
	 * How to assume the client's identity long enough to register a
	 * personality for it.
	 *
	 * Which call does that is the owning FSAL's decision, not this
	 * layer's: FSAL_VFS must use vfs_set_credentials(), which honours
	 * the export's only_one_user mode and can legitimately fail, while
	 * FSAL_GPFS uses the generic fsal_set_credentials() pair. See
	 * struct uring_pers_cred_ops. NULL selects the generic pair.
	 *
	 * This is only consulted on a registry miss. Once an identity is
	 * interned the credential travels in sqe->personality and no
	 * impersonation happens on the I/O path at all.
	 */
	const struct uring_pers_cred_ops *cred_ops;
};

/**
 * @brief Provide the read buffer for a zero-copy capable FSAL
 *
 * Call this from read2() when the FSAL advertises
 * fso_allocate_own_read_buffer. Behaviour:
 *
 *   - caller already supplied a buffer, or this is a multi-segment read:
 *     nothing is done and NULL is returned (submit a plain readv)
 *   - a registered buffer of sufficient size is available: it is installed
 *     in iov[0] together with fsal_uring_buf_release() and returned
 *   - otherwise a plain heap buffer is installed in iov[0] with its own
 *     release, and NULL is returned (submit a plain readv)
 *
 * In every case iov[0] is usable for I/O afterwards, so the caller can
 * also fall back to preadv() with no further work.
 *
 * @return The registered buffer to pass in struct fsal_uring_io::buf, or
 *         NULL to use the iovec as-is
 */
struct uring_buf *fsal_uring_read_buffer(struct fsal_uring_engine *engine,
					 struct fsal_io_arg *read_arg);

/**
 * @brief Submit an FSAL read/write through the engine
 *
 * On success (0) the request is in flight, @a req is owned by the
 * completion path, and the caller must return immediately without
 * touching @a io_arg: done_cb() will be called from the reaper thread (or
 * from a worker thread after a FSAL_CLOSEFD resume).
 *
 * On failure nothing was queued, @a req still belongs to the caller and
 * @a io_arg is untouched, so the caller can fall back to preadv()/pwritev()
 * and complete the I/O synchronously.
 *
 * @return 0 on success, -errno on failure
 */
int fsal_uring_io_start(struct fsal_uring_engine *engine,
			struct fsal_uring_io_req *req,
			const struct fsal_uring_io *io);

/**
 * @brief Finish an I/O that asked to be resumed on a worker thread
 *
 * Call this at the top of read2()/write2() when io_arg->fsal_resume is
 * set, and return. It completes the I/O (including closing the fd, which
 * is why it could not be done on the reaper thread) and calls done_cb().
 *
 * done_cb and caller_arg are the ones handed to the resuming read2()/write2()
 * call, not the ones from the original call, and they replace what the
 * request recorded. This matters: an intermediate callback such as
 * read_plus_fallback_cb() frees its caller_arg before it forwards the
 * completion, so replaying the original pair would touch freed memory.
 */
void fsal_uring_io_resume(struct fsal_io_arg *io_arg, fsal_async_cb done_cb,
			  void *caller_arg);

#endif /* USE_FSAL_URING */
#endif /* FSAL_URING_H */
