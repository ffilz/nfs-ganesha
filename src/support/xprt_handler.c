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

	/* It is possible that the current xprt is to be destroyed. If so, we do
	 * not want to associate such xprt to the session.
	 */
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYING) {
		PTHREAD_RWLOCK_unlock(&xprt_client_data->nfs41_session_list_lock);
		LogInfo(COMPONENT_SESSIONS,
			"Do not associate to the session the xprt FD: %d under destruction",
			xprt->xp_fd);
		gsh_free(new_entry);
		return false;
	}

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
 * This function destroys the input session's backchannel if it is up, and if
 * it uses the input xprt.
 */
static void destroy_session_backchannel_for_xprt(struct nfs41_session *session,
	SVCXPRT *xprt)
{
	char session_str[LOG_BUFF_LEN] = "\0";
	struct display_buffer db2 = {sizeof(session_str), session_str, session_str};
	display_session_id(&db2, session->session_id);

	if (!(atomic_fetch_uint32_t(&session->flags) & session_bc_up)) {
		goto no_backchannel;
	}
	PTHREAD_MUTEX_lock(&session->cb_chan.chan_mtx);

	/* After acquiring the lock, we re-check if the backchannel is available */
	if (session->cb_chan.clnt == NULL) {
		PTHREAD_MUTEX_unlock(&session->cb_chan.chan_mtx);
		goto no_backchannel;
	}
	/* Given that the backchannel is up, we first check if the session's
	 * backchannel actually uses the xprt being destroyed.
	 * The channel lock ensures that channel's client check (below) and the
	 * channel destroy operation are performed atomically.
	 */
	if (clnt_vc_get_client_xprt(session->cb_chan.clnt) != xprt) {
		PTHREAD_MUTEX_unlock(&session->cb_chan.chan_mtx);
		LogDebug(COMPONENT_XPRT, "Backchannel xprt for current session %s does "
			"not match the xprt to be destroyed. Skip destroying backchannel",
			session_str);
		return;
	}
	/* Now destroy the backchannel */
	nfs_rpc_destroy_chan_no_lock(&session->cb_chan);
	atomic_clear_uint32_t_bits(&session->flags, session_bc_up);
	PTHREAD_MUTEX_unlock(&session->cb_chan.chan_mtx);

	LogDebug(COMPONENT_XPRT, "Backchannel destroyed for current session %s",
		session_str);
	return;

 no_backchannel:
	LogDebug(COMPONENT_XPRT,
		"Backchannel is not up for the current session %s, skip destroying it",
		session_str);
}

/**
 * This function removes xprt references, both of the xprt from the client-
 * -data components, and of the client-data components from the xprt.
 *
 * This function should be called when destroying a xprt, in order to release
 * the above mentioned references.
 */
void unref_xprt_client_data(SVCXPRT *xprt)
{
	svc_xprt_client_data_t *xprt_client_data;
	struct glist_head *curr_node;
	nfs41_session_list_entry_t *curr_session;
	sockaddr_t xprt_addr;
	struct glist_head duplicate_session_list;
	nfs41_session_list_entry_t *duplicate_session_entry;
	char xprt_addr_str[LOG_BUFF_LEN / 2] = "\0";
	struct display_buffer db =
		{sizeof(xprt_addr_str), xprt_addr_str, xprt_addr_str};

	/* Convert xprt's socket address to string for logging */
	copy_xprt_addr(&xprt_addr, xprt);
	display_sockaddr(&db, &xprt_addr);

	LogDebug(COMPONENT_XPRT,
		"About to un-reference xprt client-data with FD: %d, socket-addr: %s",
		xprt->xp_fd, xprt_addr_str);

	if (!xprt->xp_u1) {
		LogInfo(COMPONENT_XPRT,
			"The xprt is not associated with any client-data, done un-referencing.");
		return;
	}
	glist_init(&duplicate_session_list);

	/* Now process the client data associated with the xprt */
	xprt_client_data = (svc_xprt_client_data_t *) xprt->xp_u1;
	PTHREAD_RWLOCK_wrlock(&xprt_client_data->nfs41_session_list_lock);

	/* Copy the xprt sessions to a new list to avoid deadlock that can happen
	 * if we take xprt's session-list lock followed by session's connections
	 * lock (this lock order is reverse of the order used during xprt connection
	 * association and dis-association with a session)
	 *
	 * With the below change, we do not acquire the nested session's connections
	 * lock, while holding the xprt's session-list lock. We first release the
	 * xprt's session-list lock after copying the xprt's sessions to a duplicate
	 * session-list. We then acquire the session's connection lock to process
	 * each session in the duplicate list. That is, both the mentioned operations
	 * are not done atomically.
	 *
	 * This can result in possible situations where the xprt's session-list has
	 * been cleared after the first operation, but those cleared sessions still
	 * have the xprt's reference, until the second operation. During this
	 * interval between the two operation, it is possible that another thread
	 * (in a different operation) sees a missing session on this xprt's
	 * session-list and tries to add it to that xprt, even though that session
	 * already had a reference to this xprt. If this situation happens, such a
	 * session added to this xprt's session-list will be at risk of never
	 * getting un-referenced. Also, such a xprt's reference would get re-added
	 * to the session, and then the xprt would be at risk of never getting
	 * destroyed. However, since this other thread additionally
	 * must also check if the xprt under consideration is being destroyed before
	 * adding the session to it, we are able to avoid this situation.
	 *
	 * The same situation can also happen after the xprt is un-referenced
	 * through this function, but another in-flight request is still operating
	 * on this same xprt (under destruction). The above mentioned check will
	 * also prevent this from happening.
	 */
	for (curr_node = xprt_client_data->nfs41_session_list.next;
		curr_node != &(xprt_client_data->nfs41_session_list);) {

		struct glist_head *next_node = curr_node->next;
		curr_session = glist_entry(curr_node, nfs41_session_list_entry_t, node);
		duplicate_session_entry = gsh_malloc(sizeof(nfs41_session_list_entry_t));
		duplicate_session_entry->session = curr_session->session;
		glist_add_tail(&duplicate_session_list, &duplicate_session_entry->node);

		/* Since we would want to add a new session reference for the duplicate
		 * list and release the existing session reference from the xprt, both
		 * would negate each other. So, we do not update the reference at all.
		 */

		/* Free the session-node allocated for the xprt */
		glist_del(curr_node);
		gsh_free(curr_session);
		curr_node = next_node;
	}
	PTHREAD_RWLOCK_unlock(&xprt_client_data->nfs41_session_list_lock);
	/* For each session referenced by the xprt, destroy the backchannel and
	 * release the connection_xprt held by the session.
	 */
	for (curr_node = duplicate_session_list.next;
		curr_node != &duplicate_session_list;) {

		struct glist_head *next_node = curr_node->next;
		curr_session = glist_entry(curr_node, nfs41_session_list_entry_t, node);

		destroy_session_backchannel_for_xprt(curr_session->session, xprt);

		remove_session_connection(curr_session->session, xprt);

		/* Release session reference */
		dec_session_ref(curr_session->session);

		/* Free the session-node allocated for the xprt */
		glist_del(curr_node);
		gsh_free(curr_session);
		curr_node = next_node;
	}
	LogDebug(COMPONENT_XPRT,
		"Completed un-referencing of the xprt with FD: %d, socket-addr: %s",
		xprt->xp_fd, xprt_addr_str);
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
