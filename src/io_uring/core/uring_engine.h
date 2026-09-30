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
 * @file  uring_engine.h
 * @brief Public API for the opcode-agnostic io_uring engine.
 *
 * This header is intentionally free of FSAL and NFS-Ganesha-specific types.
 * Any module — VFS FSAL, GPFS FSAL, RGW FSAL, or a future network layer —
 * may include it and create an independent engine instance configured at
 * init time via struct uring_engine_params.
 *
 * The only Ganesha-wide header pulled in is mem_components.h, which is
 * project infrastructure (it provides mem_components_t used by gsh_malloc).
 * It has no FSAL, NFS protocol, or log.h dependency.
 */

#ifndef URING_ENGINE_H
#define URING_ENGINE_H

#ifdef USE_FSAL_URING

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>
#include <liburing.h>
/*
 * mem_components.h is project-wide infrastructure (no fsal.h / log.h dep).
 * It provides mem_components_t so the engine can tag every allocation with
 * the correct memory component (MEM_COMP_FSAL, MEM_COMP_LIBNTIRPC, etc.).
 */
#include "mem_components.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Pluggable logger callback
 * -------------------------------------------------------------------------
 * Each consumer wires its own logger at create time:
 *   VFS/GPFS FSAL  -> wraps LogAtLevel(COMPONENT_URING, level, ...)
 *   RGW FSAL       -> wraps LogAtLevel(COMPONENT_FSAL,  level, ...)
 *   unit tests     -> fprintf(stderr, ...) — no Ganesha dep at all
 *
 * @param ctx   Opaque pointer supplied as uring_engine_params::log_ctx.
 * @param level Severity level (mirrors Ganesha log levels; consumers may
 *              use their own convention — the engine does not interpret it).
 * @param fmt   printf-style format string.
 */
typedef void (*uring_log_fn)(void *ctx, int level,
			     const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* -------------------------------------------------------------------------
 * struct uring_engine_params — engine init-time configuration
 * -------------------------------------------------------------------------
 * De-FSAL'd version of fsal_uring_params.  The only fields removed are
 * pers_max (FSAL credential registry — stays in struct fsal_uring_params).
 * Three fields are added: mem_comp, log_fn, log_ctx.
 *
 * All other fields are identical to fsal_uring_params so that
 * fsal_uring_params can embed this struct and forward it unchanged.
 */
struct uring_engine_params {
	/** Name used in log messages and reaper thread names. */
	const char *name;

	/** SQ entries per ring (default 256). */
	unsigned int queue_depth;

	/** Number of rings, each with its own reaper thread (default 1). */
	unsigned int nr_rings;

	/**
	 * Allocate one ring per online CPU and direct submissions to the
	 * ring matching the submitting thread's current CPU. When true,
	 * nr_rings is automatically set to the online CPU count.
	 */
	bool per_cpu_rings;

	/**
	 * Smallest registered-buffer size class (default 16 KiB).
	 * Set to 0 to run with no buffer pool at all (e.g. GPFS, RGW).
	 */
	size_t buf_min;

	/** Largest registered-buffer size class (default 1 MiB). */
	size_t buf_max;

	/** Total bytes pinned across all size classes (default 64 MiB). */
	size_t pool_bytes;

	/**
	 * Run submission on a kernel poller thread (IORING_SETUP_SQPOLL).
	 * Needs CAP_SYS_NICE.  The engine logs and falls back to ordinary
	 * submission if the kernel refuses rather than failing to start.
	 */
	bool sqpoll;

	/** Poller spin time before parking, milliseconds (default 100). */
	unsigned int sqpoll_idle_ms;

	/**
	 * Number of SQPOLL threads to start (default 1, capped at nr_rings).
	 * Rings beyond this count share an existing poller via
	 * IORING_SETUP_ATTACH_WQ.
	 */
	unsigned int sqpoll_groups;

	/**
	 * First CPU to pin pollers to (IORING_SETUP_SQ_AFF), -1 for
	 * unpinned.  Poller n takes CPU sqpoll_cpu + n.
	 */
	int sqpoll_cpu;

	/**
	 * Tag every SQE with IOSQE_ASYNC, forcing the kernel to use an
	 * io-wq worker thread.  Diagnostic knob — not the default.
	 */
	bool force_async;

	/**
	 * Cap on io-wq bounded workers (file I/O) per ring.
	 * 0 = kernel default (typically min(nr_cpus * 4, RLIMIT_NPROC / 2)).
	 */
	unsigned int iowq_max_workers;

	/*
	 * Per-ring adaptive coalescing tunables.
	 * All fields default to built-in values when left 0.
	 */

	/**
	 * Master switch for adaptive coalescing. Defaults to false: an SQE
	 * is placed and submitted in one go, with no accumulation and no
	 * spin-wait.
	 *
	 * This has to be an explicit flag rather than a tuning value.
	 * Setting coalesce_bucket_io_count to 1 does NOT turn coalescing
	 * off: once outstanding I/O passes 4 * coalesce_cif_threshold the
	 * bucket is recomputed as
	 *     base + outstanding / (2 * cif)
	 * which grows regardless of base, so submission starts being held
	 * back exactly when the ring is busiest.
	 */
	bool coalesce_enable;

	/** Base SQE bucket size. Ignored unless coalesce_enable is set. */
	uint32_t coalesce_bucket_io_count;

	/** Adaptive ceiling for the bucket size (default 64). */
	uint32_t coalesce_max_bucket_io_count;

	/**
	 * Maximum age of the oldest pending SQE before a forced flush,
	 * in microseconds (default 100).
	 */
	uint64_t coalesce_timeout_us;

	/**
	 * Back-pressure ceiling: hold new SQEs when outstanding I/O
	 * reaches this count (default 128).
	 */
	uint32_t coalesce_outstanding_max;

	/** Minimum outstanding I/O before batching activates (default 4). */
	uint32_t coalesce_cif_threshold;

	/**
	 * Memory component tag for all engine allocations.
	 * Pass MEM_COMP_FSAL for FSAL consumers,
	 * MEM_COMP_LIBNTIRPC for a future network/RPC consumer.
	 */
	mem_components_t mem_comp;

	/**
	 * Pluggable logger.  NULL silences all engine log output.
	 * The engine does not interpret the level value — it is forwarded
	 * verbatim to the callback so the consumer can map it to whatever
	 * severity enum it uses.
	 */
	uring_log_fn log_fn;

	/** Opaque context pointer forwarded to every log_fn call. */
	void *log_ctx;

	/**
	 * Optional hooks bracketing one completion harvest, called on the
	 * reaper thread immediately before the first callback of a pass and
	 * immediately after the last.
	 *
	 * A reaper routinely takes several CQEs off the ring in one wakeup,
	 * and the completion callbacks typically hand each finished request
	 * somewhere else -- a work queue, a thread pool. Doing that once per
	 * CQE means taking the destination's lock, and usually waking a
	 * thread, once per CQE as well. These hooks let the consumer collect
	 * that work during the pass and dispatch it in one go at the end.
	 *
	 * The engine itself attaches no meaning to them: it does not know
	 * what, if anything, the callbacks produce. Both NULL (the default)
	 * means no batching and is always correct.
	 */
	void (*reap_begin)(void);
	void (*reap_end)(void);
};

/* -------------------------------------------------------------------------
 * struct uring_op — description of one operation to submit
 * -------------------------------------------------------------------------
 * The engine is opcode-agnostic: the caller describes what to do by
 * supplying sqe_pre, which is invoked inside submit_lock just before
 * io_uring_submit().  The engine handles ALL ring/lock/coalesce/reaper
 * logic; the caller handles ALL SQE-layout specifics.
 *
 * This design allows every consumer to supply its own SQE format:
 *   VFS/GPFS FSAL  -> fsal_uring_rw_prep()    (readv/writev, pers stamp)
 *   GPFS ioctl     -> uring_ioctl_prep()       (IORING_OP_URING_CMD)
 *   RGW (future)   -> rgw_uring_cmd_prep()     (io_uring_prep_cmd)
 *   network (future)-> ntirpc_uring_send_prep() (io_uring_prep_sendmsg)
 */
struct uring_op {
	/**
	 * Mandatory SQE preparation callback.
	 * Called inside submit_lock with the already-allocated SQE slot.
	 * On return the engine submits (possibly batching with coalescing).
	 * MUST be non-NULL.
	 *
	 * @param sqe       The allocated SQE to fill in.
	 * @param arg       Opaque value from sqe_pre_arg.
	 * @param ring_idx  Index of the ring this SQE is going into.
	 *                  Needed by consumers that stamp sqe->personality
	 *                  with a per-ring personality id (e.g. FSAL pers).
	 */
	void (*sqe_pre)(struct io_uring_sqe *sqe, void *arg,
			unsigned int ring_idx);
	void *sqe_pre_arg;

	/**
	 * Optional second SQE to hard-link behind the first, e.g. an
	 * fsync after a write (IOSQE_IO_LINK on the first SQE).
	 * NULL = no linked SQE.
	 * Same signature as sqe_pre; ring_idx is the same ring.
	 */
	void (*sqe_pre2)(struct io_uring_sqe *sqe, void *arg,
			 unsigned int ring_idx);
	void *sqe_pre2_arg;

	/** Completion callback invoked from the reaper thread. */
	void (*cb)(void *cb_arg, int res, int res2);
	void *cb_arg;
};

/* -------------------------------------------------------------------------
 * Linked SQE chains — submitting a whole compound in one io_uring_submit()
 * -------------------------------------------------------------------------
 * A chain is N operations hard-linked with IOSQE_IO_LINK so the kernel runs
 * them in order and cancels the remainder (-ECANCELED) if one fails.  All N
 * SQEs are placed and submitted inside a single hold of the ring's
 * submit_lock, which is what makes the chain contiguous: if the lock were
 * dropped between placement and submission another thread could slip an
 * unrelated SQE into the middle of the link sequence.
 *
 * Like struct uring_op, the engine stays opcode-agnostic — the caller's
 * sqe_pre() decides the opcode, and is also where a consumer stamps
 * sqe->personality, since the engine has no notion of credentials.
 */

/**
 * Maximum SQEs in one chain.
 *
 * Deliberately a plain constant rather than NFS4_MAX_OPERATIONS: this header
 * must stay free of NFS-Ganesha types.  Callers that can exceed it must split
 * their work into several chains; submit_chain() rejects anything larger with
 * -E2BIG rather than truncating.
 */
#define URING_CHAIN_MAX_OPS 64

/** One slot in a chain: how to fill the SQE at this position. */
struct uring_chain_op {
	/**
	 * Mandatory SQE preparation callback, same contract as
	 * struct uring_op::sqe_pre.  Called inside submit_lock.
	 */
	void (*sqe_pre)(struct io_uring_sqe *sqe, void *arg,
			unsigned int ring_idx);
	void *sqe_pre_arg;

	/**
	 * Extra IOSQE_* bits to OR into the SQE.  The engine adds
	 * IOSQE_IO_LINK itself on every slot but the last, so callers must
	 * not set it here.
	 */
	uint32_t sqe_flags;
};

/** Per-operation result harvested from the CQE. */
struct uring_cqe_info {
	int32_t res;    /**< kernel return value, negative is -errno */
	uint32_t flags; /**< CQE flags */
};

/*
 * Per-slot completion tag.  One of these is pointed at by each SQE's
 * user_data, which is how a CQE is matched back to its position in the
 * chain.  Written by the engine at submit time; callers never touch it.
 *
 * It is a distinct object per slot (rather than the chain pointer plus an
 * index packed into spare user_data bits) so that no assumption is made
 * about how many bits of a userspace pointer are free.
 */
struct uring_chain_slot {
	struct uring_chain_req *chain;
	uint32_t idx;
};

/**
 * One chain request.  Allocated and owned by the caller, which must keep it
 * alive — and not relocate it — until done_cb() fires, because the in-flight
 * SQEs carry pointers into slots[].
 */
struct uring_chain_req {
	/** Number of valid entries in ops[] / cqes[]; <= URING_CHAIN_MAX_OPS. */
	unsigned int nr_ops;

	/** CQEs seen so far; the engine bumps this atomically. */
	uint32_t nr_done;

	/**
	 * Ring the chain was submitted to. Set by
	 * uring_engine_submit_chain(); the caller needs it to poll for its
	 * own completion with uring_engine_poll() instead of waiting for
	 * the reaper.
	 */
	unsigned int ring_idx;

	/**
	 * Fired exactly once, from the reaper thread, when the last CQE of
	 * the chain lands.  Runs with no op_ctx and no FSAL thread state, so
	 * it should do nothing heavier than rescheduling onto a worker.
	 */
	void (*done_cb)(void *cb_arg, struct uring_chain_req *chain);
	void *cb_arg;

	struct uring_chain_op ops[URING_CHAIN_MAX_OPS];
	struct uring_cqe_info cqes[URING_CHAIN_MAX_OPS];

	/** Engine-private completion tags, one per slot. */
	struct uring_chain_slot slots[URING_CHAIN_MAX_OPS];
};

/* -------------------------------------------------------------------------
 * struct uring_buf — registered buffer entry (was fsal_uring_buf)
 * -------------------------------------------------------------------------
 * Returned by uring_buf_get(), released by uring_buf_put().
 * The engine field ties the buffer back to its owning engine so that
 * uring_buf_put() needs no engine argument from the caller.
 */
struct uring_buf {
	struct uring_engine *engine;
	void *addr;
	size_t len;
	unsigned int index; /* io_uring fixed-buffer slot index */
	unsigned int cls;   /* size-class index within the pool */
};

/* -------------------------------------------------------------------------
 * struct uring_engine_stats — observability snapshot
 * -------------------------------------------------------------------------
 * Filled in atomically by uring_engine_get_stats().  All counters are
 * monotonically increasing; callers diff successive snapshots for rates.
 */
struct uring_engine_stats {
	uint64_t sqe_submitted;       /**< Total SQEs handed to the kernel. */
	uint64_t cqe_completed;       /**< Total CQEs reaped (res >= 0). */
	uint64_t cqe_failed;          /**< Total CQEs reaped (res < 0). */
	uint64_t sq_full_rejections;  /**< SQEs dropped because SQ was full. */
	uint64_t buf_pool_hits;       /**< uring_buf_get() served from pool. */
	uint64_t buf_pool_misses;     /**< uring_buf_get() fell back to heap. */
	uint64_t coalesce_batches;    /**< Number of coalesced flush events. */
	uint64_t coalesce_peak_batch; /**< Largest batch seen in one flush. */
};

/* -------------------------------------------------------------------------
 * struct uring_engine — opaque handle (definition in uring_engine.c)
 * -------------------------------------------------------------------------
 */
struct uring_engine;

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------
 */

/**
 * uring_engine_create - allocate and start a new engine instance.
 *
 * @params:     Init-time configuration.  Copied internally; the caller
 *              may free or reuse the struct after this call returns.
 * @engine_out: Receives a pointer to the new engine on success.
 *
 * Returns 0 on success, negative errno on failure.
 */
int uring_engine_create(const struct uring_engine_params *params,
			struct uring_engine **engine_out);

/**
 * uring_engine_destroy - drain, stop, and free an engine instance.
 *
 * Waits for in-flight operations to complete before tearing down rings
 * and reaper threads.  After this returns the pointer is invalid.
 */
void uring_engine_destroy(struct uring_engine *engine);

/**
 * uring_engine_submit - submit one operation to the engine.
 *
 * The engine selects a ring, calls op->sqe_pre() inside submit_lock to
 * prepare the SQE, and submits (possibly deferred by coalescing).
 * op->cb() is called from the reaper thread on completion.
 *
 * Returns 0 on success, negative errno on failure (e.g. -ENOSPC if SQ
 * is full and coalescing is disabled).
 */
int uring_engine_submit(struct uring_engine *engine,
			const struct uring_op *op);

/**
 * uring_engine_submit_chain - submit an N-SQE linked chain atomically.
 *
 * Every SQE in @chain is placed and io_uring_submit() is called inside one
 * uninterrupted hold of the ring's submit_lock.  This deliberately bypasses
 * the adaptive coalescing used by uring_engine_submit(): IOSQE_IO_LINK means
 * "link me to the next SQE in the ring", so releasing the lock mid-chain
 * would let another thread's SQE be linked into the chain by accident, and a
 * failure anywhere in the chain would then cancel that unrelated request.
 *
 * Any SQEs other threads had already queued for coalescing are flushed by
 * the same submit call and accounted for, so no I/O is double counted.
 *
 * All SQEs go to one ring, chosen by the engine.  @chain->done_cb is called
 * once from that ring's reaper thread after the last CQE lands, with every
 * entry of @chain->cqes[] filled in.
 *
 * @engine: Engine to submit through.
 * @chain:  Caller-allocated request with nr_ops and ops[] filled in.  Must
 *          stay alive and in place until done_cb() fires.
 *
 * Returns 0 on success and done_cb() will fire, or a negative errno on
 * failure in which case done_cb() will NOT fire:
 *   -EINVAL    nr_ops is 0
 *   -E2BIG     nr_ops exceeds URING_CHAIN_MAX_OPS
 *   -ESHUTDOWN engine is stopping or has been fenced off
 *   -EAGAIN    the SQ has no room for the whole chain
 */
int uring_engine_submit_chain(struct uring_engine *engine,
			      struct uring_chain_req *chain);

/**
 * uring_engine_op_supported - does this kernel implement @opcode?
 *
 * The engine probes the running kernel once at create time
 * (io_uring_get_probe_ring) and caches the answer, so this is a cheap bit
 * test callers can make per operation while planning a chain.
 *
 * This is what lets a binary built against new liburing headers run on an
 * older kernel: an unsupported opcode is detected during planning and the
 * caller falls back to its synchronous handler, instead of discovering the
 * problem as an -EINVAL CQE after the request has already been committed.
 *
 * @opcode: An IORING_OP_* value.
 *
 * Returns false for an unknown or unsupported opcode, and false if the
 * kernel did not support IORING_REGISTER_PROBE at all (in which case the
 * engine conservatively reports nothing beyond the always-present opcodes).
 */
bool uring_engine_op_supported(struct uring_engine *engine,
			       unsigned int opcode);

/**
 * uring_buf_get - obtain a registered buffer of at least @len bytes.
 *
 * Returns a buffer from the pool if one is available, otherwise falls
 * back to a heap allocation.  Never returns NULL (aborts on OOM).
 * Release with uring_buf_put().
 */
struct uring_buf *uring_buf_get(struct uring_engine *engine, size_t len);

/**
 * uring_buf_put - return a buffer obtained from uring_buf_get().
 */
void uring_buf_put(struct uring_buf *buf);

/**
 * uring_buf_max_size - return the maximum size serviced by the pool.
 *
 * I/O larger than this value will always result in a heap allocation.
 * Returns 0 if the engine was created with buf_min == 0 (no pool).
 */
size_t uring_buf_max_size(struct uring_engine *engine);

/**
 * uring_engine_raw_ring - escape hatch for buffer registration.
 *
 * Returns a pointer to the underlying struct io_uring for @ring_idx so
 * that callers can call io_uring_register_buffers() directly during
 * engine setup.  Must not be used to submit SQEs (use uring_engine_submit).
 *
 * Returns NULL if @ring_idx >= nr_rings.
 */
struct io_uring *uring_engine_raw_ring(struct uring_engine *engine,
				       unsigned int ring_idx);

/**
 * uring_engine_nr_rings - return the number of rings in the engine.
 *
 * Used by consumers (e.g. the FSAL personality registry) that need to
 * iterate over rings without access to the opaque struct uring_engine.
 */
unsigned int uring_engine_nr_rings(struct uring_engine *engine);

/**
 * @brief Harvest and process whatever is already complete on one ring.
 *
 * Non-blocking, and safe to call concurrently with the reaper and with
 * other callers: the dequeue is serialised on the ring's cq_lock and the
 * callbacks run outside it.
 *
 * This is what lets a thread that has just submitted collect its own
 * completion rather than suspend and wait to be handed it back. For an
 * operation the kernel satisfies inline during io_uring_enter() -- a page
 * cache read, say -- the CQE is already there when submit returns, so one
 * call finishes the request with no thread hand-off at all.
 *
 * Processes at most one harvest round (64 CQEs), deliberately: a worker
 * polling for its own completion must not be conscripted into draining an
 * unbounded backlog. Completions for other requests encountered on the way
 * are processed too, which is useful work, not a side effect to avoid.
 *
 * @return Number of completions processed.
 */
unsigned int uring_engine_poll(struct uring_engine *engine,
			       unsigned int ring_idx);

/**
 * uring_engine_get_stats - copy a snapshot of engine counters into @out.
 *
 * Thread-safe; uses atomic loads so the snapshot is not a consistent
 * point-in-time view of all counters, but individual counter reads are
 * not torn.
 */
void uring_engine_get_stats(struct uring_engine *engine,
			    struct uring_engine_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* USE_FSAL_URING */

#endif /* URING_ENGINE_H */
