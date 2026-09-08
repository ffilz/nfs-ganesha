/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
 */

/**
 * @file gsh_rados_watch.h
 * @brief Self-healing librados watch registration
 *
 * Detect expired watches through the error callback and periodic probes,
 * then re-establish them on a monitor thread with bounded backoff. A caller
 * reconcile callback refreshes state missed while the watch was down.
 */

#ifndef GSH_RADOS_WATCH_H
#define GSH_RADOS_WATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <rados/librados.h>

#include "log_common.h"
#include "mem_components.h"

/* Ceph's EBLOCKLISTED (Linux ESHUTDOWN), not exported by librados.h. */
#define GSH_RADOS_EBLOCKLISTED 108

/** @brief Probe interval while the watch is healthy, in seconds */
#define GSH_RADOS_WATCH_PROBE_SEC 30

/** @brief First retry delay after a failed re-watch, in seconds */
#define GSH_RADOS_WATCH_BACKOFF_MIN_SEC 5

/** @brief Cap on the retry delay after a failed re-watch, in seconds */
#define GSH_RADOS_WATCH_BACKOFF_MAX_SEC 60

/** @brief Slow retry interval for missing objects or blocklisted clients */
#define GSH_RADOS_WATCH_TERMINAL_SEC 600

enum gsh_rados_watch_state {
	GSH_RADOS_WATCH_OK, /*< Registered and healthy, cookie is valid */
	GSH_RADOS_WATCH_BROKEN, /*< Needs to be re-established */
	GSH_RADOS_WATCH_TERMINAL /*< Object gone or blocklisted, slow retry */
};

struct gsh_rados_watch {
	pthread_mutex_t grw_mtx;
	pthread_cond_t grw_cv; /*< Woken by the errcb and by unregister */
	pthread_t grw_thread;

	/* Not owned by us, the caller outlives the watch */
	rados_ioctx_t grw_ioctx;

	char *grw_oid; /*< Our own copy of the object name */
	rados_watchcb2_t grw_cb;
	void *grw_cb_arg;
	uint32_t grw_timeout; /*< Watch timeout handed to rados_watch3() */

	uint64_t grw_cookie; /*< 0 when not registered */
	enum gsh_rados_watch_state grw_state;
	bool grw_err; /*< errcb fired since the last re-watch */
	bool grw_rewatching; /*< A re-watch is in flight, cookie unpublished */
	bool grw_shutdown; /*< Unregister requested, stop retrying */
	uint32_t grw_backoff; /*< Current retry delay in seconds */

	/** Called after each re-establishment, never after the initial
	 *  registration. Runs on the monitor thread with no lock held.
	 */
	void (*grw_reconcile)(void);

	log_components_t grw_comp;
	mem_components_t grw_mem_comp;
	const char *grw_name; /*< Short name used in logs and thread name */
};

/* Caller must serialize register/unregister calls for each watch. */
int gsh_rados_watch_register(struct gsh_rados_watch *watch, rados_ioctx_t ioctx,
			     const char *oid, rados_watchcb2_t cb, void *cb_arg,
			     uint32_t timeout, void (*reconcile)(void),
			     log_components_t comp, mem_components_t mem_comp,
			     const char *name);

void gsh_rados_watch_unregister(struct gsh_rados_watch *watch);

#endif /* GSH_RADOS_WATCH_H */
