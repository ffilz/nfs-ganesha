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
#include "client_mgr.h"

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
	const struct gsh_client *const gsh_client = container_of(
		client, struct gsh_client, connection_manager);
	return gsh_client->hostaddr_str;
}

static enum connection_manager__drain_t callback_default_drain_other_servers(
	void *context,
	const sockaddr_t *client_address,
	const char *client_address_str,
	const struct timespec *timeout)
{
	static bool first_time = true;
	if (first_time) {
		first_time = false;
		LogWarn(COMPONENT_XPRT,
			"%s: Connection manager is enabled but missing drain callback",
			client_address_str);
	}
	return CONNECTION_MANAGER__DRAIN__SUCCESS_NO_CONNECTIONS;
}

static pthread_rwlock_t callback_lock = RWLOCK_INITIALIZER;
static const connection_manager__callback_context_t callback_default = {
	/*user_context=*/NULL, callback_default_drain_other_servers};
static connection_manager__callback_context_t callback_context =
	callback_default;

void connection_manager__callback_set(
	connection_manager__callback_context_t new)
{
	PTHREAD_RWLOCK_wrlock(&callback_lock);
	assert(callback_context.drain_and_disconnect_other_servers ==
	       callback_default.drain_and_disconnect_other_servers);
	callback_context = new;
	PTHREAD_RWLOCK_unlock(&callback_lock);
}

connection_manager__callback_context_t connection_manager__callback_clear(void)
{
	PTHREAD_RWLOCK_wrlock(&callback_lock);
	assert(callback_context.drain_and_disconnect_other_servers !=
	       callback_default.drain_and_disconnect_other_servers);
	const connection_manager__callback_context_t old = callback_context;
	callback_context = callback_default;
	PTHREAD_RWLOCK_unlock(&callback_lock);
	return old;
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
