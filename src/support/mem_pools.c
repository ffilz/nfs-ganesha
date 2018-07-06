/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Red Hat Inc., 2018
 * Author: Frank Filz ffilzlnx@mindspring.com
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

/**
 * @file mem_pool.c
 * @author Frank Filz <ffilzlnx@mindspring.com>
 */

#include "config.h"

#include <pthread.h>
#include <assert.h>
#include "common_utils.h"
#include "log.h"
#include "abstract_mem.h"

/* Mutex to protect memory pool operations */
pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Lists of memory pools */
struct glist_head mpool_list = GLIST_HEAD_INIT(mpool_list);

/**
 * @brief Create a basic object pool
 *
 * This function creates a new object pool, given a name, object size,
 * constructor and destructor.
 *
 * This initializer function is expected to abort if it fails.
 *
 * @param[in] name             The name of this pool
 * @param[in] object_size      The size of objects to allocate
 * @param[in] file             Calling source file
 * @param[in] line             Calling source line
 * @param[in] function         Calling source function
 *
 * @return A pointer to the pool object.  This pointer must not be
 *         dereferenced.  It may be stored or supplied as an argument
 *         to the other pool functions.  It must not be supplied as an
 *         argument to gsh_free, rather it must be disposed of with
 *         pool_destroy.
 */

pool_t *pool_basic_init__(const char *name, size_t object_size,
			  const char *file, int line, const char *function)
{
	pool_t *pool = (pool_t *) gsh_calloc__(1, sizeof(pool_t), file, line,
					       function);

	pool->object_size = object_size;

	assert(name);

	pool->name = gsh_strdup__(name, file, line, function);

	PTHREAD_MUTEX_lock(&pool_mutex);
	glist_add_tail(&mpool_list, &pool->mpool_next);
	PTHREAD_MUTEX_unlock(&pool_mutex);
	return pool;
}

/**
 * @brief Destroy a memory pool
 *
 * This function destroys a memory pool.  All objects must be returned
 * to the pool before this function is called.
 *
 * @param[in] pool The pool to be destroyed.
 */

void pool_destroy(pool_t *pool)
{
	PTHREAD_MUTEX_lock(&pool_mutex);
	glist_del(&pool->mpool_next);
	PTHREAD_MUTEX_unlock(&pool_mutex);
	gsh_free(pool->name);
	gsh_free(pool);
}
