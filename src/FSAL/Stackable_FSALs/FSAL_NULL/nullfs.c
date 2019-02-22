/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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

/* nullfs.c
 */

#include "config.h"

#include "nullfs_methods.h"


/* my module private storage
 */

struct null_fsal_module NULLFS = {
	.module = {
		.fs_info = {
			.maxfilesize = UINT64_MAX,
			.maxlink = _POSIX_LINK_MAX,
			.maxnamelen = 1024,
			.maxpathlen = 1024,
			.no_trunc = true,
			.chown_restricted = true,
			.case_insensitive = false,
			.case_preserving = true,
			.link_support = true,
			.symlink_support = true,
			.lock_support = true,
			.lock_support_async_block = false,
			.named_attr = true,
			.unique_handles = true,
			.acl_support = FSAL_ACLSUPPORT_ALLOW,
			.cansettime = true,
			.homogenous = true,
			.supported_attrs = ALL_ATTRIBUTES,
			.maxread = FSAL_MAXIOSIZE,
			.maxwrite = FSAL_MAXIOSIZE,
			.umask = 0,
			.auth_exportpath_xdev = false,
			.link_supports_permission_checks = true,
		}
	}
};

/* Module methods
 */

/* nullfs_init_config
 * must be called with a reference taken (via lookup_fsal)
 */

fsal_status_t nullfs_init_config(struct fsal_module *nullfs_fsal_module,
				 config_file_t config_struct,
				 struct config_error_type *err_type)
{
	/* Configuration setting options:
	 * 1. there are none that are changeable. (this case)
	 *
	 * 2. we set some here.  These must be independent of whatever
	 *    may be set by lower level fsals.
	 *
	 * If there is any filtering or change of parameters in the stack,
	 * this must be done in export data structures, not fsal params because
	 * a stackable could be configured above multiple fsals for multiple
	 * diverse exports.
	 */

	display_fsinfo(nullfs_fsal_module);
	LogDebug(COMPONENT_FSAL,
		 "FSAL INIT: Supported attributes mask = 0x%" PRIx64,
		 nullfs_fsal_module->fs_info.supported_attrs);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}
