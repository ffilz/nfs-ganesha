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
 * @file  Protocols/NFS_URING/nfs4_uring_resume_pool.c
 * @brief Dedicated work pool for io_uring COMPOUND resumes.
 *
 * WHY
 *
 * libntirpc has exactly one work pool, svc_work_pool, and everything goes
 * through its single pthread_mutex: epoll channel events (svc_rqst.c:528),
 * request dispatch, reply output queues (svc_ioq.c:511, svc_vc.c:593) and
 * svc_resume() (svc_rqst.c:1369). Worker threads also re-take that mutex
 * after every task and to park on their condvar.
 *
 * A synchronous request crosses that mutex twice: dispatch, then the worker
 * re-locking after it has run the request to completion inline. A request
 * that suspends on io_uring crosses it four times -- dispatch, the worker
 * parking after it submitted the chain, the reaper's svc_resume(), and the
 * second worker re-locking after it has encoded and sent the reply -- and
 * costs two thread wakeups instead of one.
 *
 * So turning on the io_uring COMPOUND engine roughly doubles the traffic
 * through the busiest lock in the server. Profiling a 4K read workload
 * showed 35% of all samples in __lll_lock_wait and 60% in
 * finish_task_switch, against 1.2% in the actual data copy.
 *
 * WHAT
 *
 * A second work_pool, used only for the rq_wpe of requests that went down
 * the io_uring COMPOUND path. struct work_pool is fully instance-based --
 * its own queue mutex, its own parked-thread list, its own threads -- so
 * this needs nothing from libntirpc beyond a way to say which pool a given
 * request's resume belongs to, which is struct svc_req::rq_resume_pool.
 *
 * WHAT IT DOES NOT DO
 *
 * It splits the lock; it does not remove the hand-off. The request still
 * travels worker -> reaper -> resume worker, so the context-switch half of
 * the cost is untouched. Batching the reaper's resumes is the follow-up
 * that addresses that; see FSAL_URING/URING_RESUME_POOL.md section 7.
 *
 * ONLY rq_wpe IS ROUTED
 *
 * The resume path itself can submit ioq work to svc_work_pool
 * (nfs4_compound_uring_resume -> complete_request -> svc_sendreply ->
 * svc_ioq_write_now). Routing that to this pool as well would mean a task
 * in the pool waiting on another task in the same pool, which deadlocks
 * once every thread is busy. rq_resume_pool only ever steers rq_wpe, so
 * that cannot happen here -- but it is the reason not to "simplify" this
 * by giving the whole uring path its own pool.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#ifdef HAVE_SVC_RESUME_BATCH

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "common_utils.h" /* PTHREAD_stack_size */
#include "gsh_rpc.h" /* struct work_pool, work_pool_init() */
#include "log.h"
#include "nfs_core.h" /* nfs_param */
#include "nfs4_uring_internal.h"

/**
 * @brief The pool, or all-zero when it was never created.
 *
 * Static rather than heap allocated because work_pool_init() takes a
 * caller-owned struct and the pool outlives everything that uses it.
 */
static struct work_pool uring_resume_work_pool;

/** Set once the pool is usable, cleared before it is torn down. */
static bool uring_resume_pool_live;

struct work_pool *nfs4_uring_resume_pool(void)
{
	/* Read without a barrier on purpose. The flag goes true during
	 * nfs_Init(), before any transport exists, and false only after
	 * destroy_fsals() has stopped every reaper that could resume. A
	 * racing reader is therefore not possible, and paying for an
	 * acquire on a per-request path to defend against it would be
	 * worse than pointless.
	 */
	return uring_resume_pool_live ? &uring_resume_work_pool : NULL;
}

int nfs4_uring_resume_pool_init(void)
{
	struct work_pool_params params;
	uint32_t thrd_min = nfs_param.core_param.uring_resume_thrd_min;
	uint32_t thrd_max = nfs_param.core_param.uring_resume_thrd_max;
	int rc;

	if (!nfs4_uring_compound_enabled()) {
		LogDebug(COMPONENT_INIT,
			 "io_uring COMPOUND engine is off, no resume pool");
		return 0;
	}

	if (thrd_max == 0) {
		LogEvent(
			COMPONENT_INIT,
			"Uring_Resume_ThrdMax is 0: io_uring COMPOUND resumes stay on svc_work_pool");
		return 0;
	}

	if (thrd_min > thrd_max) {
		LogWarn(COMPONENT_INIT,
			"Uring_Resume_ThrdMin (%" PRIu32
			") exceeds Uring_Resume_ThrdMax (%" PRIu32
			"), clamping to the maximum",
			thrd_min, thrd_max);
		thrd_min = thrd_max;
	}

	memset(&params, 0, sizeof(params));
	params.thrd_min = thrd_min;
	params.thrd_max = thrd_max;
	params.thr_stack_size = PTHREAD_stack_size;

	/* work_pool_init() spawns thrd_min threads immediately and grows on
	 * demand up to thrd_max, exactly as it does for svc_work_pool.
	 */
	rc = work_pool_init(&uring_resume_work_pool, "urres", &params);

	if (rc != 0) {
		/* Not fatal. Leaving the pool dead sends every resume back
		 * through svc_work_pool, which is where it went before this
		 * existed, so the server is slower but entirely correct.
		 */
		LogMajor(
			COMPONENT_INIT,
			"Could not create the io_uring resume pool (%d); resumes fall back to svc_work_pool",
			rc);
		return rc;
	}

	uring_resume_pool_live = true;

	LogEvent(COMPONENT_INIT,
		 "io_uring COMPOUND resume pool started, %" PRIu32
		 " to %" PRIu32 " threads",
		 thrd_min, thrd_max);

	return 0;
}

void nfs4_uring_resume_pool_shutdown(void)
{
	if (!uring_resume_pool_live)
		return;

	/*
	 * Must run after destroy_fsals(), which stops the io_uring reapers.
	 * work_pool_shutdown() sets thrd_max to 0, and work_pool_submit()
	 * silently drops anything submitted after that -- a resume dropped
	 * there would strand the request and leak the transport reference
	 * it holds.
	 */
	uring_resume_pool_live = false;

	LogEvent(COMPONENT_MAIN, "Stopping io_uring COMPOUND resume pool");

	if (work_pool_shutdown(&uring_resume_work_pool) != 0)
		LogMajor(
			COMPONENT_MAIN,
			"io_uring COMPOUND resume pool did not shut down cleanly");
	else
		LogEvent(COMPONENT_MAIN,
			 "io_uring COMPOUND resume pool stopped");
}

#else /* !HAVE_SVC_RESUME_BATCH */
#include <stddef.h> /*  NULL */
/*
 * Stub implementations for builds against older libntirpc that does not
 * export work_pool_init/shutdown or provide rq_resume_pool.  Resumes fall
 * back to svc_work_pool exactly as they did before the batch API existed.
 */

struct work_pool *nfs4_uring_resume_pool(void)
{
	return NULL;
}

int nfs4_uring_resume_pool_init(void)
{
	return 0;
}

void nfs4_uring_resume_pool_shutdown(void)
{
}

#endif /* HAVE_SVC_RESUME_BATCH */

#endif /* USE_FSAL_URING */
