/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2023 Google LLC
 * Contributor : Dipit Grover  dipit@google.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file xprt_handler.c
 * @brief This file handles functionality related to service transport.
 */

#include "xprt_handler.h"
#include "nfs_core.h"
#include "sal_functions.h"

/**
 * This function inits the xprt's `svc_xprt_client_data` struct.
 * For each xprt, it must be called during xprt client-data allocation.
 *
 * The caller must prevent concurrent access to this function for the
 * same xprt (for example, by holding the xprt-lock)
 */
void init_client_data_for_xprt(SVCXPRT *xprt)
{
	sockaddr_t addr;
	svc_xprt_client_data_t *xprt_client_data;
	char sockaddr_str[LOG_BUFF_LEN / 2] = "\0";
	struct display_buffer db =
		{sizeof(sockaddr_str), sockaddr_str, sockaddr_str};

	/* Copy the socket address from xprt and convert to string for logging */
	copy_xprt_addr(&addr, xprt);
	display_sockaddr(&db, &addr);

	if (xprt->xp_u1 != NULL) {
		LogInfo(COMPONENT_XPRT,
			"xp_u1 is already initialised for xprt with FD: %d and socket-addr: %s",
			xprt->xp_fd, sockaddr_str);
		return;
	}

	xprt_client_data = gsh_malloc(sizeof(svc_xprt_client_data_t));
	/* Todo: should only do this when serving NFS v4.1 */
	glist_init(&xprt_client_data->nfs41_session_list);
	PTHREAD_RWLOCK_init(&xprt_client_data->nfs41_session_list_lock, NULL);
	xprt->xp_u1 = (void *) xprt_client_data;

	LogInfo(COMPONENT_XPRT,
		"xp_u1 initialised for xprt with FD: %d and socket-addr: %s",
		xprt->xp_fd, sockaddr_str);
}

/**
 * This function adds the nfs41_session to the xprt session-list.
 * It also adds the reverse reference of the xprt to the nfs41_session's
 * connection-list.
 *
 * Note: The caller must hold nfs41_session->conn_lock for writes.
 */
bool associate_xprt_with_nfs41_session(SVCXPRT *xprt,
	nfs41_session_t *nfs41_session)
{
	svc_xprt_client_data_t *xprt_client_data;
	nfs41_session_list_entry_t *new_entry;

	if (xprt->xp_u1 == NULL) {
		LogCrit(COMPONENT_XPRT,
			"xprt->xp_u1 is not initialised for xprt FD: %d", xprt->xp_fd);
		return false;
	}
	xprt_client_data = (svc_xprt_client_data_t *) xprt->xp_u1;
	new_entry = gsh_malloc(sizeof(nfs41_session_list_entry_t));

	PTHREAD_RWLOCK_wrlock(&xprt_client_data->nfs41_session_list_lock);

	new_entry->session = nfs41_session;
	glist_add_tail(&xprt_client_data->nfs41_session_list, &new_entry->node);
	inc_session_ref(nfs41_session);

	/* Add the new connection-xprt and increase its ref-count */
	nfs41_session->connection_xprts[nfs41_session->num_conn++] = xprt;
	SVC_REF(xprt, SVC_REF_FLAG_NONE);

	PTHREAD_RWLOCK_unlock(&xprt_client_data->nfs41_session_list_lock);

	return true;
}

/**
 * After a xprt is destroyed, this function handles cleanup of the client
 * data associated with the xprt (if any). It is supposed to be invoked
 * after the xprt's connection is closed.
 */
void destroy_client_data_for_destroyed_xprt(SVCXPRT *xprt)
{
	sockaddr_t addr;
	svc_xprt_client_data_t *xprt_client_data;
	char sockaddr_str[LOG_BUFF_LEN / 2] = "\0";
	struct display_buffer db =
		{sizeof(sockaddr_str), sockaddr_str, sockaddr_str};

	/* Copy the socket address from xprt and convert to string for logging */
	copy_xprt_addr(&addr, xprt);
	display_sockaddr(&db, &addr);

	LogInfo(COMPONENT_XPRT, "Processing client data for destroyed xprt: %p "
		"with FD: %d, socket-addr: %s", xprt, xprt->xp_fd, sockaddr_str);

	if (!xprt->xp_u1) {
		LogInfo(COMPONENT_XPRT,
			"No client data is associated with the destroyed xprt. "
			"Nothing more to handle");
		return;
	}
	xprt_client_data = (svc_xprt_client_data_t *) xprt->xp_u1;
	PTHREAD_RWLOCK_destroy(&xprt_client_data->nfs41_session_list_lock);
	gsh_free(xprt_client_data);

	xprt->xp_u1 = NULL;
}
