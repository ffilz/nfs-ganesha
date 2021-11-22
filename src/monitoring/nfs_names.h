/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2021
 * Author: Bjorn Leffler leffler@google.com
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
 * Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*
 * nfs_names.h
 * Pretty print (lower case) NFS names for monitoring.
 */

#ifndef GANESHA_MONITORING_NFS_NAMES_H
#define GANESHA_MONITORING_NFS_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "nfs23.h"  // NOLINT

const char *nfs3_proc_name(const uint32_t proc);
const char *nfs4_proc_name(const uint32_t proc);

const char *nfsstat3_name(const nfsstat3 status);
const char *nfsstat4_name(const nfsstat4 status);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GANESHA_MONITORING_NFS_NAMES_H
