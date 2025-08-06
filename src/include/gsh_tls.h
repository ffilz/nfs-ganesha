// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 *
 * Copyright (C) 2025, IBM . All rights reserved.
 * Author: Deeraj Patil <deeraj.patil@ibm.com>
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
 * 02110-1301 USA.  see <http://www.gnu.org/licenses/
 *
 * ---------------------------------------
 */

/**
 * @file gsh_tls.h
 * @brief TLS configuration structures and
 * function declarations for NFS-Ganesha
 */
#ifndef GSH_TLS_H
#define GSH_TLS_H

#ifdef USE_TLS
/* TLS configuration structure */
typedef struct gsh_tls_config {
	bool enabled;
	char *cert_file;
	char *key_file;
	char *ca_file;
	char *ciphers;
	char *min_version;
	time_t session_timeout;
	bool ktls;          /* Enable kernel TLS if available */
	bool debug;          /* for enabling debug */
} gsh_tls_config_t;

extern gsh_tls_config_t tls_config;
extern struct config_block tls_core;
extern bool nfs_init_tls(gsh_tls_config_t from_ganesha);
#endif /* USE_TLS */

#endif /* GSH_TLS_H */
