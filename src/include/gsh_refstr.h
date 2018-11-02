/**
 * Copyright (c) 2018 Jeff Layton <jlayton@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 as
 * published by the Free  Software Foundation.
 */
#ifndef _GSH_REFSTR_H
#define _GSH_REFSTR_H
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <urcu/ref.h>

/**
 * @brief Refcounted strings
 *
 * This struct contains an atomic refcount and a flexarray intended to hold a
 * NULL terminated string. They are allocated via gsh_refstr_alloc, and then
 * users can acquire and release references to them via gsh_refstr_get and
 * gsh_refstr_put.
 */
struct gsh_refstr {
	struct urcu_ref	gr_ref;		/* refcount */
	char		gr_val[];	/* buffer */
};

/**
 * @brief allocate a new gsh_refstr
 *
 * Allocate a new gsh_refstr with a gr_val buffer of the given length.
 *
 * Note that if allocating for a string, ensure that you pass in a length that
 * includes the NULL byte.
 *
 * @param[in]	len	Length of the embedded buffer
 */
struct gsh_refstr *gsh_refstr_alloc(size_t len);

/**
 * @brief free the given gsh_refstr
 *
 * A callback function that the refcounting code can use to free a gsh_refstr.
 *
 * @param[in]	pointer to the gr_ref field in the structure
 */
void gsh_refstr_release(struct urcu_ref *ref);

#ifndef HAVE_URCU_REF_GET_UNLESS_ZERO
/*
 * This function was copied directly from liburcu.
 *
 * Older versions of liburcu do not have this function, so we provide it here
 * for now. Eventually once all supported distros catch up, we should be able
 * to remove this inline and just rely on the one in liburcu.
 */

/*
 * urcu_ref_get_unless_zero
 *
 * Allows getting a reference atomically if the reference count is not
 * zero. Returns true if the reference is taken, false otherwise. This
 * needs to be used in conjunction with another synchronization
 * technique (e.g.  RCU or mutex) to ensure existence of the reference
 * count. False is also returned in case incrementing the refcount would
 * result in an overflow.
 */
static inline bool urcu_ref_get_unless_zero(struct urcu_ref *ref)
{
	long old, _new, res;

	old = uatomic_read(&ref->refcount);
	for (;;) {
		if (old == 0 || old == LONG_MAX)
			return false;	/* Failure. */
		_new = old + 1;
		res = uatomic_cmpxchg(&ref->refcount, old, _new);
		if (res == old) {
			return true;	/* Success. */
		}
		old = res;
	}
}
#endif

/**
 * @brief acquire a reference to the given gsh_refstr
 *
 * This is only safe to use when we know that the refcount is not zero. The
 * typical use it to use rcu_dereference to fetch an rcu-managed pointer
 * and use this function to take a reference to it inside of the rcu_read_lock.
 *
 * Returns the same pointer passed in (for convenience).
 *
 * @param[in]	gr	Pointer to gsh_refstr
 */
static inline struct gsh_refstr *gsh_refstr_get(struct gsh_refstr *gr)
{
	/*
	 * The assumption is that the persistent reference to the object is
	 * only put after an RCU grace period has settled.
	 */
	if (!urcu_ref_get_unless_zero(&gr->gr_ref))
		abort();
	return gr;
}

/**
 * @brief release a gsh_refstr reference
 *
 * Use this to release a gsh_refstr reference.
 *
 * @param[in]	gr	Pointer to gsh_refstr
 */
static inline void gsh_refstr_put(struct gsh_refstr *gr)
{
	return urcu_ref_put(&gr->gr_ref, gsh_refstr_release);
}
#endif /* _GSH_REFSTR_H */
