/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2013
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

/**
 * @defgroup Server statistics management
 * @{
 */

/**
 * @file server_stats.h
 * @author Jim Lieb <jlieb@panasas.com>
 * @brief Server statistics
 */

#ifndef SERVER_STATS_H
#define SERVER_STATS_H

#include <sys/types.h>

extern bool gsh_mem_stats_enabled;
extern const char *mem_stat_names[];
extern struct timespec mem_stats_time;

/*
 * @brief Ganesha per-component memory statistics
 *
 * Enable/disable of accounting is global
 * (gsh_mem_stats_enabled), not per-struct.
 */
struct gsh_mem_stats {
	/*
	 * Lifetime: monotonic for the process; never cleared by
	 * gsh_mem_stats_reset() or D-Bus reset.
	 * # of gsh_mem_stats_update_alloc() calls since start.
	 */
	uint64_t lifetime_alloc_calls;
	/* # of gsh_mem_stats_update_free() calls since start. */
	uint64_t lifetime_free_calls;
	/* Total bytes in alloc paths since start (per-call size, summed). */
	uint64_t lifetime_alloc_bytes;
	/* Total bytes in free paths since start (per-call size, summed). */
	uint64_t lifetime_freed_bytes;

	/*
	 * Interval: zeroed on reset; use for "since last reset" rates and
	 * byte volume. # of alloc calls in the current interval.
	 */
	uint64_t interval_alloc_calls;
	/* # of free calls in the current interval. */
	uint64_t interval_free_calls;
	/* Total alloc bytes in the current interval. */
	uint64_t interval_alloc_bytes;
	/* Total free bytes in the current interval. */
	uint64_t interval_freed_bytes;

	/*
	 * Balance and peaks: current/peak are live; reset refreshes
	 * baseline/interval_peak relative to the new interval.
	 * Best-effort outstanding bytes (net of alloc/free) for this comp.
	 */
	uint64_t current_active_bytes;
	/* Lifetime high-water of current_active_bytes. */
	uint64_t peak_active_bytes;
	/*
	 * current_active_bytes at last reset;
	 * interval delta is (current - baseline).
	 */
	uint64_t baseline_active_bytes;
	/*
	 * Max of (current_active_bytes - baseline) since
	 * last reset (not max of current alone).
	 */
	uint64_t interval_peak_active_delta;
};

struct mem_stats_info {
	const char *mem_stat_name; /* stat name */
};

/*
 * If adding any new value to gsh_mem_stats
 * structure then increment the MAX_MEMORY_STATS_FIELD_COUNT
 */
#define MAX_MEMORY_STATS_FIELD_COUNT 12

void server_stats_nfs_done(nfs_request_t *reqdata, int rc, bool dup);

#ifdef _USE_9P
void server_stats_9p_done(u8 msgtype, struct _9p_request_data *req9p);
#endif

void server_stats_io_done(size_t requested, size_t transferred, bool success,
			  bool is_write);
void server_stats_compound_done(int num_ops, int status);
void server_stats_nfsv4_op_done(int proto_op, struct timespec *start_time,
				int status);
void server_stats_transport_done(struct gsh_client *client, uint64_t rx_bytes,
				 uint64_t rx_pkt, uint64_t rx_err,
				 uint64_t tx_bytes, uint64_t tx_pkt,
				 uint64_t tx_err);

/* For delegations */
void inc_grants(struct gsh_client *client);
void dec_grants(struct gsh_client *client);
void inc_revokes(struct gsh_client *client);
void inc_recalls(struct gsh_client *client);
void inc_failed_recalls(struct gsh_client *client);

void gsh_mem_stats_update_alloc(void *p, mem_components_t comp);
void gsh_mem_stats_update_free(void *p, mem_components_t comp);
uint64_t gsh_mem_stats_get_stat_by_index_and_comp(uint32_t index,
						  mem_components_t comp);
/** comp is gshMC[] index: 0 = libntirpc, 1+ = Ganesha mem_components_t. */
const char *gsh_mem_stats_get_mem_comp_str(mem_components_t comp);
void gsh_log_mem_stats(void);
void gsh_mem_stats_reset(void);

#endif /* !SERVER_STATS_H */
/** @} */
