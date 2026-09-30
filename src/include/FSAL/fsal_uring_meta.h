/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (C) 2026, IBM
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
 * @file  FSAL/fsal_uring_meta.h
 * @brief Layer 2 FSAL adapter — generic async metadata ops over io_uring.
 *
 * This header extends the read/write io_uring adapter (fsal_uring.h) to cover
 * every remaining VFS syscall that can be submitted asynchronously through the
 * same uring_engine.  The design is identical to the read/write path:
 *
 *   struct fsal_uring_meta_req   — generic lifecycle state (base struct)
 *   per-op context structs       — stack-allocated, live only for
 *                                  duration of submit
 *   per-op sqe_pre functions     — fill the SQE + stamp sqe->personality
 *   fsal_uring_meta_start()      — single generic submit entry-point
 *   fsal_uring_meta_done()       — generic CQE callback (in fsal_uring.c)
 *
 * HOW TO ADD A NEW OP (4 steps)
 * ----------------------------
 *   1. Define a context struct `struct fsal_uring_<op>_ctx` below with the
 *      inputs the SQE prep needs (fds, names, offsets, pers pointer).
 *
 *   2. Add a static `fsal_uring_<op>_prep()` function in fsal_uring.c that
 *      casts arg to the context struct, calls the matching
 *      `io_uring_prep_*()` liburing helper, and calls
 *      `uring_pers_attach(sqe, ctx->pers, ring_idx)`.
 *
 *   3. At the VFS call site, stack-allocate the context struct, heap-allocate
 *      a per-op req that embeds `struct fsal_uring_meta_req base` as its
 *      first member, then call `fsal_uring_meta_start()`.
 *
 *   4. Implement a `done_cb` that receives
 *      `(obj_hdl, status, NULL, caller_arg)`
 *      and drives whatever the VFS op needs after the kernel completes it.
 *
 * PERSONALITY STAMPING
 * ────────────────────
 *   Every sqe_pre function that changes credentials calls:
 *     uring_pers_attach(sqe, ctx->pers, ring_idx)
 *   Ops that have no credential check (CLOSE) leave sqe->personality at 0
 *   and pass (void)ring_idx to suppress the unused-parameter warning.
 *
 * KERNEL VERSION REQUIREMENTS
 * --------------------------
 *   Each op notes the minimum kernel version.  Degrading gracefully takes
 *   two separate checks, and neither of them is `#ifdef IORING_OP_*`: those
 *   names are enumerators of enum io_uring_op, not macros, so a
 *   preprocessor test for them is always false and silently removes the op.
 *
 *     build time    `#ifdef HAVE_IO_URING_PREP_<OP>`, set by
 *                   check_symbol_exists() on the liburing helper in
 *                   src/CMakeLists.txt — "can this liburing emit the SQE".
 *     run time      uring_engine_op_supported(engine, IORING_OP_<OP>) —
 *                   "does this kernel implement the opcode".
 *
 * XATTR OPS — DEFERRED
 * ────────────────────
 *   IORING_OP_GETXATTR / SETXATTR (Linux 5.18) require a path string, not
 *   an fd.  VFS FSAL uses fgetxattr(fd,...).  Wire when a proper fd-based
 *   opcode exists.  The /proc/self/fd/<n> workaround adds more overhead than
 *   the async benefit is worth.
 */

#ifndef FSAL_URING_META_H
#define FSAL_URING_META_H

#include "config.h"

#ifdef USE_FSAL_URING

#include <fcntl.h>
#include <linux/stat.h>
#include <stdint.h>
#include <sys/types.h>

#include <liburing.h>

#include "fsal_types.h"
#include "fsal_api.h"
#include "FSAL/fsal_uring.h"
#include "FSAL_URING/uring_pers.h" /* struct uring_pers */

/* =========================================================================
 * Generic base — struct fsal_uring_meta_req
 * =========================================================================
 *
 * Analogous to struct fsal_uring_io_req but without the read/write-specific
 * fields (fsal_fd, fsal_io_arg, share, buf, is_write).
 *
 * Rules:
 *   - Embed as the FIRST member of every per-op outer struct.
 *   - fsal_uring_meta_start() fills all fields; callers never write them.
 *   - fsal_uring_meta_done() in fsal_uring.c is the core CQE callback;
 *     it calls done_cb() then frees the whole outer struct via
 *     gsh_free(req, MEM_COMP_FSAL).  Callers must allocate the outer
 *     struct with gsh_calloc(1, sizeof(*outer), MEM_COMP_FSAL).
 */
struct fsal_uring_meta_req {
	/** Object this operation acts on. */
	struct fsal_obj_handle *obj_hdl;
	/** Callback invoked from the reaper thread on completion. */
	fsal_async_cb done_cb;
	/** Opaque forwarded verbatim to done_cb. */
	void *caller_arg;
	/** Export reference held across the async gap. */
	struct gsh_export *exp;
	struct fsal_export *fsal_export;
	/** Completion status, set by fsal_uring_meta_done(). */
	fsal_status_t status;
	/**
	 * io_uring personality snapshot.  Carries the refcount across the
	 * async gap so the reaper can call uring_pers_put() without touching
	 * op_ctx.  NULL when personalities are not in use (fallback path).
	 */
	struct uring_pers *pers;
};

/* =========================================================================
 * Generic submit entry-point
 * =========================================================================
 */

/**
 * fsal_uring_meta_start - submit one metadata op through the FSAL engine.
 *
 * The engine calls sqe_pre(sqe, sqe_pre_arg, ring_idx) synchronously inside
 * submit_lock before this function returns, so sqe_pre_arg only needs to be
 * live for the duration of this call (stack allocation is fine).
 *
 * On success (return 0): req is owned by the completion path.  done_cb() will
 * be called exactly once from the reaper thread.  The outer struct (which
 * embeds req as its first member) is freed by fsal_uring_meta_done().
 *
 * On failure (return -errno): nothing was queued, req is still owned by the
 * caller, and the caller should fall back to the synchronous syscall.
 *
 * @engine      FSAL engine wrapper (from fsal_uring_engine_create).
 * @req         Caller-allocated, base at offset 0.  Must be gsh_calloc'd with
 *              MEM_COMP_FSAL.
 * @sqe_pre     Op-specific SQE prep callback.
 * @sqe_pre_arg Pointer to the op-specific context struct (stack-allocated).
 * @obj_hdl     Object the op acts on (provides export ref + op_ctx rebuild).
 * @done_cb     Called on completion.  Receives
 *              (obj_hdl, status, NULL, caller_arg).
 * @caller_arg  Opaque forwarded to done_cb.
 */
int fsal_uring_meta_start(struct fsal_uring_engine *engine,
			  struct fsal_uring_meta_req *req,
			  void (*sqe_pre)(struct io_uring_sqe *sqe, void *arg,
					  unsigned int ring_idx),
			  void *sqe_pre_arg, struct fsal_obj_handle *obj_hdl,
			  fsal_async_cb done_cb, void *caller_arg);

/* =========================================================================
 * Per-op context structs and sqe_pre declarations
 *
 * Context structs are stack-allocated at the VFS call site.  They carry
 * only what the sqe_pre function needs to fill the SQE; they are not
 * accessible from the completion callback (they are gone by then).
 *
 * The sqe_pre functions are defined in fsal_uring.c and exported here so
 * callers can pass them by pointer to fsal_uring_meta_start().  The
 * declarations also document the exact signature expected when adding a
 * new op.
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * Op 1 — SYNC_FILE_RANGE  (vfs_commit2)
 *
 * Better fit than plain FSYNC: NFS COMMIT carries (offset, count).
 * SYNC_FILE_RANGE flushes only the requested byte range.
 *
 * Minimum kernel: IORING_OP_SYNC_FILE_RANGE — Linux 5.2
 * -------------------------------------------------------------------------
 */
struct fsal_uring_sync_ctx {
	int fd;
	uint64_t offset;
	uint32_t nbytes; /**< 0 = whole file */
	unsigned int flags; /**< SYNC_FILE_RANGE_WAIT_BEFORE|WRITE|
			       *   WAIT_AFTER */
	struct uring_pers *pers;
};

/** sqe_pre for vfs_commit2 — defined in fsal_uring.c */
void fsal_uring_sync_range_prep(struct io_uring_sqe *sqe, void *arg,
				unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 2 — FALLOCATE  (vfs_fallocate)
 *
 * Guards: #ifdef HAVE_IO_URING_PREP_FALLOCATE (build) plus
 *         uring_engine_op_supported(IORING_OP_FALLOCATE) (runtime)
 * Minimum kernel: IORING_OP_FALLOCATE — Linux 6.0
 * -------------------------------------------------------------------------
 */
struct fsal_uring_falloc_ctx {
	int fd;
	int mode; /**< 0=allocate,
			    *   FALLOC_FL_KEEP_SIZE|PUNCH_HOLE=punch */
	uint64_t offset;
	uint64_t len;
	struct uring_pers *pers;
};

/** sqe_pre for vfs_fallocate — defined in fsal_uring.c */
void fsal_uring_falloc_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 3 — OPENAT  (vfs_open2, create-by-name path only)
 *
 * Only the name != NULL branch of vfs_open2 is wired.  The by-handle
 * path (vfs_open2_by_handle) uses open_by_handle_at which has no io_uring op.
 *
 * The done callback receives res = fd.  Continuing vfs_open2 after the async
 * open (fstat + attr-setting) requires a FSAL_OPEN_RESUME mechanism, analogous
 * to FSAL_CLOSEFD but for the open completion.
 *
 * Minimum kernel: IORING_OP_OPENAT — Linux 5.6
 * -------------------------------------------------------------------------
 */
struct fsal_uring_openat_ctx {
	int dir_fd;
	const char *name; /**< Must be live until
			     *   fsal_uring_meta_start returns */
	int flags; /**< posix_flags passed to openat(2) */
	mode_t mode;
	struct uring_pers *pers;
};

/** sqe_pre for vfs_open2 create-by-name — defined in fsal_uring.c */
void fsal_uring_openat_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 4 — CLOSE  (vfs_close_my_fd and hot-path fd release)
 *
 * close(2) can block (page-cache flush, inode lock contention).  Fire-and-
 * forget async close frees the Ganesha worker immediately.  No NFS reply
 * depends on the close result.  No personality needed — the kernel close
 * path has no credential check.
 *
 * Pattern: pass a done_cb that logs NIV_WARN on error and does nothing else.
 * The worker is unblocked before the kernel close completes.
 *
 * Minimum kernel: IORING_OP_CLOSE — Linux 5.6
 * -------------------------------------------------------------------------
 */
struct fsal_uring_close_ctx {
	int fd;
	/*
	 * Deliberately no pers field — close has no credential check.
	 * fsal_uring_close_prep passes (void)ring_idx to suppress the warning.
	 */
};

/** sqe_pre for close — defined in fsal_uring.c */
void fsal_uring_close_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 5 — STATX  (fetch_attrs / fstat / fstatat sites)
 *
 * fstat(fd, &stat) maps to:
 *   statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, buf)
 *
 * The struct statx result buffer MUST be heap-allocated (it must outlive the
 * SQE submission).  The done_cb is responsible for
 * mapping statx → fsal_attrlist
 * and freeing buf.
 *
 * High priority: GETATTR is top-3 NFS operation by volume.
 *
 * Minimum kernel: IORING_OP_STATX — Linux 5.13
 * -------------------------------------------------------------------------
 */
struct fsal_uring_statx_ctx {
	int dirfd;
	const char *pathname; /**< "" for AT_EMPTY_PATH (fstat equiv) */
	int flags; /**< AT_EMPTY_PATH |
				     *   AT_SYMLINK_NOFOLLOW etc. */
	unsigned int mask; /**< STATX_BASIC_STATS or specific bits */
	struct statx *buf; /**< Heap-allocated; owned by done_cb */
	struct uring_pers *pers;
};

/** sqe_pre for statx — defined in fsal_uring.c */
void fsal_uring_statx_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 6 — RENAMEAT2  (renamefile / handle.c:1578)
 *
 * flags == 0 for a plain rename; flags == RENAME_EXCHANGE for atomic swap
 * (used by some NFSv4 servers).
 *
 * Minimum kernel: IORING_OP_RENAMEAT — Linux 5.11
 * -------------------------------------------------------------------------
 */
struct fsal_uring_rename_ctx {
	int oldfd;
	const char *old_name;
	int newfd;
	const char *new_name;
	unsigned int flags; /**< 0 or RENAME_EXCHANGE */
	struct uring_pers *pers;
};

/** sqe_pre for renameat — defined in fsal_uring.c */
void fsal_uring_rename_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 7 — UNLINKAT  (file_unlink and error-path cleanup)
 *
 * Primary site: handle.c:1662
 * Error-path unlinks: handle.c:721, 938, 1134, file.c:1050
 *
 * flags == 0 for files; flags == AT_REMOVEDIR for directories.
 *
 * Minimum kernel: IORING_OP_UNLINKAT — Linux 5.11
 * -------------------------------------------------------------------------
 */
struct fsal_uring_unlink_ctx {
	int dfd;
	const char *name;
	int flags; /**< 0 or AT_REMOVEDIR */
	struct uring_pers *pers;
};

/** sqe_pre for unlinkat — defined in fsal_uring.c */
void fsal_uring_unlink_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 8 — MKDIRAT  (makedir / handle.c:635)
 *
 * Minimum kernel: IORING_OP_MKDIRAT — Linux 5.15
 * -------------------------------------------------------------------------
 */
struct fsal_uring_mkdir_ctx {
	int dfd;
	const char *name;
	mode_t mode;
	struct uring_pers *pers;
};

/** sqe_pre for mkdirat — defined in fsal_uring.c */
void fsal_uring_mkdir_prep(struct io_uring_sqe *sqe, void *arg,
			   unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 9 — SYMLINKAT  (makesymlink / handle.c:1047)
 *
 * Note on argument order: io_uring_prep_symlinkat(sqe, target, dfd, linkpath)
 * matches symlinkat(target, dfd, linkpath) from <unistd.h>.
 *
 * Minimum kernel: IORING_OP_SYMLINKAT — Linux 5.11
 * -------------------------------------------------------------------------
 */
struct fsal_uring_symlink_ctx {
	const char *target; /**< Link content — what the symlink
			       *   points to */
	int dfd; /**< Directory fd */
	const char *linkpath; /**< Name of the new symlink */
	struct uring_pers *pers;
};

/** sqe_pre for symlinkat — defined in fsal_uring.c */
void fsal_uring_symlink_prep(struct io_uring_sqe *sqe, void *arg,
			     unsigned int ring_idx);

/* -------------------------------------------------------------------------
 * Op 10 — LINKAT  (linkfile via fsal_handle_syscalls.h:109)
 *
 * VFS uses: linkat(oldfd, "", newfd, new_name, AT_EMPTY_PATH)
 * This requires CAP_DAC_READ_SEARCH — io_uring issues the same syscall so
 * the capability requirement is unchanged.
 *
 * Minimum kernel: IORING_OP_LINKAT — Linux 5.11
 * -------------------------------------------------------------------------
 */
struct fsal_uring_linkat_ctx {
	int oldfd;
	const char *oldname; /**< "" when using AT_EMPTY_PATH */
	int newfd;
	const char *newname;
	int flags; /**< AT_EMPTY_PATH when linking by fd */
	struct uring_pers *pers;
};

/** sqe_pre for linkat — defined in fsal_uring.c */
void fsal_uring_linkat_prep(struct io_uring_sqe *sqe, void *arg,
			    unsigned int ring_idx);

/* =========================================================================
 * Per-op outer request structs
 *
 * Each VFS call site heap-allocates one of these (gsh_calloc with
 * MEM_COMP_FSAL).  The fsal_uring_meta_req base is always first so that
 * fsal_uring_meta_done() can cast cb_arg to struct fsal_uring_meta_req *
 * and then gsh_free the whole outer struct.
 *
 * Callers may add any op-specific fields after the base — for example,
 * statx_req keeps a pointer to the statx buffer that the done_cb must free.
 * =========================================================================
 */

/** Outer request for vfs_commit2 (SYNC_FILE_RANGE) */
struct fsal_uring_sync_req {
	struct fsal_uring_meta_req base;
	int fd; /**< Kept for logging in done_cb */
};

/** Outer request for vfs_fallocate (FALLOCATE) */
struct fsal_uring_falloc_req {
	struct fsal_uring_meta_req base;
};

/** Outer request for vfs_open2 create-by-name (OPENAT) */
struct fsal_uring_openat_req {
	struct fsal_uring_meta_req base;
	/**
	 * Result fd, written by fsal_uring_openat_done before calling done_cb.
	 * done_cb can read it from op_ctx or via caller_arg convention.
	 */
	int result_fd;
};

/** Outer request for fire-and-forget close (CLOSE) */
struct fsal_uring_close_req {
	struct fsal_uring_meta_req base;
	int fd; /**< Kept for logging in done_cb */
};

/** Outer request for fetch_attrs / fstat (STATX) */
struct fsal_uring_statx_req {
	struct fsal_uring_meta_req base;
	/**
	 * Heap-allocated statx result buffer.
	 * fsal_uring_statx_done maps it to fsal_attrlist then frees it.
	 */
	struct statx *statx_buf;
};

/** Outer request for renamefile (RENAMEAT2) */
struct fsal_uring_rename_req {
	struct fsal_uring_meta_req base;
};

/** Outer request for file_unlink (UNLINKAT) */
struct fsal_uring_unlink_req {
	struct fsal_uring_meta_req base;
};

/** Outer request for makedir (MKDIRAT) */
struct fsal_uring_mkdir_req {
	struct fsal_uring_meta_req base;
};

/** Outer request for makesymlink (SYMLINKAT) */
struct fsal_uring_symlink_req {
	struct fsal_uring_meta_req base;
};

/** Outer request for linkfile (LINKAT) */
struct fsal_uring_linkat_req {
	struct fsal_uring_meta_req base;
};

/* =========================================================================
 * Convenience macro — wiring a synchronous call to async + fallback
 *
 * Usage pattern at every VFS call site:
 *
 *   struct fsal_uring_<op>_ctx ctx = { .fd = fd, ..., .pers = pers };
 *   struct fsal_uring_<op>_req *req =
 *       gsh_calloc(1, sizeof(*req), MEM_COMP_FSAL);
 *
 *   if (engine && fsal_uring_meta_start(engine, &req->base,
 *                                        fsal_uring_<op>_prep, &ctx,
 *                                        obj_hdl, done_cb, caller_arg) == 0)
 *       return;   // in-flight, done_cb will fire
 *
 *   gsh_free(req, MEM_COMP_FSAL);
 *   engine unavailable — continue to synchronous syscall
 * =========================================================================
 */

#endif /* USE_FSAL_URING */
#endif /* FSAL_URING_META_H */
