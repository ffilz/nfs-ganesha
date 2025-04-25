// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Lior Suliman   liorsu@google.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
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
 * @defgroup pwnam_wrappers pwnam wrappers
 *
 * The pwnam wrappers module provides the pwnam wrapping functions in order
 * to allow alternative implementations to the libc pwnam function.
 *
 * @{
 */

/**
 * @file pwnam_wrappers.c
 * @brief Id mapping functions wrappers
 */

#ifndef PWNAM_WRAPPERS_H
#define PWNAM_WRAPPERS_H

#include <sys/types.h>
#include <grp.h>
#include <pwd.h>

typedef enum pwnam_implementation {
	PWNAM_IMPLEMENTATION__NSSWITCH = 0,
	PWNAM_IMPLEMENTATION__SSSD,
} pwnam_implementation_t;

int pwnam_wrappers__set_implementation(pwnam_implementation_t);

int pwnam_wrappers__getgrouplist(const char *user, gid_t group, gid_t *groups,
				 int *ngroups);

int pwnam_wrappers__getpwnam_r(const char *name, struct passwd *pwd, char *buf,
			       size_t buflen, struct passwd **result);

int pwnam_wrappers__getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
			       size_t buflen, struct passwd **result);

int pwnam_wrappers__getgrnam_r(const char *name, struct group *grp, char *buf,
			       size_t buflen, struct group **result);

int pwnam_wrappers__getgrgid_r(gid_t gid, struct group *grp, char *buf,
			       size_t buflen, struct group **result);

#endif /* PWNAM_WRAPPERS_H */
/** @} */
