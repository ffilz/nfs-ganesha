/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (C) 2026, IBM
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
 */

/**
 * @file    FSAL/FSAL_URING/fsal_uring_init.c
 * @brief   Module-level init/fini for the FSAL_URING io_uring engine
 *
 * This is the first call site of fsal_uring_engine_create().  All coalesce
 * params are left at zero so defaults apply (bucket_io_count=1, no
 * coalescing), preserving existing behaviour until the params are wired to
 * the FSAL config in a follow-on task.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include "log.h"
#include "FSAL/fsal_uring.h"

/** Engine instance owned by this module. NULL until fsal_uring_module_init()
 *  succeeds, NULL again after fsal_uring_module_fini(). */
static struct fsal_uring_engine *fsal_uring_engine;

/**
 * @brief Initialise the shared io_uring engine for FSAL_URING
 *
 * Called once at module load time.  On failure the module continues without
 * io_uring: callers must tolerate a NULL engine and fall back to synchronous
 * I/O.
 *
 * @return 0 on success, -errno on failure
 */
int fsal_uring_module_init(void)
{
	/*
	 * Coalesce tunables — wire these to FSAL config in a follow-on:
	 *   params.coalesce_bucket_io_count      (default 1 = no coalescing)
	 *   params.coalesce_max_bucket_io_count  (default 64)
	 *   params.coalesce_timeout_us           (default 100 µs)
	 *   params.coalesce_outstanding_max      (default 128)
	 *   params.coalesce_cif_threshold        (default 4)
	 */
	struct fsal_uring_params params = { 0 };
	int rc;

	rc = fsal_uring_engine_create(&params, &fsal_uring_engine);
	if (rc != 0)
		LogUring(NIV_WARN,
			 "FSAL_URING: engine_create failed (%d), "
			 "falling back to synchronous I/O", rc);
	return rc;
}

/**
 * @brief Tear down the shared io_uring engine
 *
 * Called once at module unload time.  Must not be called while I/O is
 * still in flight.
 */
void fsal_uring_module_fini(void)
{
	fsal_uring_engine_destroy(fsal_uring_engine);
	fsal_uring_engine = NULL;
}

#endif /* USE_FSAL_URING */
