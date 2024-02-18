// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2024 Google LLC
 * Contributor : Yoni Couriel  yonic@google.com
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
 * @file connection_manager.c
 * @brief Allows a client to be connected to a single Ganesha server at a time.
 */

#include "connection_manager.h"

#define LogInfoClient(client, format, args...)                                 \
	LogInfo(COMPONENT_XPRT, "%s: " format,                                 \
		get_client_address_for_debugging(client), ##args)
#define LogWarnClient(client, format, args...)                                 \
	LogWarn(COMPONENT_XPRT, "%s: " format,                                 \
		get_client_address_for_debugging(client), ##args)
#define LogFatalClient(client, format, args...)                                \
	do {                                                                   \
		LogFatal(COMPONENT_XPRT, "%s: " format,                        \
			get_client_address_for_debugging(client), ##args);     \
		abort();                                                       \
	} while (0);

static inline const char *get_client_address_for_debugging(
	const connection_manager__client_t *client)
{
	// TODO: b/298325057 - Get client address from gsh_client.
	return "<unknown>";
}

void connection_manager__client_init(connection_manager__client_t *client)
{
	LogInfoClient(client, "Client init %p", client);
	client->state = CONNECTION_MANAGER__CLIENT_STATE__DRAINED;
	PTHREAD_MUTEX_init(&client->mutex, NULL);
	PTHREAD_COND_init(&client->cond_change, NULL);
	glist_init(&client->connections);
	client->connections_count = 0;
}

void connection_manager__client_fini(connection_manager__client_t *client)
{
	LogInfoClient(client, "Client fini %p", client);
	assert(client->connections_count == 0);
	assert(glist_empty(&client->connections));
	assert(client->state == CONNECTION_MANAGER__CLIENT_STATE__DRAINED);
	PTHREAD_MUTEX_destroy(&client->mutex);
	PTHREAD_COND_destroy(&client->cond_change);
}
