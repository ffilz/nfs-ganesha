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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

/**
 * @defgroup Client host management
 * @{
 */

/**
 * @file client_mgr.h
 * @author Jim Lieb <jlieb@panasas.com>
 * @brief Client manager
 */

#ifndef MDCACHE_MGR_H
#define MDCACHE_MGR_H

#include "gsh_types.h"

#ifdef USE_DBUS
void dbus_mdc_init(void);
#endif

#endif				/* !CLIENT_MGR_H */
/** @} */
