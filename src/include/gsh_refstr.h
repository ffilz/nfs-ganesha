#ifndef _GSH_REFSTR_H
#define _GSH_REFSTR_H
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <urcu/ref.h>

/* Refcounted strings */
struct gsh_refstr {
	struct urcu_ref	gr_ref;
	char		gr_val[];
};

struct gsh_refstr *gsh_refstr_alloc(size_t len);
void gsh_refstr_release(struct urcu_ref *ref);

#ifndef HAVE_URCU_REF_GET_UNLESS_ZERO
/* Copied from liburcu. Newer versions of the library have this function. */

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

static inline void gsh_refstr_put(struct gsh_refstr *gr)
{
	return urcu_ref_put(&gr->gr_ref, gsh_refstr_release);
}
#endif /* _GSH_REFSTR_H */
