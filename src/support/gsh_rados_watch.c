// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2026 Nfs-Ganesha contributors
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
 * Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * gsh_rados_watch: self-healing librados watch registration
 *
 * See gsh_rados_watch.h for the rationale.
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "abstract_mem.h"
#include "common_utils.h"
#include "gsh_rados_watch.h"
#include "log.h"

/* librados shares one argument between callbacks; forward the caller's
 * argument to the notify callback.
 */
static void gsh_rados_watch_notifycb(void *arg, uint64_t notify_id,
				     uint64_t handle, uint64_t notifier_id,
				     void *data, size_t data_len)
{
	struct gsh_rados_watch *watch = arg;

	watch->grw_cb(watch->grw_cb_arg, notify_id, handle, notifier_id, data,
		      data_len);
}

/* Defer librados calls to the monitor, off this completion thread. */
static void gsh_rados_watch_errcb(void *arg, uint64_t cookie, int err)
{
	struct gsh_rados_watch *watch = arg;

	PTHREAD_MUTEX_lock(&watch->grw_mtx);

	if (watch->grw_shutdown) {
		PTHREAD_MUTEX_unlock(&watch->grw_mtx);
		return;
	}

	/* Accept errors during re-watch: the new cookie is not published yet.
	 * Otherwise, ignore errors from replaced registrations.
	 */
	if (!watch->grw_rewatching && cookie != watch->grw_cookie) {
		PTHREAD_MUTEX_unlock(&watch->grw_mtx);
		return;
	}

	LogEvent(watch->grw_comp,
		 "%s watch on %s failed: %d, will re-establish it",
		 watch->grw_name, watch->grw_oid, err);

	watch->grw_err = true;
	PTHREAD_COND_signal(&watch->grw_cv);

	PTHREAD_MUTEX_unlock(&watch->grw_mtx);
}

static bool gsh_rados_watch_terminal(int ret)
{
	return ret == -ENOENT || ret == -GSH_RADOS_EBLOCKLISTED;
}

/* Enter/return with grw_mtx held; drop it around blocking librados calls. */
static void gsh_rados_watch_rewatch(struct gsh_rados_watch *watch)
{
	rados_ioctx_t ioctx = watch->grw_ioctx;
	char *oid = watch->grw_oid;
	uint64_t old_cookie = watch->grw_cookie;
	uint64_t new_cookie = 0;
	void (*reconcile)(void) = NULL;
	int ret = 0;

	watch->grw_err = false;
	watch->grw_cookie = 0;
	watch->grw_rewatching = true;

	PTHREAD_MUTEX_unlock(&watch->grw_mtx);

	if (old_cookie != 0) {
		/* Best effort: only a blocklist stops us registering again */
		ret = rados_unwatch2(ioctx, old_cookie);
		if (ret == -GSH_RADOS_EBLOCKLISTED)
			goto out;
		if (ret < 0)
			LogDebug(watch->grw_comp,
				 "Failed to unwatch stale %s cookie %" PRIu64
				 ": %d",
				 watch->grw_name, old_cookie, ret);
	}

	/* Teardown may have started while the unwatch blocked. Registering
	 * again would only keep unregister waiting in pthread_join().
	 */
	PTHREAD_MUTEX_lock(&watch->grw_mtx);
	if (watch->grw_shutdown)
		goto out_locked;
	PTHREAD_MUTEX_unlock(&watch->grw_mtx);

	ret = rados_watch3(ioctx, oid, &new_cookie, gsh_rados_watch_notifycb,
			   gsh_rados_watch_errcb, watch->grw_timeout, watch);

out:
	PTHREAD_MUTEX_lock(&watch->grw_mtx);

out_locked:
	watch->grw_rewatching = false;

	/* Publish even during shutdown; unregister drops it after the join. */
	if (ret == 0)
		watch->grw_cookie = new_cookie;

	if (watch->grw_shutdown)
		return;

	if (ret == 0) {
		watch->grw_state = GSH_RADOS_WATCH_OK;
		watch->grw_backoff = GSH_RADOS_WATCH_BACKOFF_MIN_SEC;
		LogEvent(watch->grw_comp,
			 "Re-established %s watch on %s, cookie %" PRIu64,
			 watch->grw_name, oid, new_cookie);
		reconcile = watch->grw_reconcile;
	} else if (gsh_rados_watch_terminal(ret)) {
		if (watch->grw_state != GSH_RADOS_WATCH_TERMINAL) {
			LogCrit(watch->grw_comp,
				"Cannot re-establish %s watch on %s: %d. %s Retrying every %d seconds.",
				watch->grw_name, oid, ret,
				ret == -ENOENT
					? "The watched object is gone."
					: "This node is blocklisted by the "
					  "cluster; a restart may be required.",
				GSH_RADOS_WATCH_TERMINAL_SEC);
		}
		watch->grw_state = GSH_RADOS_WATCH_TERMINAL;
	} else {
		LogWarn(watch->grw_comp,
			"Failed to re-establish %s watch on %s: %d, retrying in %u seconds",
			watch->grw_name, oid, ret, watch->grw_backoff);
		watch->grw_state = GSH_RADOS_WATCH_BROKEN;
	}

	if (reconcile != NULL) {
		/* Refresh state missed while the watch was down. */
		PTHREAD_MUTEX_unlock(&watch->grw_mtx);
		reconcile();
		PTHREAD_MUTEX_lock(&watch->grw_mtx);
	}
}

/* Called with grw_mtx held; advances the retry backoff. */
static uint32_t gsh_rados_watch_delay(struct gsh_rados_watch *watch)
{
	uint32_t delay;

	switch (watch->grw_state) {
	case GSH_RADOS_WATCH_OK:
		return GSH_RADOS_WATCH_PROBE_SEC;

	case GSH_RADOS_WATCH_TERMINAL:
		return GSH_RADOS_WATCH_TERMINAL_SEC;

	default:
		delay = watch->grw_backoff;
		watch->grw_backoff = MIN(watch->grw_backoff * 2,
					 GSH_RADOS_WATCH_BACKOFF_MAX_SEC);
		return delay;
	}
}

/* Probe periodically to catch failures even if the error callback is missed. */
static void *gsh_rados_watch_thread(void *arg)
{
	struct gsh_rados_watch *watch = arg;
	struct timespec timeout;
	uint32_t delay;

	SetNameFunction(watch->grw_name);

	PTHREAD_MUTEX_lock(&watch->grw_mtx);

	while (!watch->grw_shutdown) {
		bool broken = watch->grw_err;

		if (!broken && watch->grw_state == GSH_RADOS_WATCH_OK) {
			rados_ioctx_t ioctx = watch->grw_ioctx;
			uint64_t cookie = watch->grw_cookie;
			int ret;

			/* Cheap: this only queries local bookkeeping */
			PTHREAD_MUTEX_unlock(&watch->grw_mtx);
			ret = rados_watch_check(ioctx, cookie);
			PTHREAD_MUTEX_lock(&watch->grw_mtx);

			if (watch->grw_shutdown)
				break;

			/* The errcb may have run while the lock was dropped
			 * and signalled nobody, so pick its flag up here.
			 */
			broken = watch->grw_err;

			if (!broken && ret < 0) {
				LogWarn(watch->grw_comp,
					"%s watch on %s is no longer valid: %d",
					watch->grw_name, watch->grw_oid, ret);
				broken = true;
			}
		}

		if (broken || watch->grw_state != GSH_RADOS_WATCH_OK)
			gsh_rados_watch_rewatch(watch);

		if (watch->grw_shutdown)
			break;

		/* Same again for the re-watch and the reconcile callback: the
		 * watch is known bad, so do not sleep out the probe interval
		 * on it, let the retry backoff apply instead.
		 */
		if (watch->grw_err && watch->grw_state == GSH_RADOS_WATCH_OK) {
			LogDebug(
				watch->grw_comp,
				"%s watch on %s failed again while being re-established",
				watch->grw_name, watch->grw_oid);
			watch->grw_state = GSH_RADOS_WATCH_BROKEN;
		}

		delay = gsh_rados_watch_delay(watch);

		(void)clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += delay;
		timeout.tv_nsec = 0;

		PTHREAD_COND_timedwait(&watch->grw_cv, &watch->grw_mtx,
				       &timeout);
	}

	PTHREAD_MUTEX_unlock(&watch->grw_mtx);

	return NULL;
}

/**
 * @brief Establish a watch that repairs itself
 *
 * Starts the monitor on success. Initial registration errors are not retried.
 *
 * @param[in,out] watch     Caller-owned watch, must outlive the registration
 * @param[in]     ioctx     The ioctx to watch on, must outlive the watch
 * @param[in]     oid       Object to watch, copied
 * @param[in]     cb        Notify callback
 * @param[in]     cb_arg    Opaque argument for @a cb
 * @param[in]     timeout   Watch timeout in seconds
 * @param[in]     reconcile Called after every re-establishment, may be NULL
 * @param[in]     comp      Log component
 * @param[in]     mem_comp  Memory component
 * @param[in]     name      Short name used in log messages and as thread name
 *
 * @return 0 on success, a negative errno otherwise.
 */
int gsh_rados_watch_register(struct gsh_rados_watch *watch, rados_ioctx_t ioctx,
			     const char *oid, rados_watchcb2_t cb, void *cb_arg,
			     uint32_t timeout, void (*reconcile)(void),
			     log_components_t comp, mem_components_t mem_comp,
			     const char *name)
{
	uint64_t cookie = 0;
	int ret;

	memset(watch, 0, sizeof(*watch));

	PTHREAD_MUTEX_init(&watch->grw_mtx, NULL);
	PTHREAD_COND_init(&watch->grw_cv, NULL);

	watch->grw_ioctx = ioctx;
	watch->grw_oid = gsh_strdup(oid, mem_comp);
	watch->grw_cb = cb;
	watch->grw_cb_arg = cb_arg;
	watch->grw_timeout = timeout;
	watch->grw_reconcile = reconcile;
	watch->grw_comp = comp;
	watch->grw_mem_comp = mem_comp;
	watch->grw_name = name;
	watch->grw_backoff = GSH_RADOS_WATCH_BACKOFF_MIN_SEC;
	watch->grw_state = GSH_RADOS_WATCH_BROKEN;

	/* Do not let rados_watch3() write the cookie in place: the errcb is
	 * armed as soon as it succeeds and reads the cookie under the lock.
	 */
	ret = rados_watch3(ioctx, watch->grw_oid, &cookie,
			   gsh_rados_watch_notifycb, gsh_rados_watch_errcb,
			   timeout, watch);
	if (ret < 0) {
		LogEvent(comp, "Failed to set %s watch on %s: %d", name, oid,
			 ret);
		goto err;
	}

	PTHREAD_MUTEX_lock(&watch->grw_mtx);
	watch->grw_cookie = cookie;
	watch->grw_state = GSH_RADOS_WATCH_OK;
	PTHREAD_MUTEX_unlock(&watch->grw_mtx);

	/* Plain pthread_create() rather than PTHREAD_create(): that wrapper
	 * needs PTHREAD_stack_size, which lives in the ganesha.nfsd binary
	 * and is not resolvable from a dlopen'd module.
	 */
	ret = pthread_create(&watch->grw_thread, NULL, gsh_rados_watch_thread,
			     watch);
	if (ret != 0) {
		LogCrit(comp, "Failed to start %s watch monitor thread: %d",
			name, ret);
		(void)rados_unwatch2(ioctx, cookie);
		(void)rados_watch_flush(rados_ioctx_get_cluster(ioctx));
		watch->grw_cookie = 0;
		ret = -ret;
		goto err;
	}

	LogDebug(comp, "Established %s watch on %s, cookie %" PRIu64, name, oid,
		 cookie);

	return 0;

err:
	gsh_free(watch->grw_oid, mem_comp);
	watch->grw_oid = NULL;
	PTHREAD_COND_destroy(&watch->grw_cv);
	PTHREAD_MUTEX_destroy(&watch->grw_mtx);

	return ret;
}

/**
 * @brief Stop the monitor thread and drop the watch
 *
 * Must be called before the caller destroys the ioctx. Safe to call on a
 * watch that was never registered, and safe to call twice.
 */
void gsh_rados_watch_unregister(struct gsh_rados_watch *watch)
{
	uint64_t cookie;
	int ret;

	if (watch->grw_oid == NULL)
		return;

	PTHREAD_MUTEX_lock(&watch->grw_mtx);
	watch->grw_shutdown = true;
	PTHREAD_COND_signal(&watch->grw_cv);
	PTHREAD_MUTEX_unlock(&watch->grw_mtx);

	/* Registration started the thread; wait for in-flight work. */
	(void)pthread_join(watch->grw_thread, NULL);

	cookie = watch->grw_cookie;
	watch->grw_cookie = 0;

	if (cookie != 0) {
		ret = rados_unwatch2(watch->grw_ioctx, cookie);
		if (ret < 0)
			LogEvent(watch->grw_comp,
				 "Failed to unwatch %s object %s: %d",
				 watch->grw_name, watch->grw_oid, ret);
	}

	/* Drain callbacks before freeing the watch or destroying its ioctx. */
	ret = rados_watch_flush(rados_ioctx_get_cluster(watch->grw_ioctx));
	if (ret < 0)
		LogEvent(watch->grw_comp,
			 "Failed to flush %s watch callbacks: %d",
			 watch->grw_name, ret);

	gsh_free(watch->grw_oid, watch->grw_mem_comp);
	watch->grw_oid = NULL;

	PTHREAD_COND_destroy(&watch->grw_cv);
	PTHREAD_MUTEX_destroy(&watch->grw_mtx);
}
