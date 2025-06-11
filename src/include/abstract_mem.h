/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 *
 *
 * Copyright © Linux box Corporation, 2012
 * Author: Adam C. Emerson <aemerson@linuxbox.com>
 *
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
 * @file   abstract_mem.h
 * @author Adam C. Emerson <aemerson@linuxbox.com>
 * @brief  Abstract memory shims to allow swapping out allocators
 *
 * This file's purpose is to allow us to easily replace the memory
 * allocator used by Ganesha.  Further, it provides a pool abstraction
 * that may be implemented in terms of the normal allocator that may
 * be expanded at a later date.  These are intended to be thin
 * wrappers, but conditionally compiled trace information could be
 * added.
 */

#ifndef ABSTRACT_MEM_H
#define ABSTRACT_MEM_H

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "log.h"
#include <malloc.h>

/* Memory Statistics Structures */
typedef enum {
	MEM_COMP_INIT,			/* Startup, config, exports, recovery */
	MEM_COMP_FSAL,			/* FSAL & filesystem semantics */
	MEM_COMP_PROTOCOL,		/* NFS / NLM / 9P / NFS4 / NFS3 */
	MEM_COMP_FILE_AND_STATE_LOCK,	/* State and lock owners, delegations */
	MEM_COMP_IO_BUFFER,		/* READ / WRITE data buffers only */
	MEM_COMP_SESSION,		/* Sessions, connections, transport */
	MEM_COMP_CLIENT,		/* Client identity, clientid, creds */
	MEM_COMP_CACHE,			/* MDCACHE */
	MEM_COMP_LIBNTIRPC,		/* LIBNTIRPC */
	MEM_COMP_DIGEST_POOL,		/* Digest Pool */
	MEM_COMP_HANDLE_POOL,		/* Handle Pool */
	MEM_COMP_DB_THREAD_POOL,	/* DB Thread Pool */
	MEM_COMP_DROP_POOL,		/* Drop Pool */
	MEM_COMP_MDCACHE_POOL,		/* MDCACHE Pool */
	MEM_COMP_NFSV4_SESSION_POOL,	/* NFSV4 Session Pool */
	MEM_COMP_DUP_REQ_POOL,		/* Dup Request Pool */
	MEM_COMP_NFS_RES_POOL,		/* NFS Res Pool */
	MEM_COMP_TCP_DRC_POOL,		/* TCP and DRC Pool */
	MEM_COMP_NFS4_CLIENT_ID_POOL,	/* Client ID Pool */
	MEM_COMP_NFS4_STATE_OWNER_POOL,	/* State Owner Pool */
	MEM_COMP_NODE_POOL,		/* Node Pool */
	MEM_COMP_DATA_POOL,		/* DATA Pool */
	MEM_COMP_ACL_POOL,		/* ACL Pool */
	MEM_COMP_MISC,			/* Utilities, glue, logging, Strings */
	MEM_COMP_GTEST,			/* Allocation and Free in GTEST only */
	MEM_COMP_MAX
} mem_components_t;

struct mem_component_info {
	const char *mem_comp_name; /* component name */
	const char *mem_comp_str; /* shorter, more useful name */
};

/* Memory Statistics API's */
extern void gsh_mem_stats_update_alloc(void *p, mem_components_t comp);
extern void gsh_mem_stats_update_free(void *p, mem_components_t comp);

#define GSH_MEM_STATS_UPDATE_ALLOC(p, comp) gsh_mem_stats_update_alloc(p, comp)
#define GSH_MEM_STATS_UPDATE_FREE(p, comp) gsh_mem_stats_update_free(p, comp)

/**
 * @page GeneralAllocator General Allocator Shim
 *
 * These functions provide an interface akin to the standard libc
 * allocation functions.  Currently they call the functions malloc,
 * free, and so forth, with changes in functionality being provided by
 * linking in alternate allocator libraries (tcmalloc and jemalloc, at
 * present.)  So long as the interface remains the same, these
 * functions can be switched out using ifdef for versions that do more
 * memory tracking or that call allocators with other names.
 */

/**
 * @brief Allocate memory
 *
 * This function allocates a block of memory no less than the given
 * size. The block of memory allocated must be released with gsh_free.
 *
 * This function aborts if no memory is available.
 *
 * @param[in] n Number of bytes to allocate
 * @param[in] file Calling source file
 * @param[in] line Calling source line
 * @param[in] function Calling source function
 *
 * @return Pointer to a block of memory.
 */
static inline void *gsh_malloc__(size_t n, uint8_t comp, const char *file,
				 int line, const char *function)
{
	void *p = malloc(n);

	if (p == NULL) {
		LogMallocFailure(file, line, function, "gsh_malloc");
		abort();
	}

	GSH_MEM_STATS_UPDATE_ALLOC(p, comp);

	return p;
}

#define gsh_malloc(n, comp)                               \
	({                                                \
		void *p_ = malloc(n);                     \
		if (p_ == NULL) {                         \
			abort();                          \
		}                                         \
		GSH_MEM_STATS_UPDATE_ALLOC(p_, comp);     \
		p_;                                       \
	})

/**
 * @brief Allocate aligned memory
 *
 * This function allocates a block of memory to the given alignment.
 * Failure may indicate either insufficient memory or an invalid
 * alignment.
 *
 * @param[in] a Block alignment
 * @param[in] n Number of bytes to allocate
 * @param[in] file Calling source file
 * @param[in] line Calling source line
 * @param[in] function Calling source function
 *
 * @return Pointer to a block of memory or NULL.
 */
static inline void *gsh_malloc_aligned__(size_t a, size_t n, uint8_t comp,
					 const char *file, int line,
					 const char *function)
{
	void *p;

#ifdef __APPLE__
	p = valloc(n);
#else
	if (posix_memalign(&p, a, n) != 0)
		p = NULL;
#endif
	if (p == NULL) {
		LogMallocFailure(file, line, function, "gsh_malloc_aligned");
		abort();
	}

	GSH_MEM_STATS_UPDATE_ALLOC(p, comp);

	return p;
}

#define gsh_malloc_aligned(a, n, comp)                    \
	({                                                \
		void *p_;                                 \
		if (posix_memalign(&p_, a, n) != 0) {     \
			abort();                          \
		}                                         \
		GSH_MEM_STATS_UPDATE_ALLOC(p_, comp);     \
		p_;                                       \
	})

/**
 * @brief Allocate zeroed memory
 *
 * This function allocates a block of memory that is guaranteed to be
 * zeroed. The block of memory allocated must be released with gsh_free.
 *
 * This function aborts if no memory is available.
 *
 * @param[in] n Number of objects in block
 * @param[in] s Size of object
 *
 * @return Pointer to a block of zeroed memory.
 */
static inline void *gsh_calloc__(size_t n, size_t s, uint8_t comp,
				 const char *file, int line,
				 const char *function)
{
	void *p = calloc(n, s);

	if (p == NULL) {
		LogMallocFailure(file, line, function, "gsh_calloc");
		abort();
	}

	GSH_MEM_STATS_UPDATE_ALLOC(p, comp);

	return p;
}

#define gsh_calloc(n, s, comp)                            \
	({                                                \
		void *p_ = calloc(n, s);                  \
		if (p_ == NULL) {                         \
			abort();                          \
		}                                         \
		GSH_MEM_STATS_UPDATE_ALLOC(p_, comp);     \
		p_;                                       \
	})

/**
 * @brief Resize a block of memory
 *
 * This function resizes the buffer indicated by the supplied pointer
 * to the given size.  The block may be moved in this process.  On
 * failure, the original block is retained at its original address.
 *
 * This function aborts if no memory is available to resize.
 *
 * @param[in] p Block of memory to resize
 * @param[in] n New size
 * @param[in] file Calling source file
 * @param[in] line Calling source line
 * @param[in] function Calling source function
 *
 * @return Pointer to the address of the resized block.
 */
static inline void *gsh_realloc__(void *p, size_t n, uint8_t comp,
				  const char *file, int line,
				  const char *function)
{
	GSH_MEM_STATS_UPDATE_FREE(p, comp);

	void *p2 = realloc(p, n);

	if (n != 0 && p2 == NULL) {
		LogMallocFailure(file, line, function, "gsh_realloc");
		abort();
	}

	GSH_MEM_STATS_UPDATE_ALLOC(p2, comp);

	return p2;
}

#define gsh_realloc(p, n, comp)                            \
	({                                                 \
		GSH_MEM_STATS_UPDATE_FREE(p, comp);        \
		void *p2_ = realloc(p, n);                 \
		if ((n) != 0 && p2_ == NULL) {             \
			abort();                           \
		}                                          \
		GSH_MEM_STATS_UPDATE_ALLOC(p2_, comp);     \
		p2_;                                       \
	})

#define gsh_strdup(s)                                        \
	({                                                   \
		const char *_src_ = (s);                     \
		size_t _len_ = strlen(_src_) + 1;            \
		char *p_ = gsh_malloc(_len_, MEM_COMP_MISC); \
		if (p_ == NULL) {                            \
			abort();                             \
		}                                            \
		memcpy(p_, _src_, _len_);                    \
		p_;                                          \
	})

#define gsh_strldup(s, l, n)                                         \
	({                                                           \
		char *p_ = (char *)gsh_malloc(l + 1, MEM_COMP_MISC); \
		memcpy(p_, s, l);                                    \
		p_[l] = '\0';                                        \
		*n = l + 1;                                          \
		p_;                                                  \
	})

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
#define gsh_strdupa(src) strdupa(src)
#else
#define gsh_strdupa(src)                              \
	({                                            \
		char *dest = alloca(strlen(src) + 1); \
		strcpy(dest, src);                    \
		dest;                                 \
	})
#endif

#define gsh_memdup(s, l)                                 \
	({                                               \
		void *p_ = gsh_malloc(l, MEM_COMP_MISC); \
		memcpy(p_, s, l);                        \
		p_;                                      \
	})

/**
 * @brief Free a block of memory
 *
 * This function frees a block of memory allocated with gsh_malloc,
 * gsh_malloc_aligned, gsh_calloc, gsh_realloc, or gsh_strdup.
 *
 * @param[in] p Block of memory to free.
 */
static inline void gsh_free(void *p, mem_components_t comp)
{
	GSH_MEM_STATS_UPDATE_FREE(p, comp);

	free(p);
}

/**
 * @brief Free a block of memory with size
 *
 * This function exists to be passed to TIRPC when setting
 * allocators.  It should not be used by anyone else.  New shim layers
 * should not redefine it.
 *
 * @param[in] p  Block of memory to free.
 * @param[in] n  Size of block (unused)
 */
static inline void gsh_free_size(void *p, size_t n __attribute__((unused)),
				 uint8_t comp)
{
	GSH_MEM_STATS_UPDATE_FREE(p, comp);

	free(p);
}

/**
 * @brief Type representing a pool
 *
 * This type represents a memory pool.  it should be treated, by all
 * callers, as a completely abstract type.  The pointer should only be
 * stored or passed to pool functions.  The pointer should never be
 * referenced.  No assumptions about the size of the pointed-to type
 * should be made.
 *
 * This allows for flexible growth in the future.
 */

typedef struct pool {
	char *name; /*< The name of the pool */
	size_t object_size; /*< The size of the objects created */
} pool_t;

/**
 * @brief Create a basic object pool
 *
 * This function creates a new object pool, given a name, object size,
 * constructor and destructor.
 *
 * This particular implementation throws the name away, but other
 * implementations that do tracking or keep counts of allocated or
 * de-allocated objects will likely wish to use it in log messages.
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
 *         to the other pool functions.
 */

static inline pool_t *pool_basic_init(const char *name, size_t object_size,
				      mem_components_t comp)
{
	pool_t *pool = (pool_t *)gsh_malloc(sizeof(pool_t), comp);

	pool->object_size = object_size;

	if (name)
		pool->name = gsh_strdup(name);
	else
		pool->name = NULL;

	return pool;
}

static inline char *gsh_concat(const char *p1, const char *p2)
{
	size_t len1 = strlen(p1);
	size_t len2 = strlen(p2);
	char *path = (char *)gsh_malloc(len1 + len2 + 1, MEM_COMP_MISC);

	memcpy(path, p1, len1);
	memcpy(path + len1, p2, len2 + 1);

	return path;
}

static inline char *gsh_concat_sep(const char *p1, char sep, const char *p2)
{
	size_t len1 = strlen(p1);
	size_t len2 = strlen(p2);
	char *path = (char *)gsh_malloc(len1 + 1 + len2 + 1, MEM_COMP_MISC);

	memcpy(path, p1, len1);
	path[len1] = sep;
	memcpy(path + len1 + 1, p2, len2 + 1);

	return path;
}

#endif /* ABSTRACT_MEM_H */
