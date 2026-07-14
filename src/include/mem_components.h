/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 *
 * Copyright (C) 2026, IBM . All rights reserved.
 * Author: Nishant Puri <Nishant.Puri1@ibm.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file mem_components.h
 * @brief Memory component enum — kept in its own header so that files
 *        included early in the chain (e.g. ip_utils.h via log.h) can
 *        reference mem_components_t without pulling in all of abstract_mem.h.
 *
 * No other Ganesha headers are included here; only <rpc/types.h> for the
 * NTIRPC_MEM_COMP_* constants that anchor the libntirpc slots.
 */

#ifndef MEM_COMPONENTS_H
#define MEM_COMPONENTS_H

#include "abstract_atomic.h"
#include <rpc/types.h>

/*
 * Memory Component List.
 */
typedef enum {
	MEM_COMP_LIBNTIRPC,
	MEM_COMP_CONFIG, /* ganesha.conf, parser */
	MEM_COMP_EXPORT, /* export list, per-export stats, export-related SAL */
	MEM_COMP_FSAL, /* FSAL & filesystem semantics */
	MEM_COMP_PROTOCOL, /* NFS / NLM / 9P / NFS4 / NFS3 */
	MEM_COMP_FILE_AND_STATE_LOCK, /* State and lock owners, delegations */
	MEM_COMP_IO_BUFFER, /* READ / WRITE data buffers only */
	MEM_COMP_CLIENT, /* Client identity, clientid, connections, sessions */
	MEM_COMP_MDCACHE, /* MDCACHE */
	MEM_COMP_DIGEST_POOL, /* Digest Pool */
	MEM_COMP_HANDLE_POOL, /* Handle Pool */
	MEM_COMP_DB_THREAD_POOL, /* DB Thread Pool */
	MEM_COMP_DROP_POOL, /* Drop Pool */
	MEM_COMP_DUP_REQ_POOL, /* Dup Request Pool */
	MEM_COMP_NFS_RES_POOL, /* NFS Res Pool */
	MEM_COMP_TCP_DRC_POOL, /* TCP and DRC Pool */
	MEM_COMP_NFS4_STATE_OWNER_POOL, /* State Owner Pool */
	MEM_COMP_NODE_POOL, /* Node Pool */
	MEM_COMP_DATA_POOL, /* DATA Pool */
	MEM_COMP_ACL_POOL, /* ACL Pool */
	MEM_COMP_ACE, /* ACE array */
	MEM_COMP_RECOVERY, /* NFS recovery state (clid_entry, rdel_fh) */
	MEM_COMP_QOS, /* QoS rate-control */
	MEM_COMP_DBUS, /* D-Bus handlers, broadcast items */
	MEM_COMP_MISC, /* Utilities, glue, logging, Strings */
	MEM_COMP_GTEST, /* Allocation and Free in GTEST only */
	MEM_COMP_MAX
} mem_components_t;

#endif /* MEM_COMPONENTS_H */
