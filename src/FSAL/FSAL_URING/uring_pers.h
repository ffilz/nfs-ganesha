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
 * @brief io_uring personality registry — credential handling for io_uring.
 *
 * Classification
 * ==============
 * Every symbol in this file is prefixed uring_pers_*.  A single grep on that
 * prefix gives the complete credential surface of the io_uring path.  Nothing
 * in this file is used outside the io_uring engine.
 *
 * Problem being solved
 * ====================
 * io_uring executes I/O under the credential of whichever thread called
 * io_uring_submit() (or, for SQPOLL, the kernel poller thread).  NFS-Ganesha
 * uses a shared worker pool, so the submitting thread is almost never the
 * same thread that called fsal_set_credentials() with the client's uid/gid.
 * Three concrete breakage modes are documented in URING_PERSONALITIES_DESIGN.md
 * sections 2.2, 2.3 and 2.4.
 *
 * io_uring personalities (IORING_OP_REGISTER_PERSONALITY) fix this by
 * snapshotting a credential into the ring at registration time.  Any later
 * SQE that carries sqe->personality = <id> executes under that credential
 * regardless of which thread submits it.
 *
 * Design
 * ======
 * Each distinct (uid, gid, garray) triple is interned once into a per-engine
 * AVL tree (keyed by a 64-bit hash, then memcmp for collision resolution).
 * The interned entry:
 *
 *   - owns a deep copy of garray (the RPC buffer copy disappears after the
 *     request; a cached entry must not borrow it)
 *   - holds one io_uring personality id per ring (ids differ across rings)
 *   - carries a 64-bit refcount: incremented at fsal_uring_io_start(),
 *     decremented at fsal_uring_io_finish() — covering unflushed SQEs too
 *   - is marked !valid on invalidation (export removal / client removal /
 *     config reload) but is only unregistered from the ring once refcnt == 0
 *
 * Hot path (>= 99.9% of I/Os)
 * ----------------------------
 *   uring_pers_for_req()        -- worker thread, called once per request
 *     rdlock -> AVL hit -> refcnt++ -> unlock   (~100 ns, no syscall)
 *   uring_pers_attach(sqe, p, ring_idx)
 *     sqe->personality = p->ids[ring_idx]       (inline, zero cost)
 *
 * Slow path (~0.001% of I/Os — first use of a new identity)
 * -----------------------------------------------------------
 *   uring_pers_for_req() -> AVL miss ->
 *     wrlock -> insert REGISTERING placeholder -> unlock ->
 *     fsal_set_credentials() (per-thread setresuid/setgroups) ->
 *     io_uring_register_personality() x nr_rings ->
 *     fsal_restore_ganesha_credentials() ->
 *     wrlock -> state = VALID -> broadcast waiters -> unlock
 *
 * Eviction safety
 * ---------------
 * io_uring recycles personality ids cyclically.  Unregistering an id while
 * a prepared-but-not-submitted SQE still references it assigns that id to a
 * different user — a security defect.  Rule: never call
 * io_uring_unregister_personality() while refcnt > 0.  Invalidation marks
 * the entry !valid (new lookups miss) without touching the ring; the actual
 * unregister happens in uring_pers_put() when refcnt drops to 0.
 *
 * Fallback
 * --------
 * If the cache is full (nr >= max) or registration fails, uring_pers_for_req()
 * returns NULL.  Callers must then disable coalescing for that SQE and fall
 * back to the original fsal_set_credentials() + submit path (same as today).
 */

#ifndef URING_PERS_H
#define URING_PERS_H

#ifdef USE_FSAL_URING

#include <liburing.h>
#undef UNUSED /* liburing may define UNUSED as object-like; Ganesha needs
		 * the function-like version from common_utils.h */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "avltree.h"
#include "gsh_list.h"
#include "fsal_types.h" /* struct user_cred */
#include "io_uring/core/uring_engine.h" /* struct uring_engine,
					 * uring_engine_raw_ring,
					 * uring_engine_nr_rings */

/* -------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------
 */

struct uring_pers_registry; /* opaque; one per engine */
struct uring_pers;

/* -------------------------------------------------------------------------
 * Public structs
 * -------------------------------------------------------------------------
 */

/**
 * @brief One interned credential identity + its per-ring personality ids.
 *
 * Allocated by uring_pers_add(); freed when refcnt reaches 0.
 * The flexible array @a ids[] holds one uint16_t per ring; size is fixed at
 * allocation time from engine->nr_rings.
 *
 * Fields written only under registry->lock (wrlock):
 *   valid, ids[], node, lru
 * Fields written atomically (no lock needed for individual ops):
 *   refcnt
 * Fields written once at allocation and then read-only:
 *   hash, uid, gid, glen, garray, cred_idx, reg
 */
struct uring_pers {
	/* --- AVL key material (immutable after insert) ------------------- */
	struct avltree_node node; /* keyed by hash, then memcmp        */
	uint64_t hash; /* hash(uid, gid, glen, garray)      */

	/* --- Credential copy (immutable after allocation) ---------------- */
	uid_t uid;
	gid_t gid;
	unsigned int glen;
	gid_t *garray; /* OWNED deep copy — see header */

	/** Dense monotonic index; key into per-engine id arrays.
	 *  Never recycled within a process run. */
	uint32_t cred_idx;

	/* --- Lifecycle --------------------------------------------------- */
	/** Refcount: number of in-flight SQEs + the tree/LRU's own ref.
	 *  Decremented by uring_pers_put(); unregister fires at 0. */
	int64_t refcnt; /* atomic (gcc __atomic builtins)    */

	/** Registration state — read under rdlock, written under wrlock. */
	enum {
		PERS_REGISTERING = 0, /* placeholder; waiters spin/wait    */
		PERS_VALID, /* ids[] are usable                  */
		PERS_INVALID, /* new lookups must miss this entry  */
	} state;

	/** Condition variable for REGISTERING -> VALID transition.
	 *  Guarded by registry->lock (used as the associated mutex). */
	pthread_cond_t cv;

	/** LRU list; newest at tail.  Protected by registry->lock. */
	struct glist_head lru;

	/** Back-reference to the registry that owns this entry. */
	struct uring_pers_registry *reg;

	/* --- Per-ring personality ids (valid once state == PERS_VALID) --- */
	/** Flexible array: one uint16_t per ring.  Allocated inline with the
	 *  struct by uring_pers_alloc().  Value 0 = no personality on that
	 *  ring.  Written once (at registration) then read-only.
	 *  MUST stay the last member — C requires the flexible array at the
	 *  end of the struct. */
	uint16_t ids[]; /* [nr_rings]                        */
};

/**
 * @brief Per-engine personality registry.
 *
 * Allocated by uring_pers_create(); destroyed by uring_pers_destroy().
 */
struct uring_pers_registry {
	pthread_rwlock_t lock;
	struct avltree tree;
	struct glist_head lru; /* LRU list of all valid entries     */
	uint32_t nr; /* current number of entries         */
	uint32_t max; /* capacity cap (default 8192)       */

	/** Next cred_idx to allocate.  Monotonically increasing; never 0. */
	uint32_t next_idx;

	/** Back-reference to the core engine (for nr_rings and ring ops). */
	struct uring_engine *core_engine;

	/* --- Statistics (best-effort, no lock) --------------------------- */
	uint64_t stat_hit;
	uint64_t stat_miss;
	uint64_t stat_evict;
	uint64_t stat_fail; /* registration errors       */
	uint64_t stat_fallback; /* returned NULL -> fallback */
};

/* -------------------------------------------------------------------------
 * Lifecycle  (called by fsal_uring_engine_create / _destroy, never by FSALs)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Allocate and initialise a personality registry for a core engine.
 *
 * @param core    The core engine (rings must already be started).
 * @param max     Maximum number of cached personalities (0 = use default 8192).
 * @param reg_out Receives the allocated registry on success.
 * @return        0 on success, -errno on failure.
 */
int uring_pers_create(struct uring_engine *core, uint32_t max,
		      struct uring_pers_registry **reg_out);

/**
 * @brief Destroy a personality registry and unregister all personalities.
 *
 * Must be called only after all I/O is complete (no in-flight SQEs).
 */
void uring_pers_destroy(struct uring_pers_registry *reg);

/* -------------------------------------------------------------------------
 * Slow path — lookup-or-register  (worker thread only)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Look up or register the personality for op_ctx->creds.
 *
 * This is the one entry point an FSAL calls.  It folds the intern lookup,
 * the optional registration (cache miss), and the reference increment into
 * a single call so vfs_write2()/vfs_read2() never touch the registry
 * directly.
 *
 * On a cache HIT:  rdlock -> AVL lookup -> refcnt++ -> unlock.  ~100 ns.
 * On a cache MISS: wrlock -> insert REGISTERING ->
 *                  fsal_set_credentials() -> io_uring_register_personality()
 *                  x nr_rings -> fsal_restore_ganesha_credentials() ->
 *                  wrlock -> VALID -> broadcast.
 *
 * PRECONDITIONS:
 *   - caller is a worker thread that may call setresuid/setgroups
 *   - caller is NOT already impersonating (see URING_PERSONALITIES_DESIGN.md
 *     section 9.5.3)
 *   - op_ctx is valid and op_ctx->creds are resolved
 *
 * @param reg  The engine's registry.
 * @return     A uring_pers with refcnt incremented, or NULL if the cache is
 *             full or registration failed.  NULL means the caller must fall
 *             back to the old impersonate-and-submit path.
 */
/**
 * @brief How to put this thread into a client's identity, per FSAL.
 *
 * Registering a personality means impersonating the client just long enough
 * for io_uring_register_personality() to snapshot current_cred(). Which call
 * does that impersonation is an FSAL decision, not a generic one:
 *
 *   FSAL_VFS   vfs_set_credentials(), which honours the export's
 *              only_one_user mode and can legitimately FAIL
 *   FSAL_GPFS  the plain fsal_set_credentials() pair
 *
 * Hardcoding fsal_set_credentials() here would snapshot an identity the
 * owning FSAL would never have used synchronously.
 */
struct uring_pers_cred_ops {
	/**
	 * Assume @creds on the calling thread.
	 * @return false if the identity could not be assumed, in which case
	 *         registration is abandoned rather than recorded wrongly.
	 */
	bool (*set)(const struct user_cred *creds, void *arg);

	/** Restore the daemon identity. Must pair with a successful set(). */
	void (*restore)(void *arg);

	/** Opaque FSAL context, e.g. the struct fsal_module * for VFS. */
	void *arg;
};

/**
 * @brief Resolve the personality for op_ctx->creds.
 *
 * @param cred_ops  How to impersonate while taking the snapshot. NULL uses
 *                  the generic fsal_set_credentials() pair.
 */
struct uring_pers *
uring_pers_for_req(struct uring_pers_registry *reg,
		   const struct uring_pers_cred_ops *cred_ops);

/**
 * @brief Resolve the personality for an explicit credential.
 *
 * Same as uring_pers_for_req() but for an identity that is not
 * op_ctx->creds — an FSAL that needs to run a particular operation as
 * someone else. Deliberately not served from the per-request cache, which
 * only ever holds the caller's own identity.
 */
struct uring_pers *uring_pers_for_creds(
	struct uring_pers_registry *reg, const struct user_cred *creds,
	const struct uring_pers_cred_ops *cred_ops);

/* -------------------------------------------------------------------------
 * Reference counting
 * -------------------------------------------------------------------------
 */

/** Increment refcount.  Used when passing a uring_pers pointer to a new
 *  owner (e.g. copying into fsal_uring_io_req before submit). */
void uring_pers_ref(struct uring_pers *p);

/**
 * @brief Decrement refcount; unregister from all rings if it reaches 0.
 *
 * Safe to call from the reaper thread.
 */
void uring_pers_put(struct uring_pers *p);

/* -------------------------------------------------------------------------
 * Hot path — SQE stamping  (inline, zero cost)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Stamp sqe->personality with the cached id for ring @a ring_idx.
 *
 * Called inside fsal_uring_submit() for both the data SQE and the linked
 * fsync SQE.  If @a p is NULL (fallback path) the field is left at 0, which
 * means "use the submitting thread's credential" — correct for the
 * impersonate-and-submit fallback.
 *
 * @param sqe       SQE being prepared.
 * @param p         Personality from uring_pers_for_req(), or NULL.
 * @param ring_idx  Index of the ring this SQE is going into.
 */
static inline void uring_pers_attach(struct io_uring_sqe *sqe,
				     const struct uring_pers *p,
				     unsigned int ring_idx)
{
	sqe->personality = (p != NULL) ? p->ids[ring_idx] : 0;
}

/* -------------------------------------------------------------------------
 * Invalidation  (called from config/export/client removal paths)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Invalidate all personalities whose credentials were resolved under
 *        export @a export_id.
 *
 * Does not unregister immediately; unregister happens lazily when refcnt
 * reaches 0 in uring_pers_put().
 */
void uring_pers_sweep_export(struct uring_pers_registry *reg,
			     uint16_t export_id);

/**
 * @brief Invalidate all personalities associated with the given client id.
 */
void uring_pers_sweep_client(struct uring_pers_registry *reg,
			     uint64_t client_id);

/**
 * @brief Invalidate every personality in the registry (config reload).
 */
void uring_pers_sweep_all(struct uring_pers_registry *reg);

/* -------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------
 */

/** Log hit/miss/evict/fail counters at NIV_INFO. */
void uring_pers_log_stats(struct uring_pers_registry *reg);

#endif /* USE_FSAL_URING */
#endif /* URING_PERS_H */
