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
 * @file    uring_engine.c
 * @brief   Shared opcode-agnostic io_uring engine
 *
 * Provides multi-ring management, SQPOLL, adaptive coalescing,
 * registered buffer pooling, and per-ring completion reaper threads.
 * Opcode handling is delegated to the caller via the sqe_pre hook.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <liburing.h>

#undef UNUSED

#include "abstract_atomic.h"
#include "abstract_mem.h"
#include "common_utils.h"
#include "io_uring/core/uring_engine.h"

/*
 * Log severity aliases — mirrors Ganesha NIV_* integers.
 * Consumers that use Ganesha logging pass their own log_fn that
 * maps these back to NIV_* enum values.
 */
#define URING_LOG_CRIT 0
#define URING_LOG_WARN 4
#define URING_LOG_EVENT 5
#define URING_LOG_INFO 6
#define URING_LOG_DEBUG 7
#define URING_LOG_FULL_DEBUG 8

#define URING_LOG(e, lvl, fmt, ...)                                         \
	do {                                                                \
		if ((e)->params.log_fn)                                     \
			(e)->params.log_fn((e)->params.log_ctx, (lvl), fmt, \
					   ##__VA_ARGS__);                  \
	} while (0)

#define URING_DEF_QUEUE_DEPTH 256
#define URING_DEF_RINGS 1
#define URING_MAX_RINGS 512
#define URING_DEF_BUF_MIN (4 * 1024)
#define URING_DEF_BUF_MAX (1024 * 1024)
#define URING_DEF_POOL_BYTES (64UL * 1024 * 1024)
#define URING_MIN_BUF_MIN 4096
#define URING_MAX_CLASSES 16
/*
 * Hard kernel limit on the number of iovecs that can be passed to a single
 * io_uring_register_buffers() call.  The kernel enforces UIO_MAXIOV * 16
 * (1024 * 16 = 16384) across every Linux version that supports io_uring
 * (5.1+).  The constant is not exposed in uapi headers, so we define it here.
 * If a future kernel raises it the planner will simply be conservative.
 */
#define URING_MAX_FIXED_BUFS 16384
#define URING_DEF_SQPOLL_IDLE_MS 100
#define URING_DEF_COALESCE_BUCKET_IO_COUNT 1
#define URING_DEF_COALESCE_MAX_BUCKET_IO_COUNT 64
#define URING_DEF_COALESCE_TIMEOUT_US 100
#define URING_DEF_COALESCE_OUTSTANDING_MAX 128
#define URING_DEF_COALESCE_CIF_THRESHOLD 4

/*
 * A request pointer is at least 8 byte aligned, so the low bit of
 * sqe->user_data is free to say which of the (up to two) chained SQEs a
 * completion belongs to.
 */
#define URING_TAG_FSYNC 0x1UL
#define URING_TAG_MASK 0x1UL

/*
 * Bit 1 marks a CQE belonging to a linked chain (uring_engine_submit_chain).
 * Its user_data points at a struct uring_chain_slot rather than a
 * struct uring_req, so the reaper has to test this before it tries to read
 * the pointer as a uring_req.  Both objects are at least 8 byte aligned, so
 * bits 0-2 are available and the two encodings cannot collide.
 */
#define URING_TAG_CHAIN 0x2UL
#define URING_TAG_CHAIN_MASK 0x7UL

/*
 * Width of the cached opcode-support bitmap. IORING_OP_LAST grows with every
 * kernel release, so the bitmap is sized well past it rather than tracking
 * it; opcodes at or beyond this bound simply report unsupported.
 */
#define URING_OP_MASK_BITS 256
#define URING_OP_MASK_WORDS (URING_OP_MASK_BITS / 64)

/* io_uring_submit() retries for the transient -EBUSY/-EAGAIN cases. */
#define URING_SUBMIT_RETRIES 16

/** One in-flight submission. pending==2 when a linked second SQE is present. */
struct uring_req {
	void (*cb)(void *cb_arg, int res, int res2);
	void *cb_arg;
	int32_t res;
	int32_t res2;
	int32_t pending;
};

/** One ring with its submission lock and reaper thread. */
struct uring_ring {
	struct io_uring ring;
	pthread_mutex_t submit_lock;
	/**
	 * CQ consumer lock.
	 *
	 * The completion ring is single-consumer: io_uring_peek_cqe() plus
	 * io_uring_cqe_seen() advance the head non-atomically, so two
	 * threads doing it concurrently corrupt it. The reaper is no longer
	 * the only consumer -- a worker that has just submitted can poll
	 * for its own completion (uring_engine_poll()) rather than wait for
	 * the reaper to hand it back -- so the dequeue is serialised here.
	 *
	 * Held only to copy CQEs out of the ring. The callbacks run after
	 * it is dropped, so completion processing stays parallel.
	 */
	pthread_mutex_t cq_lock;
	pthread_t reaper;
	bool reaper_started;
	bool ring_init;
	unsigned int idx;
	struct uring_engine *engine;

	/*
	 * Per-ring coalescing state. All live in gsh_calloc'd memory, so
	 * they start at zero without explicit initialisation.
	 */

	/** SQEs prepared but not yet submitted; resets to 0 after each
	 *  flush, inside submit_lock only. */
	uint32_t io_count;
	/** SQEs submitted to the kernel, not yet CQE'd; decremented by the
	 *  reaper per CQE, no lock. */
	uint32_t outstanding_io;
	/** Monotonic µs of the first SQE in the current batch; 0 when no
	 *  batch is pending. */
	uint64_t first_io_time_us;
	/** Largest io_count seen at submit time, best-effort, no lock. */
	uint32_t stat_peak_batch;
};

/** One size class of the registered buffer pool. */
struct uring_class {
	/** Buffer size for this class, a power of two. */
	size_t size;
	unsigned int nr;
	/** First registered buffer index belonging to this class. */
	unsigned int base;
	unsigned int *free_stack;
	unsigned int free_top;
	pthread_mutex_t lock;
	uint64_t stat_get;
	uint64_t stat_empty;
};

struct uring_engine {
	struct uring_engine_params params; /* copy of init-time config */
	char name[24];

	unsigned int nr_rings;
	struct uring_ring *rings;
	uint32_t next_ring;
	bool per_cpu_rings;

	/* Registered buffer pool. Empty when nr_bufs == 0. */
	unsigned int nr_bufs;
	unsigned int nr_classes;
	struct uring_class *classes;
	/** Size of the largest class, the most a fixed buffer can hold. */
	size_t buf_max;
	void *buf_area;
	size_t buf_area_size;
	struct iovec *buf_iov;
	struct uring_buf *bufs;
	bool bufs_registered;

	/**
	 * Opcodes this kernel implements, one bit per IORING_OP_* value.
	 * Filled in once at create time from IORING_REGISTER_PROBE and read
	 * lock-free thereafter by uring_engine_op_supported().
	 */
	uint64_t op_mask[URING_OP_MASK_WORDS];
	/** True if the kernel answered the probe at all. */
	bool op_mask_valid;

	/** Set once the SQ state can no longer be trusted. */
	bool broken;
	/** Tells the reapers to wind down. */
	bool stopping;

	/** Tag every SQE with IOSQE_ASYNC. */
	bool force_async;
	/** True once the rings actually came up with SQPOLL. */
	bool sqpoll;

	uint64_t stat_submitted;
	uint64_t stat_completed;
	uint64_t stat_failed;
	uint64_t stat_no_sqe;
	uint64_t stat_no_buf;

	/* Resolved coalesce parameters (defaults applied at engine_create). */
	/** Master switch; false means submit each SQE immediately. */
	bool coalesce_enable;
	uint32_t coalesce_bucket_io_count;
	uint32_t coalesce_max_bucket_io_count;
	uint64_t coalesce_timeout_us;
	uint32_t coalesce_outstanding_max;
	uint32_t coalesce_cif_threshold;
};
/* --- Registered buffer pool --- */

/** Smallest power of two >= @a n, for normalising the class endpoints. */
static size_t uring_roundup_pow2(size_t n)
{
	size_t p = 1;

	while (p < n)
		p *= 2;

	return p;
}

/** Largest power of two <= @a n, for snapping slot counts to clean values. */
static size_t uring_rounddown_pow2(size_t n)
{
	size_t p = 1;

	if (n < 1)
		return 1;

	while (p * 2 <= n)
		p *= 2;

	return p;
}

size_t uring_buf_max_size(struct uring_engine *engine)
{
	return engine->nr_bufs != 0 ? engine->buf_max : 0;
}

struct uring_buf *uring_buf_get(struct uring_engine *engine, size_t len)
{
	struct uring_class *cls;
	struct uring_buf *buf = NULL;
	unsigned int free_left;
	unsigned int i;

	if (engine->nr_bufs == 0 || len > engine->buf_max)
		return NULL;

	/* Smallest class that fits. Classes are ordered by size, and there
	 * are only a handful, so a scan is cheaper than anything cleverer.
	 */
	for (i = 0; i < engine->nr_classes; i++) {
		if (len <= engine->classes[i].size)
			break;
	}

	cls = &engine->classes[i];

	PTHREAD_MUTEX_lock(&cls->lock);

	if (cls->free_top > 0)
		buf = &engine->bufs[cls->free_stack[--cls->free_top]];

	free_left = cls->free_top;

	PTHREAD_MUTEX_unlock(&cls->lock);

	if (buf == NULL) {
		/* Deliberately not promoting to a larger class: a burst of
		 * small I/O would eat the big buffers, and large transfers are
		 * precisely the ones that gain most from being pre-pinned. A
		 * small heap buffer is cheap; losing the 1 MiB class is not.
		 */
		(void)atomic_inc_uint64_t(&cls->stat_empty);
		(void)atomic_inc_uint64_t(&engine->stat_no_buf);
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: %zu byte class exhausted, all %u in use"
			  " - this %zu byte read falls back to a"
			  " heap buffer",
			  engine->name, cls->size, cls->nr, len);
		return NULL;
	}

	(void)atomic_inc_uint64_t(&cls->stat_get);

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "%s: pool get buf %u for %zu bytes from the %zu byte class (%u/%u free)",
		  engine->name, buf->index, len, cls->size, free_left, cls->nr);

	return buf;
}

void uring_buf_put(struct uring_buf *buf)
{
	struct uring_engine *engine;
	struct uring_class *cls;
	unsigned int free_left;

	if (buf == NULL)
		return;

	engine = buf->engine;
	cls = &engine->classes[buf->cls];

	PTHREAD_MUTEX_lock(&cls->lock);
	cls->free_stack[cls->free_top++] = buf->index;
	free_left = cls->free_top;
	PTHREAD_MUTEX_unlock(&cls->lock);

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "%s: pool put buf %u to the %zu byte class (%u/%u free)",
		  engine->name, buf->index, cls->size, free_left, cls->nr);
}

static void uring_pool_destroy(struct uring_engine *engine)
{
	unsigned int i;

	if (engine->nr_bufs == 0)
		return;

	for (i = 0; i < engine->nr_classes; i++) {
		gsh_free(engine->classes[i].free_stack,
			 engine->params.mem_comp);
		PTHREAD_MUTEX_destroy(&engine->classes[i].lock);
	}

	gsh_free(engine->classes, engine->params.mem_comp);
	gsh_free(engine->bufs, engine->params.mem_comp);
	gsh_free(engine->buf_iov, engine->params.mem_comp);

	if (engine->buf_area != NULL)
		(void)munmap(engine->buf_area, engine->buf_area_size);

	engine->classes = NULL;
	engine->bufs = NULL;
	engine->buf_iov = NULL;
	engine->buf_area = NULL;
	engine->nr_bufs = 0;
	engine->nr_classes = 0;
	engine->buf_max = 0;
}

/** Number of power-of-two classes spanning [buf_min, buf_max]. */
static unsigned int uring_count_classes(size_t buf_min, size_t buf_max)
{
	unsigned int nr = 0;
	size_t size;

	for (size = buf_min; size <= buf_max; size *= 2) {
		nr++;

		if (nr == URING_MAX_CLASSES)
			break;
	}

	return nr;
}

/**
 * @brief Work out the size classes and how many buffers each one gets
 *
 * Classes are powers of two from @a buf_min to @a buf_max.  The pool budget
 * is distributed geometrically: each class from smallest to largest gets
 * twice the byte allocation of the class below it (ratio = 2).  This gives
 * every class the same number of concurrent buffer slots while spending bytes
 * proportionally to buffer size — the largest class gets half the total
 * budget, the second-largest a quarter, and so on.
 *
 * Why equal slot counts?  A class is exhausted when all its buffers are
 * in-flight simultaneously.  The cost of a miss is proportional to the
 * buffer size: falling back to a 1 MiB heap buffer is expensive; falling
 * back to a 4 KiB one is cheap.  Equal slots means each class can absorb
 * the same concurrency before spilling to the heap, and the bytes spent per
 * slot scale with the buffer size as they should.
 *
 * Closed form: weight[i] = 2^i (i=0 is smallest), sum = 2^nc - 1.
 * bytes_for_class[i] = pool_bytes * 2^i / (2^nc - 1)
 * nr[i]             = bytes_for_class[i] / class_size[i]
 *                   = pool_bytes / (buf_min * (2^nc - 1))   -- constant!
 *
 * So nr_per_class = pool_bytes / (buf_min * (2^nr_classes - 1)), and
 * total nr_bufs   = nr_classes * nr_per_class.
 *
 * For any config within the supported range (pool_mb <= 65536, buf_min >=
 * 4096, up to 9 classes from 4 KiB to 1 MiB) this stays well below the
 * kernel's URING_MAX_FIXED_BUFS = 16384 limit up to ~3600 MiB.  The guard
 * at the end of this function is a safety net for extreme configs only.
 *
 * @a pool_bytes is a hard ceiling; the actual pinned size equals
 * nr_classes * nr_per_class * buf_min * (2^nc - 1) / (2^nc - 1) = as
 * computed, always <= pool_bytes (integer truncation loses at most
 * nr_classes * buf_max bytes, which is negligible).
 *
 * @return Number of classes filled in, always at least 1
 */
static unsigned int uring_classes_plan(struct uring_engine *engine,
				       size_t buf_min, size_t buf_max,
				       size_t pool_bytes)
{
	unsigned int nr_classes;
	size_t nr_per_class;
	uint64_t weight_sum;
	size_t size;
	unsigned int i;

	nr_classes = uring_count_classes(buf_min, buf_max);

	/*
	 * weight_sum = 2^nr_classes - 1  (sum of geometric series 2^0 + 2^1
	 * + ... + 2^(nc-1)).  nr_per_class is the slot count every class
	 * receives; it equals pool_bytes / (buf_min * weight_sum) because
	 * class[i] has size buf_min * 2^i and weight 2^i, so bytes_i / size_i
	 * = pool_bytes * 2^i / (weight_sum * buf_min * 2^i) = constant.
	 */
	weight_sum = ((uint64_t)1 << nr_classes) - 1;
	nr_per_class = (size_t)((uint64_t)pool_bytes /
				((uint64_t)buf_min * weight_sum));
	/* Round down to the nearest power of two so every class count is a
	 * clean 2^n (e.g. 512, 256, 128 …).  This wastes at most ~50% of the
	 * last power-of-two step, which is acceptable for a pinned buffer pool.
	 * Floor of 2 ensures no class is eliminated on tiny pool configs.
	 */
	nr_per_class = uring_rounddown_pow2(nr_per_class);
	if (nr_per_class < 2)
		nr_per_class = 2;

	engine->classes = gsh_calloc(nr_classes, sizeof(*engine->classes),
				     engine->params.mem_comp);

	size = buf_min;

	for (i = 0; i < nr_classes; i++) {
		struct uring_class *cls = &engine->classes[i];

		cls->size = size;
		cls->nr = nr_per_class; /* same count for every class */
		cls->base = engine->nr_bufs;

		engine->nr_bufs += cls->nr;
		engine->buf_area_size += (size_t)cls->nr * size;
		engine->buf_max = size;

		size *= 2;
	}

	/*
	 * Safety net: the geometric planner stays under URING_MAX_FIXED_BUFS
	 * for pool_mb up to ~3600 MiB with buf_min=4096.  For pathological
	 * configs beyond that, scale every class's count down uniformly with
	 * a floor of 2 so registration does not fail with EINVAL.
	 */
	if (engine->nr_bufs > URING_MAX_FIXED_BUFS) {
		unsigned int old_nr_bufs = engine->nr_bufs;

		engine->nr_bufs = 0;
		engine->buf_area_size = 0;

		for (i = 0; i < nr_classes; i++) {
			struct uring_class *cls = &engine->classes[i];
			unsigned int scaled;

			scaled = (unsigned int)((uint64_t)cls->nr *
						URING_MAX_FIXED_BUFS /
						old_nr_bufs);
			if (scaled < 2)
				scaled = 2;

			cls->nr = scaled;
			cls->base = engine->nr_bufs;
			engine->nr_bufs += cls->nr;
			engine->buf_area_size += (size_t)cls->nr * cls->size;
		}

		URING_LOG(engine, URING_LOG_EVENT,
			  "%s: buffer count scaled from %u to %u to"
			  " stay within the kernel's %u-iovec"
			  " registration limit; effective pool"
			  " %zu MiB (configured %zu MiB) —"
			  " all %u size classes from %zu to %zu"
			  " bytes preserved",
			  engine->name, old_nr_bufs, engine->nr_bufs,
			  URING_MAX_FIXED_BUFS,
			  engine->buf_area_size / (1024 * 1024),
			  pool_bytes / (1024 * 1024), nr_classes, buf_min,
			  engine->buf_max);
	}

	if (engine->buf_area_size > pool_bytes) {
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: pinning %zu bytes, over the %zu byte"
			  " budget, because one %zu byte buffer is"
			  " the smallest pool possible",
			  engine->name, engine->buf_area_size, pool_bytes,
			  buf_min);
	}

	return nr_classes;
}

/**
 * @brief Allocate and register the size-classed fixed buffer pool
 *
 * A failure here is not fatal: the engine simply runs without fixed
 * buffers and uses plain opcodes. That is the common case
 * when RLIMIT_MEMLOCK is small, which is exactly when we must not insist.
 */
static void uring_pool_create(struct uring_engine *engine, size_t buf_min,
			      size_t buf_max, size_t pool_bytes)
{
	unsigned int i;
	unsigned int j;
	char *addr;
	int rc;

	engine->nr_classes =
		uring_classes_plan(engine, buf_min, buf_max, pool_bytes);

	engine->buf_area = mmap(NULL, engine->buf_area_size,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (engine->buf_area == MAP_FAILED) {
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: could not map %zu bytes of io_uring buffers: %s",
			  engine->name, engine->buf_area_size, strerror(errno));
		engine->buf_area = NULL;
		gsh_free(engine->classes, engine->params.mem_comp);
		engine->classes = NULL;
		engine->nr_classes = 0;
		engine->nr_bufs = 0;
		engine->buf_area_size = 0;
		engine->buf_max = 0;
		return;
	}

	engine->buf_iov = gsh_calloc(engine->nr_bufs, sizeof(*engine->buf_iov),
				     engine->params.mem_comp);
	engine->bufs = gsh_calloc(engine->nr_bufs, sizeof(*engine->bufs),
				  engine->params.mem_comp);

	/* Classes are laid out back to back in the one mapping, so a class is
	 * just a run of registered buffer indices and registration stays a
	 * single flat array.
	 */
	addr = engine->buf_area;

	for (i = 0; i < engine->nr_classes; i++) {
		struct uring_class *cls = &engine->classes[i];

		PTHREAD_MUTEX_init(&cls->lock, NULL);
		cls->free_stack = gsh_calloc(cls->nr, sizeof(*cls->free_stack),
					     engine->params.mem_comp);

		for (j = 0; j < cls->nr; j++) {
			unsigned int idx = cls->base + j;

			engine->buf_iov[idx].iov_base = addr;
			engine->buf_iov[idx].iov_len = cls->size;
			engine->bufs[idx].engine = engine;
			engine->bufs[idx].addr = addr;
			engine->bufs[idx].len = cls->size;
			engine->bufs[idx].index = idx;
			engine->bufs[idx].cls = i;
			cls->free_stack[j] = idx;

			addr += cls->size;
		}

		cls->free_top = cls->nr;

		URING_LOG(engine, URING_LOG_EVENT,
			  "%s:   class %zu bytes x %u buffers = %zu MiB",
			  engine->name, cls->size, cls->nr,
			  ((size_t)cls->nr * cls->size) / (1024 * 1024));
	}

	/* Every ring registers the same array, so a buffer index is valid on
	 * any of them.
	 */
	for (i = 0; i < engine->nr_rings; i++) {
		rc = io_uring_register_buffers(&engine->rings[i].ring,
					       engine->buf_iov,
					       engine->nr_bufs);
		if (rc < 0) {
			if (rc == -EINVAL)
				URING_LOG(engine, URING_LOG_WARN,
					  "%s: io_uring_register_buffers"
					  " failed: EINVAL - kernel rejected"
					  " %u iovecs (limit is %u); this"
					  " should not happen after the"
					  " count-scaling pass",
					  engine->name, engine->nr_bufs,
					  URING_MAX_FIXED_BUFS);
			else if (rc == -ENOMEM)
				URING_LOG(engine, URING_LOG_WARN,
					  "%s: io_uring_register_buffers"
					  " failed: ENOMEM - not enough"
					  " locked memory to pin %zu MiB;"
					  " raise RLIMIT_MEMLOCK or reduce"
					  " io_uring_pool_mb",
					  engine->name,
					  engine->buf_area_size /
						  (1024 * 1024));
			else
				URING_LOG(engine, URING_LOG_WARN,
					  "%s: io_uring_register_buffers"
					  " failed: %s - falling back to"
					  " non-fixed I/O",
					  engine->name, strerror(-rc));

			while (i-- > 0) {
				(void)io_uring_unregister_buffers(
					&engine->rings[i].ring);
			}

			uring_pool_destroy(engine);
			return;
		}
	}

	engine->bufs_registered = true;

	URING_LOG(engine, URING_LOG_EVENT,
		  "%s: registered %u fixed buffers across %u size"
		  " classes (%zu to %zu bytes), %zu MiB pinned",
		  engine->name, engine->nr_bufs, engine->nr_classes,
		  engine->classes[0].size, engine->buf_max,
		  engine->buf_area_size / (1024 * 1024));
}
/* --- Completion --- */

/**
 * @brief One CQE, copied out of the ring.
 *
 * The callbacks must not run while cq_lock is held, and the CQE slot is
 * reusable by the kernel the moment io_uring_cqe_seen() runs, so what the
 * callbacks need is copied out first.
 */
struct uring_cqe_copy {
	uint64_t user_data;
	int32_t res;
	uint32_t flags;
};

/** CQEs copied per locked harvest round. */
#define URING_HARVEST_MAX 64

static void uring_handle_cqe(struct uring_ring *ring, uint64_t user_data,
			     int32_t cqe_res, uint32_t cqe_flags)
{
	struct uring_engine *engine = ring->engine;
	struct uring_req *req;

	if (user_data == 0) {
		/* Wake-up NOP posted by uring_engine_destroy(). */
		return;
	}

	atomic_dec_uint32_t(&ring->outstanding_io);

	if ((user_data & URING_TAG_CHAIN) != 0) {
		/* One SQE of a linked chain. Record this slot's result and,
		 * once every slot has reported, hand the whole chain to the
		 * caller. A linked SQE that was cancelled because an earlier
		 * one failed still posts a CQE (-ECANCELED), so the count
		 * always reaches nr_ops.
		 */
		struct uring_chain_slot *slot =
			(struct uring_chain_slot
				 *)(uintptr_t)(user_data &
					       ~URING_TAG_CHAIN_MASK);
		struct uring_chain_req *chain = slot->chain;

		chain->cqes[slot->idx].res = cqe_res;
		chain->cqes[slot->idx].flags = cqe_flags;

		if (cqe_res < 0)
			(void)atomic_inc_uint64_t(&engine->stat_failed);
		else
			(void)atomic_inc_uint64_t(&engine->stat_completed);

		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "%s: chain slot %u/%u res=%d", engine->name,
			  slot->idx, chain->nr_ops, cqe_res);

		if (__atomic_add_fetch(&chain->nr_done, 1, __ATOMIC_ACQ_REL) ==
		    chain->nr_ops)
			chain->done_cb(chain->cb_arg, chain);

		return;
	}

	req = (struct uring_req *)(uintptr_t)(user_data & ~URING_TAG_MASK);

	if ((user_data & URING_TAG_FSYNC) != 0)
		req->res2 = cqe_res;
	else
		req->res = cqe_res;

	if (__atomic_sub_fetch(&req->pending, 1, __ATOMIC_ACQ_REL) != 0)
		return;

	if (req->res < 0)
		(void)atomic_inc_uint64_t(&engine->stat_failed);
	else
		(void)atomic_inc_uint64_t(&engine->stat_completed);

	URING_LOG(engine, URING_LOG_FULL_DEBUG, "%s: completion res=%d res2=%d",
		  engine->name, req->res, req->res2);

	req->cb(req->cb_arg, req->res, req->res2);

	gsh_free(req, engine->params.mem_comp);
}

/**
 * @brief Copy up to @a max ready CQEs out of the ring under cq_lock.
 */
static unsigned int uring_harvest_locked(struct uring_ring *ring,
					 struct uring_cqe_copy *out,
					 unsigned int max)
{
	struct io_uring_cqe *cqe;
	unsigned int n = 0;

	PTHREAD_MUTEX_lock(&ring->cq_lock);

	while (n < max && io_uring_peek_cqe(&ring->ring, &cqe) == 0) {
		out[n].user_data = cqe->user_data;
		out[n].res = cqe->res;
		out[n].flags = cqe->flags;
		n++;
		io_uring_cqe_seen(&ring->ring, cqe);
	}

	PTHREAD_MUTEX_unlock(&ring->cq_lock);

	return n;
}

/**
 * @brief One harvest round: take what is ready, then run the callbacks.
 *
 * Bounded by URING_HARVEST_MAX so that a worker polling for its own
 * completion cannot be conscripted into draining an unbounded backlog.
 */
static unsigned int uring_drain_once(struct uring_ring *ring)
{
	struct uring_cqe_copy harvest[URING_HARVEST_MAX];
	unsigned int n;
	unsigned int i;

	n = uring_harvest_locked(ring, harvest, URING_HARVEST_MAX);

	for (i = 0; i < n; i++)
		uring_handle_cqe(ring, harvest[i].user_data, harvest[i].res,
				 harvest[i].flags);

	return n;
}

/** Drain the ring completely. For the reaper, which has nothing else to do. */
static unsigned int uring_drain(struct uring_ring *ring)
{
	unsigned int total = 0;
	unsigned int n;

	do {
		n = uring_drain_once(ring);
		total += n;
	} while (n == URING_HARVEST_MAX);

	return total;
}

/**
 * @brief Completion reaper
 *
 * One per ring. This thread has no module-specific thread state, so
 * callbacks that need one have to build it themselves.
 */
static void *uring_reaper(void *arg)
{
	struct uring_ring *ring = arg;
	struct uring_engine *engine = ring->engine;
	struct io_uring_cqe *cqe;
	char tname[16];
	int rc;

	(void)snprintf(tname, sizeof(tname), "%.9s_urng%u", engine->name,
		       ring->idx);
	SetNameFunction(tname);
#ifdef __linux__
	(void)pthread_setname_np(pthread_self(), tname);
#endif

	URING_LOG(engine, URING_LOG_DEBUG, "%s: reaper %u started",
		  engine->name, ring->idx);

	while (!__atomic_load_n(&engine->stopping, __ATOMIC_ACQUIRE)) {
		rc = io_uring_wait_cqe(&ring->ring, &cqe);

		if (rc == -EINTR)
			continue;

		if (rc < 0) {
			URING_LOG(engine, URING_LOG_CRIT,
				  "%s: io_uring_wait_cqe failed: %s - reaper %u exiting",
				  engine->name, strerror(-rc), ring->idx);
			__atomic_store_n(&engine->broken, true,
					 __ATOMIC_RELEASE);
			break;
		}

		/*
		 * `cqe` is deliberately ignored. io_uring_wait_cqe() does
		 * not advance the CQ head, it only blocks until something
		 * is there -- and a worker polling for its own completion
		 * may well have consumed it by now. Re-reading the ring
		 * under cq_lock below is the only safe way to find out, and
		 * finding nothing is a wasted wakeup, not an error.
		 *
		 * The hooks bracket the whole harvest so the consumer can
		 * batch whatever the callbacks hand onward, making a pass of
		 * N completions cost one dispatch rather than N.
		 */
		if (engine->params.reap_begin != NULL)
			engine->params.reap_begin();

		(void)uring_drain(ring);

		if (engine->params.reap_end != NULL)
			engine->params.reap_end();
	}

	/* Nothing should be in flight at shutdown, but do not silently drop
	 * a completion if something is.
	 */
	if (uring_drain(ring) != 0) {
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: reaper %u drained late completions at shutdown",
			  engine->name, ring->idx);
	}

	URING_LOG(engine, URING_LOG_DEBUG, "%s: reaper %u stopped",
		  engine->name, ring->idx);

	return NULL;
}
/* --- Per-ring adaptive coalescing helpers --- */


/**
 * @brief Atomically set *var to 0 if its current value equals expected.
 * Returns true if the CAS succeeded (we zeroed it), false if it changed.
 */
static inline bool uring_cas_zero_uint32(uint32_t *var, uint32_t expected)
{
	return __atomic_compare_exchange_n(var, &expected, 0u, false,
					   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

struct uring_coalesce_decision {
	bool submit;
	const char *reason;
};

static inline uint64_t uring_coalesce_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000ULL) +
	       ((uint64_t)ts.tv_nsec / 1000ULL);
}

static inline uint32_t
uring_coalesce_calc_bucket(uint32_t outstanding_io,
			   const struct uring_engine *engine)
{
	uint32_t bucket;
	uint32_t cif = engine->coalesce_cif_threshold;

	if (outstanding_io < cif)
		return 1;

	if (outstanding_io < 2 * cif)
		bucket = (engine->coalesce_bucket_io_count * 4) / 5;
	else if (outstanding_io < 3 * cif)
		bucket = (engine->coalesce_bucket_io_count * 3) / 4;
	else if (outstanding_io < 4 * cif)
		bucket = (engine->coalesce_bucket_io_count * 2) / 3;
	else
		bucket = engine->coalesce_bucket_io_count +
			 (outstanding_io / (2 * cif));

	if (bucket < 1)
		bucket = 1;
	if (bucket > engine->coalesce_max_bucket_io_count)
		bucket = engine->coalesce_max_bucket_io_count;

	return bucket;
}

static inline struct uring_coalesce_decision
uring_coalesce_recalc(uint32_t io_count,
		      uint32_t outstanding_io,
		      uint64_t first_io_time_us,
		      uint64_t now_us,
		      const struct uring_engine *engine)
{
	struct uring_coalesce_decision decision;
	uint64_t elapsed_us;
	uint32_t bucket;

	decision.submit = false;
	decision.reason = "accumulating";

	if (io_count == 0)
		return decision;

	/*
	 * Belt and braces: uring_engine_submit() already routes around this
	 * function entirely when coalescing is off, so this only matters if
	 * a future caller reaches it directly. Never hold an SQE back when
	 * the feature is disabled.
	 */
	if (!engine->coalesce_enable) {
		decision.submit = true;
		decision.reason = "coalesce-disabled";
		return decision;
	}

	/* Low outstanding — submit aggressively */
	if (outstanding_io < engine->coalesce_cif_threshold) {
		decision.submit = true;
		decision.reason = "low-outstanding";
		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "coalesce: submit=%s reason=%s io_count=%u outstanding=%u bucket=1",
			  "yes", decision.reason, io_count, outstanding_io);
		return decision;
	}

	/* Time-bucket expiry */
	if (first_io_time_us != 0) {
		elapsed_us = now_us - first_io_time_us;
		if (elapsed_us >= engine->coalesce_timeout_us) {
			decision.submit = true;
			decision.reason = "time-bucket-expired";
			URING_LOG(engine, URING_LOG_FULL_DEBUG,
				  "coalesce: submit=%s reason=%s"
				  " io_count=%u outstanding=%u"
				  " elapsed_us=%" PRIu64,
				  "yes", decision.reason, io_count,
				  outstanding_io, elapsed_us);
			return decision;
		}
	}

	/* IO-count bucket full */
	bucket = uring_coalesce_calc_bucket(outstanding_io, engine);
	if (io_count >= bucket) {
		decision.submit = true;
		decision.reason = "io-count-bucket-full";
		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "coalesce: submit=%s reason=%s io_count=%u outstanding=%u bucket=%u",
			  "yes", decision.reason, io_count, outstanding_io,
			  bucket);
		return decision;
	}

	/* Back-pressure: outstanding at limit — hold */
	if (outstanding_io >= engine->coalesce_outstanding_max) {
		decision.reason = "outstanding-io-limit";
		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "coalesce: submit=%s reason=%s io_count=%u outstanding=%u bucket=%u",
			  "no", decision.reason, io_count, outstanding_io,
			  bucket);
		return decision;
	}

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "coalesce: submit=%s reason=%s io_count=%u outstanding=%u bucket=%u",
		  decision.submit ? "yes" : "no", decision.reason,
		  io_count, outstanding_io, bucket);

	return decision;
}

/* --- Submission --- */

static struct uring_ring *uring_pick_ring(struct uring_engine *engine)
{
	uint32_t n;
	int cpu;

	if (engine->nr_rings == 1)
		return &engine->rings[0];

	if (engine->per_cpu_rings) {
		cpu = sched_getcpu();
		if (cpu >= 0)
			return &engine->rings[(unsigned int)cpu %
					      engine->nr_rings];
	}

	n = atomic_inc_uint32_t(&engine->next_ring);

	return &engine->rings[n % engine->nr_rings];
}

int uring_engine_submit(struct uring_engine *engine, const struct uring_op *op)
{
	struct uring_ring *ring;
	struct uring_req *req;
	struct io_uring_sqe *sqe;
	unsigned int nr_sqe;
	uint32_t my_count;
	uint32_t cur;
	uint32_t outstanding;
	uint32_t n;
	uint64_t now;
	uint64_t first;
	uint64_t deadline;
	struct uring_coalesce_decision decision;
	int tries;
	int rc;

	if (__atomic_load_n(&engine->broken, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&engine->stopping, __ATOMIC_ACQUIRE))
		return -ESHUTDOWN;

	assert(op->sqe_pre != NULL);

	nr_sqe = op->sqe_pre2 != NULL ? 2 : 1;

	req = gsh_calloc(1, sizeof(*req), engine->params.mem_comp);
	req->cb = op->cb;
	req->cb_arg = op->cb_arg;
	req->pending = nr_sqe;

	ring = uring_pick_ring(engine);

	/*
	 * Direct submission, used in two cases:
	 *
	 *  - coalescing is switched off (the default), so there is nothing
	 *    to accumulate and no reason to run the decide/spin phases;
	 *  - SQPOLL, where the kernel polls the SQ itself so there is no
	 *    submit syscall to amortise and batching only adds latency.
	 */
	if (engine->sqpoll || !engine->coalesce_enable)
		goto direct_submit;

	/* Phase 1: SQE placement — brief submit_lock */
	PTHREAD_MUTEX_lock(&ring->submit_lock);

	if (io_uring_sq_space_left(&ring->ring) < nr_sqe) {
		/*
		 * SQ full — flush whatever is already queued before claiming
		 * space, and update counters so spinners see a consistent
		 * state.
		 */
		n = atomic_fetch_uint32_t(&ring->io_count);
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: ring %u SQ full (space=%u needed=%u)"
			  " — forcing mid-batch flush of %u SQE(s);"
			  " consider increasing queue_depth",
			  engine->name, ring->idx,
			  io_uring_sq_space_left(&ring->ring), nr_sqe, n);
		if (n > 0) {
			for (tries = 0;; tries++) {
				rc = io_uring_submit(&ring->ring);
				if (rc >= 0)
					break;
				if ((rc == -EINTR || rc == -EAGAIN ||
				     rc == -EBUSY) &&
				    tries < URING_SUBMIT_RETRIES) {
					sched_yield();
					continue;
				}
				break;
			}
			atomic_add_uint32_t(&ring->outstanding_io, n);
			atomic_store_uint32_t(&ring->io_count, 0);
			atomic_store_uint64_t(&ring->first_io_time_us, 0);
		}

		if (io_uring_sq_space_left(&ring->ring) < nr_sqe) {
			PTHREAD_MUTEX_unlock(&ring->submit_lock);
			gsh_free(req, engine->params.mem_comp);
			(void)atomic_inc_uint64_t(&engine->stat_no_sqe);
			return -EAGAIN;
		}
	}

	sqe = io_uring_get_sqe(&ring->ring);
	op->sqe_pre(sqe, op->sqe_pre_arg, ring->idx);
	io_uring_sqe_set_data(sqe, req);

	if (engine->force_async) {
		/* Skip the kernel's inline attempt and go straight to an
		 * io-wq worker. Costs a thread handoff on every I/O, page
		 * cache hits included, so this is off unless asked for.
		 */
		sqe->flags |= IOSQE_ASYNC;
	}

	if (op->sqe_pre2 != NULL) {
		/* IOSQE_IO_LINK orders the linked SQE after the primary and
		 * cancels it with -ECANCELED if the primary SQE failed.
		 */
		sqe->flags |= IOSQE_IO_LINK;

		sqe = io_uring_get_sqe(&ring->ring);
		op->sqe_pre2(sqe, op->sqe_pre2_arg, ring->idx);
		io_uring_sqe_set_data(sqe, (void *)((uintptr_t)req |
						    URING_TAG_FSYNC));
	}

	/* Record the batch start time for the very first SQE in this batch. */
	if (atomic_fetch_uint32_t(&ring->io_count) == 0)
		atomic_store_uint64_t(&ring->first_io_time_us,
				      uring_coalesce_now_us());

	/* my_count = total SQEs pending in ring after my addition. */
	my_count = atomic_add_uint32_t(&ring->io_count, nr_sqe);

	PTHREAD_MUTEX_unlock(&ring->submit_lock);

	/* Phase 2: Decision — lock-free */
	outstanding = atomic_fetch_uint32_t(&ring->outstanding_io);
	now = uring_coalesce_now_us();
	first = atomic_fetch_uint64_t(&ring->first_io_time_us);
	deadline = first + engine->coalesce_timeout_us;
	decision = uring_coalesce_recalc(my_count, outstanding, first, now,
					 engine);

	if (decision.submit) {
		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "%s: ring %u coalesce decision_flush my_count=%u reason=%s",
			  engine->name, ring->idx, my_count, decision.reason);
		goto try_flush;
	}

	/* Phase 3: Spin — lock-free */
	while (true) {
		cur = atomic_fetch_uint32_t(&ring->io_count);
		if (cur != my_count) {
			URING_LOG(engine, URING_LOG_FULL_DEBUG,
				  "%s: ring %u coalesce %s my_count=%u cur=%u",
				  engine->name, ring->idx,
				  cur == 0 ? "already_flushed" : "new_arrival",
				  my_count, cur);
			return 0;
		}
		now = uring_coalesce_now_us(); /* refresh each iter */
		if (now >= deadline) {
			URING_LOG(engine, URING_LOG_FULL_DEBUG,
				  "%s: ring %u coalesce timeout_flush my_count=%u",
				  engine->name, ring->idx, my_count);
			goto try_flush;
		}
		if (__atomic_load_n(&engine->stopping, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&engine->broken, __ATOMIC_ACQUIRE)) {
			URING_LOG(engine, URING_LOG_FULL_DEBUG,
				  "%s: ring %u coalesce stopping my_count=%u",
				  engine->name, ring->idx, my_count);
			return -ESHUTDOWN;
		}
		sched_yield();
	}

	/* Phase 4: Claim flush role — CAS + submit */
try_flush:
	if (!uring_cas_zero_uint32(&ring->io_count, my_count)) {
		/* Lost CAS: a new arrival bumped io_count, or another thread
		 * already zeroed it. Either way, someone else will flush.
		 */
		URING_LOG(engine, URING_LOG_FULL_DEBUG,
			  "%s: ring %u coalesce lost_cas my_count=%u",
			  engine->name, ring->idx, my_count);
		return 0;
	}

	/*
	 * Won CAS: io_count is now 0. Re-acquire lock for io_uring_submit()
	 * (liburing sqe_tail constraint). Clear first_io_time_us inside the
	 * lock to prevent a new-arrival race on the timestamp.
	 */
	PTHREAD_MUTEX_lock(&ring->submit_lock);

	for (tries = 0;; tries++) {
		rc = io_uring_submit(&ring->ring);

		if (rc >= 0)
			break;

		if ((rc == -EINTR || rc == -EAGAIN || rc == -EBUSY) &&
		    tries < URING_SUBMIT_RETRIES) {
			sched_yield();
			continue;
		}

		break;
	}

	atomic_store_uint64_t(&ring->first_io_time_us, 0); /* inside lock */
	atomic_add_uint32_t(&ring->outstanding_io, my_count);

	PTHREAD_MUTEX_unlock(&ring->submit_lock);

	/* Best-effort peak stat — unsynchronised, benign race. */
	if (my_count > ring->stat_peak_batch)
		ring->stat_peak_batch = my_count;

	if (rc < 0) {
		/* liburing has already published these SQEs to the ring tail,
		 * so the kernel may still pick them up on someone else's
		 * submit and run the callback. Freeing req here would be a
		 * use-after-free, so it is deliberately leaked and the engine
		 * is fenced off instead.
		 */
		URING_LOG(engine, URING_LOG_CRIT,
			  "%s: io_uring_submit failed: %s - disabling engine",
			  engine->name, strerror(-rc));
		__atomic_store_n(&engine->broken, true, __ATOMIC_RELEASE);
		return rc;
	}

	(void)atomic_add_uint64_t(&engine->stat_submitted, my_count);

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "%s: submitted ring=%u batch=%u linked=%s", engine->name,
		  ring->idx, my_count, op->sqe_pre2 != NULL ? "yes" : "no");

	return 0;

	/*
	 * Direct path: place the SQE and submit it in the same hold of
	 * submit_lock. No io_count accounting, no CAS, no spin — this is
	 * both the SQPOLL path and the (default) coalescing-disabled path.
	 */
direct_submit:
	PTHREAD_MUTEX_lock(&ring->submit_lock);

	if (io_uring_sq_space_left(&ring->ring) < nr_sqe) {
		/* Push out whatever is already queued and look again. */
		(void)io_uring_submit(&ring->ring);

		if (io_uring_sq_space_left(&ring->ring) < nr_sqe) {
			PTHREAD_MUTEX_unlock(&ring->submit_lock);
			gsh_free(req, engine->params.mem_comp);
			(void)atomic_inc_uint64_t(&engine->stat_no_sqe);
			return -EAGAIN;
		}
	}

	sqe = io_uring_get_sqe(&ring->ring);
	op->sqe_pre(sqe, op->sqe_pre_arg, ring->idx);
	io_uring_sqe_set_data(sqe, req);

	if (engine->force_async)
		sqe->flags |= IOSQE_ASYNC;

	if (op->sqe_pre2 != NULL) {
		sqe->flags |= IOSQE_IO_LINK;

		sqe = io_uring_get_sqe(&ring->ring);
		op->sqe_pre2(sqe, op->sqe_pre2_arg, ring->idx);
		io_uring_sqe_set_data(sqe, (void *)((uintptr_t)req |
						    URING_TAG_FSYNC));
	}

	for (tries = 0;; tries++) {
		rc = io_uring_submit(&ring->ring);

		if (rc >= 0)
			break;

		if ((rc == -EINTR || rc == -EAGAIN || rc == -EBUSY) &&
		    tries < URING_SUBMIT_RETRIES) {
			sched_yield();
			continue;
		}

		break;
	}

	/*
	 * Count these the same way the coalescing path does. The reaper
	 * decrements outstanding_io for every CQE it sees, so a submit path
	 * that does not increment it drives the counter to underflow — and
	 * since this is now the default path, that would leave the
	 * coalescing heuristics reading a garbage ~4 billion the moment
	 * anyone turned them on.
	 */
	atomic_add_uint32_t(&ring->outstanding_io, nr_sqe);

	PTHREAD_MUTEX_unlock(&ring->submit_lock);

	if (rc < 0) {
		URING_LOG(engine, URING_LOG_CRIT,
			  "%s: io_uring_submit failed: %s - disabling engine",
			  engine->name, strerror(-rc));
		__atomic_store_n(&engine->broken, true, __ATOMIC_RELEASE);
		return rc;
	}

	(void)atomic_add_uint64_t(&engine->stat_submitted, nr_sqe);

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "%s: submitted direct ring=%u nr_sqe=%u linked=%s",
		  engine->name, ring->idx, nr_sqe,
		  op->sqe_pre2 != NULL ? "yes" : "no");

	return 0;
}

/* --- Linked chain submission --- */

int uring_engine_submit_chain(struct uring_engine *engine,
			      struct uring_chain_req *chain)
{
	struct uring_ring *ring;
	struct io_uring_sqe *sqe;
	uint32_t pending;
	uint32_t flushed = 0;
	unsigned int i;
	int tries;
	int rc;

	if (engine == NULL || chain == NULL || chain->nr_ops == 0)
		return -EINVAL;

	if (chain->nr_ops > URING_CHAIN_MAX_OPS)
		return -E2BIG;

	if (__atomic_load_n(&engine->broken, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&engine->stopping, __ATOMIC_ACQUIRE))
		return -ESHUTDOWN;

	chain->nr_done = 0;

	ring = uring_pick_ring(engine);
	chain->ring_idx = ring->idx;

	/*
	 * Everything below happens under one hold of submit_lock, including
	 * io_uring_submit(). Unlike uring_engine_submit(), the lock is not
	 * dropped for a coalescing decision: IOSQE_IO_LINK binds an SQE to
	 * whichever SQE physically follows it, so letting another thread
	 * place an SQE in the middle of the run would splice an unrelated
	 * request into this chain.
	 */
	PTHREAD_MUTEX_lock(&ring->submit_lock);

	if (io_uring_sq_space_left(&ring->ring) < chain->nr_ops) {
		/* Push out what is already queued, then look again — the
		 * chain needs all of its SQEs in one contiguous run.
		 */
		pending = atomic_fetch_uint32_t(&ring->io_count);

		if (pending > 0) {
			for (tries = 0;; tries++) {
				rc = io_uring_submit(&ring->ring);
				if (rc >= 0)
					break;
				if ((rc == -EINTR || rc == -EAGAIN ||
				     rc == -EBUSY) &&
				    tries < URING_SUBMIT_RETRIES) {
					sched_yield();
					continue;
				}
				break;
			}
			atomic_add_uint32_t(&ring->outstanding_io, pending);
			atomic_store_uint32_t(&ring->io_count, 0);
			atomic_store_uint64_t(&ring->first_io_time_us, 0);
			/* Their flush thread's CAS will now fail, so it will
			 * not count these; record them here instead.
			 */
			(void)atomic_add_uint64_t(&engine->stat_submitted,
						  pending);
			flushed += pending;
		}

		if (io_uring_sq_space_left(&ring->ring) < chain->nr_ops) {
			PTHREAD_MUTEX_unlock(&ring->submit_lock);
			(void)atomic_inc_uint64_t(&engine->stat_no_sqe);
			URING_LOG(engine, URING_LOG_WARN,
				  "%s: ring %u cannot fit a %u SQE chain"
				  " (space=%u); consider raising"
				  " queue_depth",
				  engine->name, ring->idx, chain->nr_ops,
				  io_uring_sq_space_left(&ring->ring));
			return -EAGAIN;
		}
	}

	for (i = 0; i < chain->nr_ops; i++) {
		sqe = io_uring_get_sqe(&ring->ring);

		/* Space for the whole chain was verified above under this
		 * same lock, so the ring cannot run dry mid-chain.
		 */
		assert(sqe != NULL);

		chain->ops[i].sqe_pre(sqe, chain->ops[i].sqe_pre_arg,
				      ring->idx);

		sqe->flags |= chain->ops[i].sqe_flags;

		if (engine->force_async)
			sqe->flags |= IOSQE_ASYNC;

		/* Link each SQE to its successor. The final SQE must be left
		 * unlinked, otherwise the kernel would hold the chain open
		 * waiting on an SQE that is not part of it.
		 */
		if (i + 1 < chain->nr_ops)
			sqe->flags |= IOSQE_IO_LINK;

		chain->slots[i].chain = chain;
		chain->slots[i].idx = i;
		sqe->user_data = (uint64_t)(uintptr_t)&chain->slots[i] |
				 URING_TAG_CHAIN;
	}

	/*
	 * SQEs other threads parked for coalescing are flushed by the same
	 * submit, so they move to outstanding_io here. Zeroing io_count makes
	 * their compare-and-swap in the coalescing path fail, so they return
	 * without counting the same SQEs again.
	 */
	pending = atomic_fetch_uint32_t(&ring->io_count);

	for (tries = 0;; tries++) {
		rc = io_uring_submit(&ring->ring);

		if (rc >= 0)
			break;

		if ((rc == -EINTR || rc == -EAGAIN || rc == -EBUSY) &&
		    tries < URING_SUBMIT_RETRIES) {
			sched_yield();
			continue;
		}

		break;
	}

	if (pending > 0) {
		atomic_store_uint32_t(&ring->io_count, 0);
		atomic_store_uint64_t(&ring->first_io_time_us, 0);
	}

	atomic_add_uint32_t(&ring->outstanding_io, pending + chain->nr_ops);

	PTHREAD_MUTEX_unlock(&ring->submit_lock);

	if (rc < 0) {
		/* liburing has already published these SQEs to the ring tail,
		 * so the kernel may still run them and fire done_cb. The
		 * chain therefore cannot be reclaimed by the caller; fence
		 * the engine off rather than risk a use-after-free.
		 */
		URING_LOG(engine, URING_LOG_CRIT,
			  "%s: io_uring_submit failed for chain: %s - disabling engine",
			  engine->name, strerror(-rc));
		__atomic_store_n(&engine->broken, true, __ATOMIC_RELEASE);
		return rc;
	}

	flushed += pending;

	(void)atomic_add_uint64_t(&engine->stat_submitted,
				  chain->nr_ops + pending);

	URING_LOG(engine, URING_LOG_FULL_DEBUG,
		  "%s: submitted chain ring=%u nr_ops=%u (+%u coalesced)",
		  engine->name, ring->idx, chain->nr_ops, flushed);

	return 0;
}

/* --- Kernel opcode discovery --- */

bool uring_engine_op_supported(struct uring_engine *engine, unsigned int opcode)
{
	if (engine == NULL || !engine->op_mask_valid)
		return false;

	if (opcode >= URING_OP_MASK_BITS)
		return false;

	return (engine->op_mask[opcode / 64] & (1ULL << (opcode % 64))) != 0;
}

/**
 * @brief Cache which IORING_OP_* values the running kernel implements.
 *
 * Called once from uring_engine_create() after ring 0 is up. All rings are
 * created with the same flags against the same kernel, so one probe covers
 * the engine.
 *
 * If the kernel is too old to support IORING_REGISTER_PROBE the mask is left
 * invalid and uring_engine_op_supported() reports false for everything,
 * which makes callers fall back to their synchronous paths rather than
 * submit opcodes that may not exist.
 */
static void uring_probe_opcodes(struct uring_engine *engine)
{
	struct io_uring_probe *probe;
	unsigned int op;
	unsigned int nr = 0;

	probe = io_uring_get_probe_ring(&engine->rings[0].ring);

	if (probe == NULL) {
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: kernel did not answer"
			  " IORING_REGISTER_PROBE; treating all"
			  " opcodes as unsupported",
			  engine->name);
		return;
	}

	for (op = 0; op < URING_OP_MASK_BITS; op++) {
		if (io_uring_opcode_supported(probe, op)) {
			engine->op_mask[op / 64] |= 1ULL << (op % 64);
			nr++;
		}
	}

	io_uring_free_probe(probe);
	engine->op_mask_valid = true;

	URING_LOG(engine, URING_LOG_INFO,
		  "%s: kernel implements %u io_uring opcode(s)", engine->name,
		  nr);
}

/* --- Engine lifecycle --- */

void uring_engine_get_stats(struct uring_engine *engine,
			    struct uring_engine_stats *out)
{
	if (engine == NULL || out == NULL)
		return;

	out->sqe_submitted = atomic_fetch_uint64_t(&engine->stat_submitted);
	out->cqe_completed = atomic_fetch_uint64_t(&engine->stat_completed);
	out->cqe_failed = atomic_fetch_uint64_t(&engine->stat_failed);
	out->sq_full_rejections = atomic_fetch_uint64_t(&engine->stat_no_sqe);
	out->buf_pool_hits = 0;
	out->buf_pool_misses = atomic_fetch_uint64_t(&engine->stat_no_buf);
	out->coalesce_batches = 0;
	out->coalesce_peak_batch = 0;

	for (unsigned int i = 0; i < engine->nr_classes; i++) {
		out->buf_pool_hits +=
			atomic_fetch_uint64_t(&engine->classes[i].stat_get);
	}

	for (unsigned int i = 0; i < engine->nr_rings; i++) {
		if (engine->rings[i].stat_peak_batch > out->coalesce_peak_batch)
			out->coalesce_peak_batch =
				engine->rings[i].stat_peak_batch;
	}
}

struct io_uring *uring_engine_raw_ring(struct uring_engine *engine,
				       unsigned int ring_idx)
{
	if (engine == NULL || ring_idx >= engine->nr_rings)
		return NULL;

	return &engine->rings[ring_idx].ring;
}

unsigned int uring_engine_poll(struct uring_engine *engine,
			       unsigned int ring_idx)
{
	if (engine == NULL || ring_idx >= engine->nr_rings)
		return 0;

	if (__atomic_load_n(&engine->broken, __ATOMIC_ACQUIRE))
		return 0;

	return uring_drain_once(&engine->rings[ring_idx]);
}

unsigned int uring_engine_nr_rings(struct uring_engine *engine)
{
	return engine != NULL ? engine->nr_rings : 0;
}

int uring_engine_create(const struct uring_engine_params *params,
			struct uring_engine **engine_out)
{
	struct uring_engine_params defaults = { 0 };
	struct uring_engine *engine;
	struct io_uring_params ring_params;
	unsigned int queue_depth;
	unsigned int nr_rings;
	size_t buf_min;
	size_t buf_max;
	size_t pool_bytes;
	unsigned int sqpoll_idle_ms;
	unsigned int sqpoll_groups;
	bool want_sqpoll;
	unsigned int i;
	int rc;

	if (params == NULL)
		params = &defaults;

	want_sqpoll = params->sqpoll;
	sqpoll_idle_ms = params->sqpoll_idle_ms != 0
				 ? params->sqpoll_idle_ms
				 : URING_DEF_SQPOLL_IDLE_MS;

	queue_depth = params->queue_depth != 0 ? params->queue_depth
					       : URING_DEF_QUEUE_DEPTH;

	if (params->per_cpu_rings) {
		long nproc = sysconf(_SC_NPROCESSORS_ONLN);

		if (nproc <= 0)
			nproc = 1;
		nr_rings = (unsigned int)nproc;
	} else {
		nr_rings = params->nr_rings != 0 ? params->nr_rings
						 : URING_DEF_RINGS;
	}

	if (nr_rings > URING_MAX_RINGS)
		nr_rings = URING_MAX_RINGS;

	/* A poller needs a ring of its own to be created against, so there
	 * can never be more of them than there are rings.
	 */
	sqpoll_groups = params->sqpoll_groups != 0 ? params->sqpoll_groups : 1;

	if (sqpoll_groups > nr_rings)
		sqpoll_groups = nr_rings;

	/* buf_min == 0 means "no pool at all"; everything else gets rounded
	 * into a usable shape rather than rejected, so a careless config
	 * degrades instead of refusing to start.
	 */
	buf_min = params->buf_min;
	buf_max = params->buf_max != 0 ? params->buf_max : URING_DEF_BUF_MAX;
	pool_bytes = params->pool_bytes != 0 ? params->pool_bytes
					     : URING_DEF_POOL_BYTES;

	if (buf_min != 0) {
		if (buf_min < URING_MIN_BUF_MIN)
			buf_min = URING_MIN_BUF_MIN;

		/* Classes are powers of two, so the ends must be too. */
		buf_min = uring_roundup_pow2(buf_min);
		buf_max = uring_roundup_pow2(buf_max);

		if (buf_max < buf_min)
			buf_max = buf_min;

		/* pool_bytes is left exactly as configured: it is pinned
		 * memory, so the planner lowers buf_max to fit the budget
		 * rather than the budget being raised to fit buf_max.
		 */
	}

	engine = gsh_calloc(1, sizeof(*engine), params->mem_comp);
	engine->params = *params;

	(void)snprintf(engine->name, sizeof(engine->name), "%s",
		       params->name != NULL ? params->name : "uring_engine");

	engine->nr_rings = nr_rings;
	engine->per_cpu_rings = params->per_cpu_rings;
	engine->force_async = params->force_async;
	engine->rings =
		gsh_calloc(nr_rings, sizeof(*engine->rings), params->mem_comp);

	for (i = 0; i < nr_rings; i++) {
		struct uring_ring *ring = &engine->rings[i];

		ring->engine = engine;
		ring->idx = i;

		memset(&ring_params, 0, sizeof(ring_params));

		/* One I/O can produce two CQEs (data plus linked SQE), so
		 * give the CQ twice the SQ. Without this a burst of linked
		 * operations can overflow the CQ and make io_uring_submit()
		 * return -EBUSY.
		 */
		ring_params.flags = IORING_SETUP_CQSIZE;
		ring_params.cq_entries = queue_depth * 2;

		if (want_sqpoll) {
			ring_params.flags |= IORING_SETUP_SQPOLL;
			ring_params.sq_thread_idle = sqpoll_idle_ms;

			if (i < sqpoll_groups) {
				/* One of the rings that starts a poller. */
				if (params->sqpoll_cpu >= 0) {
					ring_params.flags |=
						IORING_SETUP_SQ_AFF;
					ring_params.sq_thread_cpu =
						(params->sqpoll_cpu + (int)i);
				}
			} else {
				/* Share an existing poller rather than start
				 * another one.
				 */
				ring_params.flags |= IORING_SETUP_ATTACH_WQ;
				ring_params.wq_fd =
					(unsigned int)engine
						->rings[i % sqpoll_groups]
						.ring.ring_fd;
			}
		}

		rc = io_uring_queue_init_params(queue_depth, &ring->ring,
						&ring_params);

		if (rc < 0 && want_sqpoll && i == 0) {
			/* SQPOLL needs CAP_SYS_NICE, which a hardened unit
			 * file may not grant. That is not worth refusing to
			 * start over, so drop it and carry on with ordinary
			 * submission. The capability is process wide, so if
			 * ring 0 gets a poller the rest will too, and a
			 * failure later is a real error rather than a
			 * permission problem.
			 */
			URING_LOG(engine, URING_LOG_WARN,
				  "%s: could not start with SQPOLL (%s),"
				  " falling back to ordinary submission",
				  engine->name, strerror(-rc));

			want_sqpoll = false;

			memset(&ring_params, 0, sizeof(ring_params));
			ring_params.flags = IORING_SETUP_CQSIZE;
			ring_params.cq_entries = queue_depth * 2;

			rc = io_uring_queue_init_params(queue_depth,
							&ring->ring,
							&ring_params);
		}

		if (rc < 0) {
			URING_LOG(engine, URING_LOG_WARN,
				  "%s: io_uring_queue_init_params(%u) failed: %s",
				  engine->name, queue_depth, strerror(-rc));
			goto fail;
		}

		ring->ring_init = true;
		PTHREAD_MUTEX_init(&ring->submit_lock, NULL);
		PTHREAD_MUTEX_init(&ring->cq_lock, NULL);

		if (params->iowq_max_workers != 0) {
#ifdef HAVE_IO_URING_REGISTER_IOWQ_MAX_WORKERS
			/* [0] is the bounded class, which is what file I/O
			 * uses; [1] is unbounded. A zero means "leave that
			 * class alone". The call rewrites the array with the
			 * previous values, so it cannot be shared or const.
			 */
			unsigned int iowq[2] = { params->iowq_max_workers, 0 };

			rc = io_uring_register_iowq_max_workers(&ring->ring,
								iowq);

			if (rc < 0) {
				URING_LOG(engine, URING_LOG_WARN,
					  "%s: ring %u could not cap io-wq workers at %u: %s",
					  engine->name, i,
					  params->iowq_max_workers,
					  strerror(-rc));
			}
#else
			URING_LOG(engine, URING_LOG_WARN,
				  "%s: liburing is too old to cap io-wq workers, ignoring the limit of %u",
				  engine->name, params->iowq_max_workers);
#endif
		}
	}

	/* Off unless explicitly asked for. */
	engine->coalesce_enable = params->coalesce_enable;

	engine->coalesce_bucket_io_count =
		params->coalesce_bucket_io_count != 0
			? params->coalesce_bucket_io_count
			: URING_DEF_COALESCE_BUCKET_IO_COUNT;
	engine->coalesce_max_bucket_io_count =
		params->coalesce_max_bucket_io_count != 0
			? params->coalesce_max_bucket_io_count
			: URING_DEF_COALESCE_MAX_BUCKET_IO_COUNT;
	engine->coalesce_timeout_us = params->coalesce_timeout_us != 0
					      ? params->coalesce_timeout_us
					      : URING_DEF_COALESCE_TIMEOUT_US;
	engine->coalesce_outstanding_max =
		params->coalesce_outstanding_max != 0
			? params->coalesce_outstanding_max
			: URING_DEF_COALESCE_OUTSTANDING_MAX;
	engine->coalesce_cif_threshold =
		params->coalesce_cif_threshold != 0
			? params->coalesce_cif_threshold
			: URING_DEF_COALESCE_CIF_THRESHOLD;

	engine->sqpoll = want_sqpoll;

	/* Ask the kernel which opcodes it implements before any consumer can
	 * plan work. All rings are identical, so probing ring 0 is enough.
	 */
	uring_probe_opcodes(engine);

	if (buf_min != 0)
		uring_pool_create(engine, buf_min, buf_max, pool_bytes);

	for (i = 0; i < nr_rings; i++) {
		struct uring_ring *ring = &engine->rings[i];

		rc = pthread_create(&ring->reaper, NULL, uring_reaper, ring);

		if (rc != 0) {
			URING_LOG(engine, URING_LOG_WARN,
				  "%s: could not start reaper %u: %s",
				  engine->name, i, strerror(rc));
			rc = -rc;
			goto fail;
		}

		ring->reaper_started = true;
	}

	URING_LOG(engine, URING_LOG_INFO,
		  "%s: io_uring engine up, %u ring(s) of %u entries,"
		  " %u fixed buffer(s), per_cpu=%s, sqpoll=%s,"
		  " force_async=%s, coalesce=%s",
		  engine->name, nr_rings, queue_depth, engine->nr_bufs,
		  engine->per_cpu_rings ? "yes" : "no",
		  engine->sqpoll ? "yes" : "no",
		  engine->force_async ? "yes" : "no",
		  engine->coalesce_enable ? "yes" : "no");

	if (engine->coalesce_enable) {
		URING_LOG(engine, URING_LOG_INFO,
			  "%s: submission coalescing on: bucket=%u max_bucket=%u timeout=%" PRIu64
			  "us outstanding_max=%u cif=%u — submitting threads spin while batching",
			  engine->name, engine->coalesce_bucket_io_count,
			  engine->coalesce_max_bucket_io_count,
			  engine->coalesce_timeout_us,
			  engine->coalesce_outstanding_max,
			  engine->coalesce_cif_threshold);
	}

	if (engine->sqpoll) {
		URING_LOG(engine, URING_LOG_INFO,
			  "%s: submission runs on %u kernel poller(s)"
			  " (iou-sqp-*), idle=%ums, first cpu=%d",
			  engine->name, sqpoll_groups, sqpoll_idle_ms,
			  params->sqpoll_cpu);
		URING_LOG(engine, URING_LOG_INFO,
			  "%s: coalescing DISABLED (sqpoll mode — no syscall to batch)",
			  engine->name);
	} else {
		URING_LOG(engine, URING_LOG_INFO,
			  "%s: coalescing bucket_io_count=%u max_bucket=%u timeout_us=%" PRIu64
			  " outstanding_max=%u cif_threshold=%u",
			  engine->name,
			  engine->coalesce_bucket_io_count,
			  engine->coalesce_max_bucket_io_count,
			  engine->coalesce_timeout_us,
			  engine->coalesce_outstanding_max,
			  engine->coalesce_cif_threshold);
		if (engine->coalesce_bucket_io_count == 1) {
			URING_LOG(engine, URING_LOG_INFO,
				  "%s: coalescing effectively disabled (bucket_io_count=1)",
				  engine->name);
		}
	}

	if (engine->force_async) {
		URING_LOG(engine, URING_LOG_WARN,
			  "%s: force_async is on, every I/O is handed"
			  " to an io-wq worker - this is a diagnostic"
			  " setting",
			  engine->name);
	}

	*engine_out = engine;

	return 0;

fail:
	uring_engine_destroy(engine);

	return rc;
}

/**
 * @brief Wake a reaper that is parked in io_uring_wait_cqe()
 *
 * A NOP with user_data 0 is the agreed sentinel; the reaper ignores it and
 * re-tests engine->stopping.
 */
static void uring_wake_reaper(struct uring_ring *ring)
{
	struct io_uring_sqe *sqe;
	uint32_t n;
	int tries;

	for (tries = 0; tries < URING_SUBMIT_RETRIES; tries++) {
		PTHREAD_MUTEX_lock(&ring->submit_lock);

		sqe = io_uring_get_sqe(&ring->ring);

		if (sqe != NULL) {
			io_uring_prep_nop(sqe);
			io_uring_sqe_set_data(sqe, NULL);

			/*
			 * Flush any pending coalesce batch together with the
			 * NOP. Capture io_count before the submit so spinning
			 * threads see io_count == 0 after the call and exit
			 * immediately rather than timing out.
			 */
			n = atomic_fetch_uint32_t(&ring->io_count);
			(void)io_uring_submit(&ring->ring);
			if (n > 0) {
				atomic_add_uint32_t(&ring->outstanding_io, n);
				atomic_store_uint32_t(&ring->io_count, 0);
				atomic_store_uint64_t(&ring->first_io_time_us,
						      0);
			}

			PTHREAD_MUTEX_unlock(&ring->submit_lock);
			return;
		}

		/*
		 * SQ full — flush what is queued so space opens up, then
		 * retry. Same counter update as above.
		 */
		n = atomic_fetch_uint32_t(&ring->io_count);
		(void)io_uring_submit(&ring->ring);
		if (n > 0) {
			atomic_add_uint32_t(&ring->outstanding_io, n);
			atomic_store_uint32_t(&ring->io_count, 0);
			atomic_store_uint64_t(&ring->first_io_time_us, 0);
		}

		PTHREAD_MUTEX_unlock(&ring->submit_lock);
		sched_yield();
	}

	URING_LOG(ring->engine, URING_LOG_CRIT,
		  "%s: could not post shutdown NOP to ring %u",
		  ring->engine->name, ring->idx);
}

void uring_engine_destroy(struct uring_engine *engine)
{
	unsigned int i;

	if (engine == NULL)
		return;

	__atomic_store_n(&engine->stopping, true, __ATOMIC_RELEASE);

	for (i = 0; i < engine->nr_rings; i++) {
		struct uring_ring *ring = &engine->rings[i];

		if (!ring->reaper_started)
			continue;

		uring_wake_reaper(ring);
		(void)pthread_join(ring->reaper, NULL);
		ring->reaper_started = false;
	}

	if (engine->bufs_registered) {
		for (i = 0; i < engine->nr_rings; i++) {
			(void)io_uring_unregister_buffers(
				&engine->rings[i].ring);
		}
		engine->bufs_registered = false;
	}

	uring_pool_destroy(engine);

	for (i = 0; i < engine->nr_rings; i++) {
		struct uring_ring *ring = &engine->rings[i];

		if (!ring->ring_init)
			continue;

		io_uring_queue_exit(&ring->ring);
		PTHREAD_MUTEX_destroy(&ring->submit_lock);
		PTHREAD_MUTEX_destroy(&ring->cq_lock);
		ring->ring_init = false;
	}

	gsh_free(engine->rings, engine->params.mem_comp);
	gsh_free(engine, engine->params.mem_comp);
}

#endif /* USE_FSAL_URING */
