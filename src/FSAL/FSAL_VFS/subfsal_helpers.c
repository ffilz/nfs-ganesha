/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2017 VMware, Inc.
 * Author: Sriram Patil sriramp@vmware.com
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include "fsal_api.h"
#include "vfs_methods.h"
#include "fsal_types.h"
#include "FSAL/fsal_commonlib.h"
#include "fsal_convert.h"
#include "nfs_core.h"
#include "nfs_proto_tools.h"

fsal_status_t vfs_get_fs_locations(struct vfs_fsal_obj_handle *hdl,
				   struct attrlist *attrs_out)
{
	char *xattr_content;
	size_t attrsize = 0;
	fsal_status_t st;

	/* referral configuration is in a xattr "user.fs_location"
	 * on the directory in the form
	 * server:/path/to/referred/directory.
	 * It gets storeded in attrs_out->fs_locations->locations
	 */
	xattr_content = gsh_calloc(XATTR_BUFFERSIZE, sizeof(char));

	st = vfs_getextattr_value_by_name((struct fsal_obj_handle *)hdl,
			"user.fs_location",
			xattr_content,
			XATTR_BUFFERSIZE,
			&attrsize);

	if (!FSAL_IS_ERROR(st)) {
		char *path = xattr_content;
		char *server = strsep(&path, ":");

		LogDebug(COMPONENT_FSAL, "user.fs_location: %s",
				xattr_content);

		nfs4_fs_locations_release(attrs_out->fs_locations);

		attrs_out->fs_locations = nfs4_fs_locations_new(server,
								path);
		FSAL_SET_MASK(attrs_out->valid_mask, ATTR4_FS_LOCATIONS);
	}
	gsh_free(xattr_content);

	return st;
}
