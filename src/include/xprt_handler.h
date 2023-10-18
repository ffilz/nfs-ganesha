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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
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
 * @file xprt_handler.h
 * @brief Functionality related to service transport.
 */

#ifndef XPRT_HANDLER_H
#define XPRT_HANDLER_H

#include "gsh_rpc.h"
#include "sal_data.h"

typedef struct nfs41_session_list_entry {
	struct nfs41_session *session;
	struct glist_head node;
} nfs41_session_list_entry_t;

/* Represents miscellaneous client data related to the svc-xprt */
typedef struct svc_xprt_client_data {
	pthread_rwlock_t nfs41_session_list_lock;
	struct glist_head nfs41_session_list;
} svc_xprt_client_data_t;

void init_client_data_for_xprt(SVCXPRT *);
void destroy_client_data_for_destroyed_xprt(SVCXPRT *);

void unref_xprt_client_data(SVCXPRT *);
bool associate_xprt_with_nfs41_session(SVCXPRT *, nfs41_session_t *);

#endif				/* XPRT_HANDLER_H */
