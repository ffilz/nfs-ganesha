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
 * All functions are prefixed uring_pers_*.  This is the ONLY file that calls
 * io_uring_register_personality() or io_uring_unregister_personality().
 * See uring_pers.h for the design narrative.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <string.h>

#include <liburing.h>
#undef UNUSED

#include "abstract_mem.h"
#include "common_utils.h"
#include "fsal.h"
#include "FSAL/access_check.h" /* fsal_set_credentials,
				 * fsal_restore_ganesha_credentials */
#include "FSAL/fsal_uring.h" /* LogUring */
#include "io_uring/core/uring_engine.h" /* struct uring_engine,
					 * uring_engine_raw_ring,
					 * uring_engine_nr_rings */
#include "log.h"

#include "uring_pers.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Compute a 64-bit hash over (uid, gid, glen, garray).
 *
 * FNV-1a over the raw bytes of each field.  Good enough for our table sizes
 * (max 8192 entries); collisions are resolved by the memcmp comparator.
 */
static uint64_t uring_pers_hash(uid_t uid, gid_t gid, unsigned int glen,
				const gid_t *garray)
{
	uint64_t h = 14695981039346656037ULL; /* FNV-1a offset basis */
	unsigned int i;
	const uint8_t *p;

#define FNV_MIX(val)                                \
	do {                                        \
		p = (const uint8_t *)&(val);        \
		for (i = 0; i < sizeof(val); i++) { \
			h ^= (uint64_t)p[i];        \
			h *= 1099511628211ULL;      \
		}                                   \
	} while (0)

	FNV_MIX(uid);
	FNV_MIX(gid);
	FNV_MIX(glen);
	for (i = 0; i < glen; i++)
		FNV_MIX(garray[i]);
#undef FNV_MIX

	return h;
}

/**
 * @brief AVL comparator: order by hash, then full memcmp on uid/gid/garray.
 *
 * Using hash as the primary key keeps the tree balanced for typical workloads.
 * The secondary memcmp handles the (astronomically rare) hash collision.
 */
static int uring_pers_cmp(const struct avltree_node *a,
			  const struct avltree_node *b)
{
	const struct uring_pers *pa =
		avltree_container_of(a, struct uring_pers, node);
	const struct uring_pers *pb =
		avltree_container_of(b, struct uring_pers, node);

	if (pa->hash < pb->hash)
		return -1;
	if (pa->hash > pb->hash)
		return 1;

	/* Same hash — compare fields lexicographically. */
	if (pa->uid != pb->uid)
		return (int)pa->uid - (int)pb->uid;
	if (pa->gid != pb->gid)
		return (int)pa->gid - (int)pb->gid;
	if (pa->glen != pb->glen)
		return (int)pa->glen - (int)pb->glen;
	if (pa->glen == 0)
		return 0;
	return memcmp(pa->garray, pb->garray, pa->glen * sizeof(gid_t));
}

/**
 * @brief Allocate a uring_pers node, deep-copying the garray.
 *
 * ids[] is a flexible array of nr_rings uint16_t values, zeroed (0 means
 * "no personality, use submitting thread's creds").
 */
static struct uring_pers *uring_pers_alloc(const struct user_cred *creds,
					   unsigned int nr_rings, uint64_t hash,
					   uint32_t cred_idx)
{
	struct uring_pers *p;
	/* ids[] is a C99 flexible array at the end of the struct.
	 * gsh_calloc zeroes everything, so ids[r] starts at 0 for all r. */
	p = gsh_calloc(1, sizeof(*p) + nr_rings * sizeof(uint16_t),
		       MEM_COMP_FSAL);

	p->hash = hash;
	p->uid = creds->caller_uid;
	p->gid = creds->caller_gid;
	p->glen = creds->caller_glen;
	p->cred_idx = cred_idx;
	/* ids[] already zeroed — no explicit assignment needed */

	if (creds->caller_glen > 0 && creds->caller_garray != NULL) {
		p->garray = gsh_malloc(creds->caller_glen * sizeof(gid_t),
				       MEM_COMP_FSAL);
		if (p->garray == NULL) {
			gsh_free(p, MEM_COMP_FSAL);
			return NULL;
		}
		memcpy(p->garray, creds->caller_garray,
		       creds->caller_glen * sizeof(gid_t));
	}

	pthread_cond_init(&p->cv, NULL);
	/* refcnt starts at 1: the tree's own reference. */
	__atomic_store_n(&p->refcnt, 1, __ATOMIC_RELEASE);
	p->state = PERS_REGISTERING;

	return p;
}

/**
 * @brief Unregister from all rings and free the entry.
 *
 * Only called when refcnt has just reached 0 inside uring_pers_put().
 */
static void uring_pers_free(struct uring_pers *p)
{
	struct uring_pers_registry *reg = p->reg;
	struct uring_engine *core = reg->core_engine;
	unsigned int nr_rings = uring_engine_nr_rings(core);
	unsigned int r;

	/* Unregister from every ring.  The personality was registered on all
	 * rings or on none (partial registration fails the whole entry). */
	if (p->state == PERS_VALID || p->state == PERS_INVALID) {
		for (r = 0; r < nr_rings; r++) {
			if (p->ids[r] == 0)
				continue;
			(void)io_uring_unregister_personality(
				uring_engine_raw_ring(core, r), p->ids[r]);
			LogUring(NIV_DEBUG,
				 "uring_pers: unregistered id %u on ring %u"
				 " (uid=%u gid=%u glen=%u idx=%u)",
				 p->ids[r], r, p->uid, p->gid, p->glen,
				 p->cred_idx);
		}
	}

	pthread_cond_destroy(&p->cv);
	gsh_free(p->garray, MEM_COMP_FSAL);
	gsh_free(p, MEM_COMP_FSAL);
}

/**
 * @brief Evict the LRU entry from the registry if it is idle (refcnt == 1,
 *        meaning only the tree holds a reference).
 *
 * PRECONDITION: called with registry->lock held as a write lock.
 * POSTCONDITION: if an entry was evicted, it is removed from the tree and LRU
 *               and returned with its tree reference still held so the caller
 *               can call uring_pers_free() after releasing the lock.
 *
 * Returns the evicted entry, or NULL if nothing is evictable.
 */
static struct uring_pers *uring_pers_evict_lru(struct uring_pers_registry *reg)
{
	struct uring_pers *p;
	struct glist_head *g;

	/* Walk from oldest (head) to newest (tail). */
	glist_for_each(g, &reg->lru) {
		p = glist_entry(g, struct uring_pers, lru);

		/* Only evict idle entries (tree ref is the sole reference). */
		if (__atomic_load_n(&p->refcnt, __ATOMIC_ACQUIRE) != 1)
			continue;
		/* Do not evict an entry still being registered. */
		if (p->state == PERS_REGISTERING)
			continue;

		/* Remove from tree and LRU. */
		avltree_remove(&p->node, &reg->tree);
		glist_del(&p->lru);
		reg->nr--;
		(void)__atomic_fetch_add(&reg->stat_evict, 1, __ATOMIC_RELAXED);

		LogUring(NIV_DEBUG,
			 "uring_pers: evicted uid=%u gid=%u glen=%u idx=%u"
			 " (cache full, nr=%u)",
			 p->uid, p->gid, p->glen, p->cred_idx, reg->nr);
		return p;
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Public API — lifecycle
 * -------------------------------------------------------------------------
 */

int uring_pers_create(struct uring_engine *core, uint32_t max,
		      struct uring_pers_registry **reg_out)
{
	struct uring_pers_registry *reg;

	reg = gsh_calloc(1, sizeof(*reg), MEM_COMP_FSAL);

	PTHREAD_RWLOCK_init(&reg->lock, NULL);
	avltree_init(&reg->tree, uring_pers_cmp, 0);
	glist_init(&reg->lru);

	reg->core_engine = core;
	reg->max = (max != 0) ? max : 8192;
	reg->next_idx = 1; /* 0 is reserved for "no personality" */

	*reg_out = reg;

	LogUring(NIV_INFO, "uring_pers: registry created, max=%u nr_rings=%u",
		 reg->max, uring_engine_nr_rings(core));
	return 0;
}

void uring_pers_destroy(struct uring_pers_registry *reg)
{
	struct avltree_node *n;
	struct uring_pers *p;

	if (reg == NULL)
		return;

	/* Walk the whole tree and free every entry regardless of refcnt.
	 * This is called at shutdown when no I/O should be in flight. */
	PTHREAD_RWLOCK_wrlock(&reg->lock);

	while ((n = avltree_first(&reg->tree)) != NULL) {
		p = avltree_container_of(n, struct uring_pers, node);
		avltree_remove(&p->node, &reg->tree);
		glist_del(&p->lru);

		if (__atomic_load_n(&p->refcnt, __ATOMIC_ACQUIRE) > 1) {
			LogUring(NIV_WARN,
				 "uring_pers: destroying entry"
				 " with refcnt=%" PRId64
				 " uid=%u gid=%u — I/O still in flight?",
				 __atomic_load_n(&p->refcnt, __ATOMIC_ACQUIRE),
				 p->uid, p->gid);
		}

		/* Force refcnt to 0 so uring_pers_free() unregisters. */
		__atomic_store_n(&p->refcnt, 0, __ATOMIC_RELEASE);
		uring_pers_free(p);
	}

	PTHREAD_RWLOCK_unlock(&reg->lock);
	PTHREAD_RWLOCK_destroy(&reg->lock);

	LogUring(NIV_INFO,
		 "uring_pers: registry destroyed"
		 " hit=%" PRIu64 " miss=%" PRIu64 " evict=%" PRIu64
		 " fail=%" PRIu64 " fallback=%" PRIu64,
		 reg->stat_hit, reg->stat_miss, reg->stat_evict, reg->stat_fail,
		 reg->stat_fallback);

	gsh_free(reg, MEM_COMP_FSAL);
}

/* -------------------------------------------------------------------------
 * Public API — slow path: lookup-or-register
 * -------------------------------------------------------------------------
 */

/**
 * @brief Undo the impersonation taken for a personality snapshot.
 *
 * Mirrors whichever primitive assumed the identity. FSAL_VFS skips the
 * restore entirely in only_one_user mode, so calling the generic
 * fsal_restore_ganesha_credentials() unconditionally would be wrong.
 */
static void uring_pers_restore_creds(const struct uring_pers_cred_ops *cred_ops)
{
	if (cred_ops != NULL && cred_ops->set != NULL) {
		if (cred_ops->restore != NULL)
			cred_ops->restore(cred_ops->arg);
		return;
	}

	fsal_restore_ganesha_credentials();
}

struct uring_pers *uring_pers_for_req(struct uring_pers_registry *reg,
				      const struct uring_pers_cred_ops
					      *cred_ops)
{
	return uring_pers_for_creds(reg, &op_ctx->creds, cred_ops);
}

struct uring_pers *uring_pers_for_creds(
	struct uring_pers_registry *reg, const struct user_cred *creds,
	const struct uring_pers_cred_ops *cred_ops)
{
	struct uring_engine *core = reg->core_engine;
	uint64_t hash;
	struct uring_pers key;
	struct uring_pers *p = NULL;
	struct avltree_node *n;
	struct uring_pers *evicted = NULL;
	uint32_t cred_idx;
	unsigned int r;
	int rc;

	/* Phase 1: optimistic read-locked lookup */
	hash = uring_pers_hash(creds->caller_uid, creds->caller_gid,
			       creds->caller_glen, creds->caller_garray);

	memset(&key, 0, sizeof(key));
	key.hash = hash;
	key.uid = creds->caller_uid;
	key.gid = creds->caller_gid;
	key.glen = creds->caller_glen;
	key.garray = creds->caller_garray; /* used only for comparison */

	PTHREAD_RWLOCK_rdlock(&reg->lock);

	n = avltree_lookup(&key.node, &reg->tree);
	if (n != NULL) {
		p = avltree_container_of(n, struct uring_pers, node);

		if (p->state == PERS_VALID) {
			/* Fast hit: bump refcnt and return. */
			__atomic_fetch_add(&p->refcnt, 1, __ATOMIC_ACQ_REL);
			__atomic_fetch_add(&reg->stat_hit, 1, __ATOMIC_RELAXED);
			/* Touch LRU: would need wrlock — skip for hot path.
			 * LRU order is best-effort; correctness does not
			 * depend on it. */
			PTHREAD_RWLOCK_unlock(&reg->lock);
			LogUring(NIV_FULL_DEBUG,
				 "uring_pers: HIT uid=%u gid=%u idx=%u", p->uid,
				 p->gid, p->cred_idx);
			return p;
		}

		/* REGISTERING or INVALID — retry under write-lock path,
		 * which will either wait for registration or miss and
		 * re-register. */
		p = NULL;
	}

	PTHREAD_RWLOCK_unlock(&reg->lock);

	/* Phase 2: write-locked lookup + insert */
	__atomic_fetch_add(&reg->stat_miss, 1, __ATOMIC_RELAXED);

	PTHREAD_RWLOCK_wrlock(&reg->lock);

	/*
	 * Re-check under write lock: another worker may have won the race.
	 *
	 * A race loser must not park on pthread_cond_wait() here. reg->lock is
	 * a pthread_rwlock_t, and pthread_cond_wait() unlocks its second
	 * argument as a pthread_mutex_t — passing a rwlock is undefined and
	 * corrupts the lock. Instead drop the lock, yield, and repeat the
	 * lookup from scratch, because the winner may have removed or freed
	 * the entry while we were not holding the lock.
	 */
relookup:
	n = avltree_lookup(&key.node, &reg->tree);
	if (n != NULL) {
		p = avltree_container_of(n, struct uring_pers, node);

		if (p->state == PERS_REGISTERING) {
			/* Race loser: let winner finish, then look again. */
			LogUring(NIV_DEBUG,
				 "uring_pers: waiting for REGISTERING uid=%u gid=%u",
				 p->uid, p->gid);
			PTHREAD_RWLOCK_unlock(&reg->lock);
			sched_yield();
			PTHREAD_RWLOCK_wrlock(&reg->lock);
			goto relookup;
		}

		if (p->state == PERS_VALID) {
			__atomic_fetch_add(&p->refcnt, 1, __ATOMIC_ACQ_REL);
			PTHREAD_RWLOCK_unlock(&reg->lock);
			return p;
		}

		/* INVALID (invalidated while we waited): treat as a
		 * miss — the old entry stays in the tree until its
		 * refcnt reaches 0. */
		p = NULL;
	}

	/* Phase 3: capacity check + optional eviction */
	if (reg->nr >= reg->max) {
		evicted = uring_pers_evict_lru(reg);
		if (evicted == NULL) {
			/* All entries are in flight — cannot evict. */
			PTHREAD_RWLOCK_unlock(&reg->lock);
			__atomic_fetch_add(&reg->stat_fallback, 1,
					   __ATOMIC_RELAXED);
			LogUring(NIV_WARN,
				 "uring_pers: cache full (nr=%u max=%u),"
				 " falling back to impersonation",
				 reg->nr, reg->max);
			return NULL;
		}
	}

	/* Phase 4: allocate placeholder and insert */
	cred_idx = reg->next_idx++;

	p = uring_pers_alloc(creds, uring_engine_nr_rings(core), hash,
			     cred_idx);
	if (p == NULL) {
		PTHREAD_RWLOCK_unlock(&reg->lock);
		if (evicted != NULL)
			uring_pers_free(evicted);
		__atomic_fetch_add(&reg->stat_fail, 1, __ATOMIC_RELAXED);
		LogUring(NIV_WARN,
			 "uring_pers: OOM allocating entry uid=%u gid=%u",
			 creds->caller_uid, creds->caller_gid);
		return NULL;
	}

	p->reg = reg;
	p->state = PERS_REGISTERING;

	avltree_insert(&p->node, &reg->tree);
	glist_add_tail(&reg->lru, &p->lru);
	reg->nr++;

	/* Release the write lock BEFORE the syscalls.  Waiters can see the
	 * REGISTERING placeholder and will cond_wait on p->cv. */
	PTHREAD_RWLOCK_unlock(&reg->lock);

	/* Free the evicted entry now that we hold no lock. */
	if (evicted != NULL)
		uring_pers_free(evicted);

	/* Phase 5: impersonate and register */
	/*
	 * We are still running as Ganesha root (euid=0, ruid=0).
	 * fsal_set_credentials() switches this thread's euid/egid/groups to
	 * the client's identity.  The EFFECTIVE capability set becomes empty
	 * (unless uid==0 and no_root_squash), which is exactly the credential
	 * snapshot io_uring_register_personality() captures.
	 *
	 * We must NOT be already impersonating when we arrive here — see
	 * URING_PERSONALITIES_DESIGN.md §9.5.3.
	 */
	/*
	 * Impersonate through the owning FSAL's primitive, not the generic
	 * one. FSAL_VFS honours only_one_user here and can refuse outright;
	 * snapshotting an identity it would never have assumed would hand
	 * the kernel a credential the synchronous path considered a
	 * permission failure.
	 */
	if (cred_ops != NULL && cred_ops->set != NULL) {
		if (!cred_ops->set(creds, cred_ops->arg)) {
			LogUring(NIV_DEBUG,
				 "uring_pers: FSAL refused to assume uid=%u gid=%u; no personality",
				 creds->caller_uid, creds->caller_gid);
			goto fail_register;
		}
	} else {
		fsal_set_credentials(creds);
	}

	for (r = 0; r < uring_engine_nr_rings(core); r++) {
		rc = io_uring_register_personality(
			uring_engine_raw_ring(core, r));
		if (rc < 0) {
			LogUring(NIV_WARN,
				 "uring_pers: io_uring_register_personality"
				 " failed on ring %u: %s"
				 " (uid=%u gid=%u glen=%u) — fallback",
				 r, strerror(-rc), creds->caller_uid,
				 creds->caller_gid, creds->caller_glen);
			/* Undo any ids registered so far. */
			while (r-- > 0) {
				(void)io_uring_unregister_personality(
					uring_engine_raw_ring(core, r),
					p->ids[r]);
				p->ids[r] = 0;
			}
			uring_pers_restore_creds(cred_ops);
			goto fail_register;
		}

		p->ids[r] = (uint16_t)rc;

		LogUring(NIV_DEBUG,
			 "uring_pers: registered id=%u on ring %u"
			 " (uid=%u gid=%u glen=%u idx=%u)",
			 p->ids[r], r, p->uid, p->gid, p->glen, p->cred_idx);
	}

	uring_pers_restore_creds(cred_ops);

	/* Phase 6: publish */
	PTHREAD_RWLOCK_wrlock(&reg->lock);
	p->state = PERS_VALID;
	pthread_cond_broadcast(&p->cv);
	PTHREAD_RWLOCK_unlock(&reg->lock);

	/* Bump refcnt for the caller (tree already holds +1 from alloc). */
	__atomic_fetch_add(&p->refcnt, 1, __ATOMIC_ACQ_REL);

	LogUring(NIV_DEBUG,
		 "uring_pers: REGISTERED uid=%u gid=%u glen=%u idx=%u", p->uid,
		 p->gid, p->glen, p->cred_idx);
	return p;

fail_register:
	/* Remove the placeholder from the tree so future lookups miss. */
	PTHREAD_RWLOCK_wrlock(&reg->lock);
	avltree_remove(&p->node, &reg->tree);
	glist_del(&p->lru);
	reg->nr--;
	p->state = PERS_INVALID;
	pthread_cond_broadcast(&p->cv); /* wake any waiters; they will retry */
	PTHREAD_RWLOCK_unlock(&reg->lock);

	__atomic_fetch_add(&reg->stat_fail, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&reg->stat_fallback, 1, __ATOMIC_RELAXED);

	/* Drop the tree's reference; since we hold the only ref, free now. */
	__atomic_store_n(&p->refcnt, 0, __ATOMIC_RELEASE);
	uring_pers_free(p);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Public API — reference counting
 * -------------------------------------------------------------------------
 */

void uring_pers_ref(struct uring_pers *p)
{
	__atomic_fetch_add(&p->refcnt, 1, __ATOMIC_ACQ_REL);
}

void uring_pers_put(struct uring_pers *p)
{
	int64_t prev;

	if (p == NULL)
		return;

	prev = __atomic_fetch_sub(&p->refcnt, 1, __ATOMIC_ACQ_REL);
	assert(prev > 0);

	if (prev != 1)
		return; /* still referenced */

	/* refcnt hit 0 — unregister and free.
	 *
	 * The entry was already removed from the tree either by
	 * uring_pers_evict_lru() or by uring_pers_sweep_*() (which both hold
	 * the write lock when they do so).  By the time refcnt reaches 0 the
	 * entry is no longer visible to new lookups, so no new SQEs can pick
	 * up the ids[] after this point.  It is therefore safe to call
	 * io_uring_unregister_personality() without any lock.
	 */
	uring_pers_free(p);
}

/* -------------------------------------------------------------------------
 * Public API — invalidation / sweep
 * -------------------------------------------------------------------------
 */

/**
 * @brief Internal: walk the whole tree, mark entries invalid, remove from
 *        tree+LRU, collect idle ones for immediate free.
 *
 * Entries with refcnt > 1 are only marked INVALID; uring_pers_put() will
 * call uring_pers_free() when the last SQE completes.
 *
 * @param reg      The registry.
 * @param match_fn A predicate that returns true for entries to invalidate,
 *                 or NULL to invalidate all.
 * @param match_arg Passed through to match_fn.
 */
typedef bool (*uring_pers_match_fn)(const struct uring_pers *p, void *arg);

static void uring_pers_sweep(struct uring_pers_registry *reg,
			     uring_pers_match_fn match_fn, void *match_arg)
{
	struct avltree_node *n;
	struct avltree_node *next;
	struct uring_pers *p;
	/* Collect idle entries to free after releasing the lock. */
	struct uring_pers *free_list[64];
	unsigned int nfree = 0;
	unsigned int i;

	PTHREAD_RWLOCK_wrlock(&reg->lock);

	for (n = avltree_first(&reg->tree); n != NULL; n = next) {
		next = avltree_next(n);
		p = avltree_container_of(n, struct uring_pers, node);

		if (match_fn != NULL && !match_fn(p, match_arg))
			continue;
		if (p->state == PERS_INVALID)
			continue; /* already invalidated */

		p->state = PERS_INVALID;
		avltree_remove(&p->node, &reg->tree);
		glist_del(&p->lru);
		reg->nr--;

		/* If the tree held the only reference, collect for free. */
		if (__atomic_load_n(&p->refcnt, __ATOMIC_ACQUIRE) == 1) {
			if (nfree < 64)
				free_list[nfree++] = p;
			else
				/* Overflow: decrement so uring_pers_put()
				 * in a completion will free it later. */
				__atomic_fetch_sub(&p->refcnt, 1,
						   __ATOMIC_ACQ_REL);
		}
		/* else: in-flight; uring_pers_put() will call uring_pers_free
		 * when the last completion arrives. */
	}

	PTHREAD_RWLOCK_unlock(&reg->lock);

	/* Free idle entries without holding any lock. */
	for (i = 0; i < nfree; i++) {
		__atomic_store_n(&free_list[i]->refcnt, 0, __ATOMIC_RELEASE);
		uring_pers_free(free_list[i]);
	}
}

/* Match helpers for the sweep functions below. */

struct uring_pers_sweep_export_arg {
	uint16_t export_id;
};

static bool uring_pers_match_export(const struct uring_pers *p, void *arg)
{
	/* We do not currently tag entries with export_id, because the
	 * identity (uid/gid/garray) is already export-independent after
	 * squash_creds() has run.  A full sweep is therefore conservative
	 * but correct; entries are cheap to re-register.
	 *
	 * A future optimisation could tag entries with the set of exports
	 * they have been used for and only invalidate entries exclusive to
	 * the removed export.  For now, sweep all.
	 */
	(void)p;
	(void)arg;
	return true;
}

static bool uring_pers_match_client(const struct uring_pers *p, void *arg)
{
	/* Same reasoning as export: identity is already client-independent
	 * at this layer.  Conservative full sweep. */
	(void)p;
	(void)arg;
	return true;
}

void uring_pers_sweep_export(struct uring_pers_registry *reg,
			     uint16_t export_id)
{
	struct uring_pers_sweep_export_arg arg = { .export_id = export_id };

	LogUring(NIV_DEBUG, "uring_pers: sweeping export_id=%u", export_id);
	uring_pers_sweep(reg, uring_pers_match_export, &arg);
}

void uring_pers_sweep_client(struct uring_pers_registry *reg,
			     uint64_t client_id)
{
	LogUring(NIV_DEBUG, "uring_pers: sweeping client_id=%" PRIu64,
		 client_id);
	uring_pers_sweep(reg, uring_pers_match_client, &client_id);
}

void uring_pers_sweep_all(struct uring_pers_registry *reg)
{
	LogUring(NIV_DEBUG, "uring_pers: sweeping all entries");
	uring_pers_sweep(reg, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------
 */

void uring_pers_log_stats(struct uring_pers_registry *reg)
{
	LogUring(NIV_INFO,
		 "uring_pers: nr=%u max=%u"
		 " hit=%" PRIu64 " miss=%" PRIu64 " evict=%" PRIu64
		 " fail=%" PRIu64 " fallback=%" PRIu64,
		 reg->nr, reg->max, reg->stat_hit, reg->stat_miss,
		 reg->stat_evict, reg->stat_fail, reg->stat_fallback);
}

#endif /* USE_FSAL_URING */
