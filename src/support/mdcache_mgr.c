/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2013
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 * Copyright (C) Bigtera Inc., 2022
 *
 * contributor : Vicente Cheng	<vicente_cheng@bigtera.com>
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
 * -------------
 */

/**
 * @defgroup clntmmt Client management
 * @{
 */

/**
 * @file client_mgr.c
 * @author Jim Lieb <jlieb@panasas.com>
 * @brief Protocol client manager
 */

#include "config.h"

#include <time.h>
#include <assert.h>
#include "fsal.h"
#include "log.h"
#ifdef USE_DBUS
#include "gsh_dbus.h"
#endif
#include "mdcache_mgr.h"
#include "server_stats_private.h"

#ifdef USE_DBUS

/* DBUS interface(s)
 */

/* org.ganesha.nfsd.mdcstats interface
 */

/**
 * DBUS method to show general MDCache statistics
 *
 */

static bool show_mdc_reclaim_detail(DBusMessageIter *args,
				    DBusMessage *reply,
				    DBusError *error)

{
	bool success = true;
	char *errormsg = "OK";
	DBusMessageIter iter;
	struct timespec timestamp;

	now(&timestamp);
	dbus_message_iter_init_append(reply, &iter);
	gsh_dbus_status_reply(&iter, success, errormsg);
	gsh_dbus_append_timestamp(&iter, &timestamp);

	mdcache_lru_reclaim_status(&iter);
	return true;
}

static struct gsh_dbus_method mdc_show_reclaim_detail = {
	.name = "ShowMDCacheReclaimDetail",
	.method = show_mdc_reclaim_detail,
	.args = {STATUS_REPLY,
		TIMESTAMP_REPLY,
		TOTAL_OPS_REPLY,
		LRU_UTILIZATION_REPLY,
		END_ARG_LIST}
};

static bool show_mdc_general(DBusMessageIter *args,
			       DBusMessage *reply,
			       DBusError *error)
{
	bool success = true;
	char *errormsg = "OK";
	DBusMessageIter iter;
	struct timespec timestamp;

	now(&timestamp);
	dbus_message_iter_init_append(reply, &iter);
	gsh_dbus_status_reply(&iter, success, errormsg);
	gsh_dbus_append_timestamp(&iter, &timestamp);

	mdcache_dbus_show(&iter);
	mdcache_utilization(&iter);

	return true;
}


static struct gsh_dbus_method mdc_show_general = {
	.name = "ShowMDCacheGeneral",
	.method = show_mdc_general,
	.args = {STATUS_REPLY,
		 TIMESTAMP_REPLY,
		 TOTAL_OPS_REPLY,
		 LRU_UTILIZATION_REPLY,
		 END_ARG_LIST}
};

static struct gsh_dbus_method *mdcache_stats_methods[] = {
	&mdc_show_general,
	&mdc_show_reclaim_detail,
	NULL
};

static struct gsh_dbus_interface mdcache_statistics = {
	.name = "org.ganesha.nfsd.mdcstats",
	.props = NULL,
	.methods = mdcache_stats_methods,
	.signals = NULL
};

/* DBUS list of interfaces on /org/ganesha/nfsd/MDCMgr
 */

static struct gsh_dbus_interface *mdcmgr_interfaces[] = {
	&mdcache_statistics,
	NULL
};

void dbus_mdc_init(void)
{
	gsh_dbus_register_path("MDCMgr", mdcmgr_interfaces);
}

#endif				/* USE_DBUS */

/** @} */
