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
 * @addtogroup pwnam_wrappers
 * @{
 */

/**
 * @file pwnam_wrappers.c
 * @brief pwnam wrappers
 */

#include "pwnam_wrappers.h"

int pwnam_wrappers__getgrouplist(const char *user, gid_t group, gid_t *groups,
				 int *ngroups)
{
	return getgrouplist(user, group, groups, ngroups);
}

int pwnam_wrappers__getpwnam_r(const char *name, struct passwd *pwd, char *buf,
			       size_t buflen, struct passwd **result)
{
	return getpwnam_r(name, pwd, buf, buflen, result);
}

int pwnam_wrappers__getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
			       size_t buflen, struct passwd **result)
{
	return getpwuid_r(uid, pwd, buf, buflen, result);
}

int pwnam_wrappers__getgrnam_r(const char *name, struct group *grp, char *buf,
			       size_t buflen, struct group **result)
{
	return getgrnam_r(name, grp, buf, buflen, result);
}

int pwnam_wrappers__getgrgid_r(gid_t gid, struct group *grp, char *buf,
			       size_t buflen, struct group **result)
{
	return getgrgid_r(gid, grp, buf, buflen, result);
}
