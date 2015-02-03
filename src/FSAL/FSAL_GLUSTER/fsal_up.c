/*
 * Copyright (C) Red Hat  Inc., 2015
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
 * @file    fsal_up.c
 *
 * @author  Soumya Koduri <skoduri@redhat.com>
 *
 * @brief   Upcall Interface for FSAL_GLUSTER
 *
 */

#include "config.h"

#include "fsal.h"
#include "fsal_up.h"
#include "gluster_internal.h"
#include "fsal_convert.h"
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <sys/time.h>

void *GLUSTERFSAL_UP_Thread(void *Arg)
{
	struct glusterfs_export     *glfsexport                 = Arg;
	const struct fsal_up_vector *event_func;
	char                        thr_name[16];
	int                         rc                          = 0;
	struct callback_arg         callback;
	unsigned char               globjhdl[GLAPI_HANDLE_LENGTH];
	int                         reason                      = 0;
	int                         flags                       = 0;
	int                         retry                       = 0;
	struct gsh_buffdesc         key;
	uint32_t                    expire_time_attr            = 0;
	uint32_t                    upflags                     = 0;
	int                         errsv                       = 0;
	char                        vol_uuid[GLAPI_UUID_LENGTH] = {'\0'};


	snprintf(thr_name, sizeof(thr_name),
		 "fsal_up_%p",
		 glfsexport->gl_fs);
	SetNameFunction(thr_name);

	/* Set the FSAL UP functions that will be used to process events. */
	event_func = glfsexport->export.up_ops;

	if (event_func == NULL) {
		LogFatal(COMPONENT_FSAL_UP,
			 "FSAL up vector does not exist. Can not continue.");
		gsh_free(Arg);
		return NULL;
	}

	LogFullDebug(COMPONENT_FSAL_UP,
		     "Initializing FSAL Callback context for %p.",
		     glfsexport->gl_fs);

	if (!glfsexport->gl_fs)
		goto out;

	callback.fs = glfsexport->gl_fs;
	expire_time_attr = cache_param.expire_time_attr;

	/* Start querying for events and processing. */
	while (!glfsexport->destroy_mode) {
		LogFullDebug(COMPONENT_FSAL_UP,
			     "Requesting event from FSAL Callback interface for %p.",
			     glfsexport->gl_fs);

		rc = glfs_h_poll_upcall(glfsexport->gl_fs, &callback);
		errsv = errno;
		reason = callback.reason;
		flags = callback.flags;
		expire_time_attr = callback.expire_time_attr;

		if (rc != 0) {
			if (rc == ENOSYS) {
				LogFatal(COMPONENT_FSAL_UP,
					 "GLUSTERFS was not found, rc ENOSYS");
				return NULL;
			}
			LogCrit(COMPONENT_FSAL_UP,
				"OPENHANDLE_INODE_UPDATE failed for %p."
				" rc %d errno %d (%s) reason %d",
				glfsexport->gl_fs, rc, errsv,
				strerror(errsv), reason);

			rc = -(rc);
			if (retry < 1000) {
				retry++;
				continue;
			}

			if (errsv == EUNATCH)
				LogFatal(COMPONENT_FSAL_UP,
					 "GLUSTERFS file system %p has gone away.",
					 glfsexport->gl_fs);

			continue;
		}

		retry = 0;

		LogDebug(COMPONENT_FSAL_UP,
			 "inode update: rc %d reason %d update ino %ld flags:%x",
			 rc, reason, callback.buf.st_ino, flags);

		LogFullDebug(COMPONENT_FSAL_UP,
			     "inode update: flags:%x, callback.handle:%p, "
			     "expire: %d",
			     (callback.flags), callback.object,
			     expire_time_attr);

		if (!callback.object)
			continue;

		rc = glfs_h_extract_handle(callback.object,
					   globjhdl+GLAPI_UUID_LENGTH,
					   GFAPI_HANDLE_LENGTH);
		if (rc < 0) {
			LogDebug(COMPONENT_FSAL_UP,
				 "glfs_h_extract_handle failed %p",
				 glfsexport->gl_fs);
			continue;
		}
		rc = glfs_get_volumeid(callback.fs, vol_uuid,
				       GLAPI_UUID_LENGTH);
		if (rc < 0) {
			LogDebug(COMPONENT_FSAL_UP,
				 "glfs_get_volumeid failed %p",
				 glfsexport->gl_fs);
			continue;
		}
		memcpy(globjhdl, vol_uuid, GLAPI_UUID_LENGTH);
		key.addr = &globjhdl;
		key.len = GLAPI_HANDLE_LENGTH;

		LogDebug(COMPONENT_FSAL_UP, "Received event to process for %p",
			 glfsexport->gl_fs);


		/* Decide what type of event this is
		 * inode update / invalidate? */
		switch (reason) {
		case INODE_UPDATE:	/* Update Event */
		{
				struct attrlist attr;

				LogFullDebug(COMPONENT_FSAL_UP,
					    "inode update: flags:%x update ino"
					    " %ld n_link:%d",
					    flags, callback.buf.st_ino,
					    (int)callback.buf.st_nlink);

				/* Check for accepted flags, any other changes
				   just invalidate. */
				if (flags &
				    (UP_SIZE | UP_NLINK | UP_MODE | UP_OWN |
				     UP_TIMES | UP_ATIME)) {
					posix2fsal_attributes(&callback.buf,
							      &attr);
					attr.mask = 0;
					if (flags & UP_SIZE)
						attr.mask |=
						   ATTR_CHGTIME | ATTR_CHANGE |
						   ATTR_SIZE | ATTR_SPACEUSED;
					if (flags & UP_MODE)
						attr.mask |=
						   ATTR_CHGTIME | ATTR_CHANGE |
						   ATTR_MODE;
					if (flags & UP_OWN)
						attr.mask |=
						   ATTR_CHGTIME | ATTR_CHANGE |
						   ATTR_OWNER;
					if (flags & UP_TIMES)
						attr.mask |=
						   ATTR_CHGTIME | ATTR_CHANGE |
						   ATTR_ATIME | ATTR_CTIME |
						    ATTR_MTIME;
					if (flags & UP_ATIME)
						attr.mask |=
						   ATTR_CHGTIME | ATTR_CHANGE |
						   ATTR_ATIME;

					attr.expire_time_attr =
					    expire_time_attr;

					rc = event_func->
					    update(glfsexport->export.fsal,
						   &key, &attr, upflags);

					if ((flags & UP_NLINK)
					    && (attr.numlinks == 0)) {
						upflags = fsal_up_nlink;
						attr.mask = 0;
						rc = up_async_update
						    (general_fridge,
						     event_func,
						     glfsexport->export.fsal,
						     &key, &attr,
						     upflags, NULL, NULL);
					}
				} else {
					rc = event_func->
					    invalidate(
						glfsexport->export.fsal, &key,
						CACHE_INODE_INVALIDATE_ATTRS
						|
						CACHE_INODE_INVALIDATE_CONTENT);
				}

		}
		break;

		case INODE_INVALIDATE:
		{
			LogFullDebug(COMPONENT_FSAL_UP,
				    "inode invalidate: flags:%x update ino %ld",
				    flags, callback.buf.st_ino);

			upflags = CACHE_INODE_INVALIDATE_ATTRS |
				  CACHE_INODE_INVALIDATE_CONTENT;
			rc = event_func->invalidate_close(
						glfsexport->export.fsal,
						event_func,
						&key,
						upflags);
		}
		break;
		default:
			LogWarn(COMPONENT_FSAL_UP, "Unknown event: %d", reason);
			continue;
		}

		if (rc && rc != CACHE_INODE_NOT_FOUND) {
			LogWarn(COMPONENT_FSAL_UP,
				"Event %d could not be processed for fd %p, rc %d",
				reason, glfsexport->gl_fs, rc);
		}
		glfs_h_close(callback.object);
	}

out:
	return NULL;
}				/* GLUSTERFSFSAL_UP_Thread */
