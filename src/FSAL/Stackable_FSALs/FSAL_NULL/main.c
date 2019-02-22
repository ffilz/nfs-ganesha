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

/* main.c
 * Module core functions
 */

#include "config.h"

#include "fsal.h"
#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include "gsh_list.h"
#include "FSAL/fsal_init.h"
#include "nullfs_methods.h"


/* FSAL name determines name of shared library: libfsal<name>.so */
const char myname[] = "NULL";

/* Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

/* linkage to the exports and handle ops initializers
 */
MODULE_INIT void nullfs_init(void)
{
	int retval;
	struct fsal_module *myself = &NULLFS.module;

	retval = register_fsal(myself, myname, FSAL_MAJOR_VERSION,
			       FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS);
	if (retval != 0) {
		fprintf(stderr, "NULLFS module failed to register");
		return;
	}
	myself->m_ops.create_export = nullfs_create_export;
	myself->m_ops.update_export = nullfs_update_export;
	myself->m_ops.init_config = nullfs_init_config;

	/* Initialize the fsal_obj_handle ops for FSAL NULL */
	nullfs_handle_ops_init(&NULLFS.handle_ops);
}

MODULE_FINI void nullfs_unload(void)
{
	int retval;

	retval = unregister_fsal(&NULLFS.module);
	if (retval != 0) {
		fprintf(stderr, "NULLFS module failed to unregister");
		return;
	}
}
