/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2021
 * Author: Bjorn Leffler leffler@google.com
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * @brief Monitoring functions for NFS Ganesha.
 *
 * Monitoring must fail gracefully.
 * Monitoring problems should not affect serving.
 */

#ifndef GANESHA_MONITORING_H
#define GANESHA_MONITORING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "gsh_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t export_id_t;

/* Allow FSALs to register a human readable label used for per-export metrics.
 * The default label (if the FSAL doesn't set one) is "exportid=<fsid_major>".
 */
void monitoring_register_export_label(export_id_t export_id, const char *label);

/* Init monitoring export at TCP port <port>. */
void monitoring_init(const uint16_t port);

/*
 * The following two functions generate the following metrics,
 * exported both as total and per export.
 *
 * - Total request count.
 * - Total request count by success / failure status.
 * - Total bytes sent.
 * - Total bytes received.
 * - Request size in bytes as histogram.
 * - Response size in bytes as histogram.
 * - Latency in ms as histogram.
 */


void monitoring__dynamic_observe_nfs_request(
			      const char *operation,
			      nsecs_elapsed_t request_time,
			      const char *version,
			      const char *status_label,
			      export_id_t export_id,
			      const char *client_ip);

void monitoring__dynamic_observe_nfs_io(
			size_t bytes_requested,
			size_t bytes_transferred,
			bool success,
			bool is_write,
			export_id_t export_id,
			const char *client_ip);

/* MDCache hit rates. */
void monitoring__dynamic_mdcache_cache_hit(
				const char *operation,
				export_id_t export_id);
void monitoring__dynamic_mdcache_cache_miss(
				const char *operation,
				export_id_t export_id);

/* In flight RPC stats. */
void monitoring_rpc_received(void);
void monitoring_rpc_completed(void);
void monitoring_rpcs_in_flight(const uint64_t value);

#ifdef __cplusplus
}
#endif

#endif   /* GANESHA_MONITORING_H */
