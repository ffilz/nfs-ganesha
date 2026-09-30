// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
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
 * Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file mdcache_uring.c
 * @brief MDCACHE cache-coherence helpers for the io_uring fast paths
 *
 * The io_uring fast paths bypass the normal stacked FSAL call chain: the
 * protocol layer hands a file descriptor and an SQE straight to the kernel,
 * so MDCACHE never sees the read2/write2/getattrs call and therefore never
 * gets the chance to validate or invalidate its cached attributes.
 *
 * This file supplies the small set of hooks the io_uring paths need in order
 * to keep the MDCACHE attribute cache coherent:
 *
 *   - probing the cache before issuing a STATX   (GETATTR hot path)
 *   - folding a completed STATX back in          (GETATTR slow path)
 *   - invalidating after a mutating completion   (WRITE/ALLOCATE/
 *                                                 DEALLOCATE/COMMIT)
 *
 * Every entry point here is handed whatever `data->current_obj` happens to
 * be, which may well be a non-MDCACHE handle (MDCACHE can be turned off per
 * export). Each function therefore degrades to a safe no-op in that case
 * rather than blindly container_of()-ing the handle.
 *
 * Nothing in this file modifies existing MDCACHE behaviour; it only adds
 * new, separately-called helpers. The whole file compiles away when
 * USE_FSAL_URING is off.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/sysmacros.h> /* for makedev(3) */
#include <linux/stat.h> /* for struct statx / STATX_* */

#include "abstract_atomic.h"
#include "fsal.h"
#include "fsal_convert.h"
#include "mdcache_int.h"

/**
 * @brief Return the MDCACHE entry behind a handle, if there is one
 *
 * MDCACHE is a stackable FSAL, so the handle the protocol layer is holding
 * is only an MDCACHE handle when MDCACHE is actually stacked on this export.
 * There is no generic "is this a stackable FSAL" marker in the tree, so we
 * compare the owning module against the MDCACHE module directly.
 *
 * @note No locks are taken or required.
 *
 * @param[in] obj_hdl	Handle to inspect (may be NULL)
 *
 * @return The MDCACHE entry, or NULL if @a obj_hdl is not an MDCACHE handle.
 */
mdcache_entry_t *mdcache_uring_entry(struct fsal_obj_handle *obj_hdl)
{
	if (obj_hdl == NULL || obj_hdl->fsal != &MDCACHE.module)
		return NULL;

	return container_of(obj_hdl, mdcache_entry_t, obj_handle);
}

/**
 * @brief Unstack an MDCACHE handle down to the sub-FSAL handle
 *
 * The io_uring paths need the handle of the FSAL that actually owns the file
 * descriptor (e.g. FSAL_VFS), not the MDCACHE wrapper. We loop rather than
 * unstack a single level so that an MDCACHE stacked on an MDCACHE (which
 * the stacking configuration permits) is fully resolved.
 *
 * @note No locks are taken or required. The returned handle is *not*
 *       referenced; the caller's reference on @a obj_hdl keeps the whole
 *       stack alive, exactly as for the normal subcall() path.
 *
 * @param[in] obj_hdl	Handle to unstack (may be NULL)
 *
 * @return The bottom-most sub-handle, or @a obj_hdl unchanged if it is not
 *         an MDCACHE handle.
 */
struct fsal_obj_handle *
mdcache_uring_sub_handle(struct fsal_obj_handle *obj_hdl)
{
	mdcache_entry_t *entry;

	while (obj_hdl != NULL) {
		entry = mdcache_uring_entry(obj_hdl);

		if (entry == NULL || entry->sub_handle == NULL)
			break;

		obj_hdl = entry->sub_handle;
	}

	return obj_hdl;
}

/**
 * @brief Convert a kernel struct statx into an fsal_attrlist
 *
 * There is no struct statx converter anywhere else in the tree, so this is
 * modelled on ceph2fsal_attributes() (FSAL_CEPH/internal.c) and
 * posix2fsal_attributes() (FSAL/fsal_convert.c), with three differences
 * forced on us by the kernel ABI:
 *
 *  - struct statx carries struct statx_timestamp (__s64 tv_sec, __u32
 *    tv_nsec), which is layout-incompatible with struct timespec, so the
 *    timestamps are converted field by field rather than struct-assigned.
 *
 *  - there is no stx_version, so the change attribute is synthesised the
 *    same way posix2fsal_attributes() does it: max(mtime, ctime) in
 *    nanoseconds. MDCACHE relies on attrs.change, so we must always provide
 *    it when both mtime and ctime are available.
 *
 *  - stx_dev_major/stx_dev_minor and stx_rdev_major/stx_rdev_minor are not
 *    covered by any stx_mask bit; the kernel always fills them in. They are
 *    recombined with makedev(3). Callers that care about the export's view
 *    of the fsid normally overwrite attrs->fsid with obj->fs->fsid after
 *    this call, exactly as the other FSALs do.
 *
 * Only the fields the kernel flagged in stx_mask are converted, and
 * valid_mask is built to match exactly what was converted, so that
 * mdcache_is_attrs_valid() can never believe we supplied more than we did.
 *
 * @note No locks are taken or required; this is a pure conversion.
 * @note valid_mask is OR-ed into @a attrs and supported is overwritten, so
 *       @a attrs must already have been initialised, normally by
 *       fsal_prepare_attrs().
 *
 * @param[in]     stx	The kernel statx result
 * @param[in,out] attrs	The attribute list to fill in
 */
void mdcache_uring_statx_to_attrs(const struct statx *stx,
				  struct fsal_attrlist *attrs)
{
	if (stx == NULL || attrs == NULL)
		return;

	if (op_ctx != NULL && op_ctx->fsal_export != NULL) {
		attrs->supported =
			op_ctx->fsal_export->exp_ops.fs_supported_attrs(
				op_ctx->fsal_export);
	}

	/* stx_dev_* and stx_rdev_* are outside of stx_mask; the kernel
	 * always provides them.
	 */
	attrs->valid_mask |= ATTR_FSID | ATTR_RAWDEV;
	attrs->fsid = posix2fsal_fsid(
		makedev(stx->stx_dev_major, stx->stx_dev_minor));
	attrs->rawdev = posix2fsal_devt(
		makedev(stx->stx_rdev_major, stx->stx_rdev_minor));

	if (stx->stx_mask & STATX_TYPE) {
		attrs->valid_mask |= ATTR_TYPE;
		attrs->type = posix2fsal_type(stx->stx_mode);
	}

	if (stx->stx_mask & STATX_MODE) {
		attrs->valid_mask |= ATTR_MODE;
		attrs->mode = unix2fsal_mode(stx->stx_mode);
	}

	if (stx->stx_mask & STATX_INO) {
		attrs->valid_mask |= ATTR_FILEID;
		attrs->fileid = stx->stx_ino;
	}

	if (stx->stx_mask & STATX_NLINK) {
		attrs->valid_mask |= ATTR_NUMLINKS;
		attrs->numlinks = stx->stx_nlink;
	}

	if (stx->stx_mask & STATX_UID) {
		attrs->valid_mask |= ATTR_OWNER;
		attrs->owner = stx->stx_uid;
	}

	if (stx->stx_mask & STATX_GID) {
		attrs->valid_mask |= ATTR_GROUP;
		attrs->group = stx->stx_gid;
	}

	if (stx->stx_mask & STATX_SIZE) {
		attrs->valid_mask |= ATTR_SIZE;
		attrs->filesize = stx->stx_size;
	}

	if (stx->stx_mask & STATX_BLOCKS) {
		attrs->valid_mask |= ATTR_SPACEUSED;
		attrs->spaceused = (uint64_t)stx->stx_blocks * S_BLKSIZE;
	}

	/* struct statx_timestamp is not struct timespec, convert by field. */
	if (stx->stx_mask & STATX_ATIME) {
		attrs->valid_mask |= ATTR_ATIME;
		attrs->atime.tv_sec = stx->stx_atime.tv_sec;
		attrs->atime.tv_nsec = stx->stx_atime.tv_nsec;
	}

	if (stx->stx_mask & STATX_MTIME) {
		attrs->valid_mask |= ATTR_MTIME;
		attrs->mtime.tv_sec = stx->stx_mtime.tv_sec;
		attrs->mtime.tv_nsec = stx->stx_mtime.tv_nsec;
	}

	if (stx->stx_mask & STATX_CTIME) {
		attrs->valid_mask |= ATTR_CTIME;
		attrs->ctime.tv_sec = stx->stx_ctime.tv_sec;
		attrs->ctime.tv_nsec = stx->stx_ctime.tv_nsec;
	}

	if (stx->stx_mask & STATX_BTIME) {
		attrs->valid_mask |= ATTR_CREATION;
		attrs->creation.tv_sec = stx->stx_btime.tv_sec;
		attrs->creation.tv_nsec = stx->stx_btime.tv_nsec;
	}

	/* No stx_version in struct statx, so synthesise the change attribute
	 * from max(mtime, ctime) the way posix2fsal_attributes() does. Must
	 * run after mtime/ctime have been filled in above.
	 */
	if ((stx->stx_mask & (STATX_MTIME | STATX_CTIME)) ==
	    (STATX_MTIME | STATX_CTIME)) {
		attrs->valid_mask |= ATTR_CHANGE;
		attrs->change = gsh_time_cmp(&attrs->mtime, &attrs->ctime) > 0
					? timespec_to_nsecs(&attrs->mtime)
					: timespec_to_nsecs(&attrs->ctime);
	}
}

/**
 * @brief Probe the MDCACHE attribute cache for an io_uring GETATTR
 *
 * This is the cache-hit fast path: if the cached attributes still cover
 * everything the caller asked for and have not expired, we hand them back
 * and the caller skips the STATX SQE entirely.
 *
 * @note Takes attr_lock for READ internally and always releases it before
 *       returning. The caller must hold no MDCACHE lock.
 * @note @a attrs_out must have been prepared with fsal_prepare_attrs() (its
 *       request_mask drives both the validity test and what is copied out),
 *       and on a true return the caller owns any references in it and must
 *       release them with fsal_release_attrs().
 *
 * @param[in]     obj_hdl	Handle being queried
 * @param[in]     mask		Attributes the caller needs
 * @param[in,out] attrs_out	Receives the cached attributes on a hit
 *
 * @retval true  The cache was valid; @a attrs_out has been filled in.
 * @retval false Not an MDCACHE handle, caching disabled, or a cache miss.
 *               The caller must fetch the attributes itself.
 */
bool mdcache_uring_attrs_valid(struct fsal_obj_handle *obj_hdl, attrmask_t mask,
			       struct fsal_attrlist *attrs_out)
{
	mdcache_entry_t *entry = mdcache_uring_entry(obj_hdl);
	bool valid;

	if (entry == NULL || attrs_out == NULL)
		return false;

	/* Attr_Expiration_Time == 0 disables attribute caching for this
	 * export. mdcache_getattrs() short circuits on this before even
	 * taking attr_lock, do the same.
	 */
	if (op_ctx == NULL || op_ctx->export_perms.expire_time_attr == 0)
		return false;

	PTHREAD_RWLOCK_rdlock(&entry->attr_lock);

	valid = mdcache_is_attrs_valid(entry, mask);

	if (valid) {
		/* Struct copy, taking any ACL reference for the caller. */
		fsal_copy_attrs(attrs_out, &entry->attrs, false);
	}

	PTHREAD_RWLOCK_unlock(&entry->attr_lock);

	return valid;
}

/**
 * @brief Snapshot the attribute generation of an entry
 *
 * An io_uring STATX drops every lock across the SQE/CQE boundary, so a
 * mutating operation can complete in between and bump attr_generation. The
 * caller snapshots the generation before submitting the SQE and feeds it
 * back to mdcache_uring_update_attrs_from_statx() on completion, which is
 * the same race guard mdcache_refresh_attrs() uses around its getattrs
 * subcall.
 *
 * @note No locks are taken or required; the read is atomic.
 *
 * @param[in] obj_hdl	Handle to snapshot
 *
 * @return The current attribute generation, or 0 for a non-MDCACHE handle.
 */
uint32_t mdcache_uring_attr_generation(struct fsal_obj_handle *obj_hdl)
{
	mdcache_entry_t *entry = mdcache_uring_entry(obj_hdl);

	if (entry == NULL)
		return 0;

	return atomic_fetch_uint32_t(&entry->attr_generation);
}

/**
 * @brief Fold a completed io_uring STATX back into the attribute cache
 *
 * Called from the STATX CQE handler. Converts the kernel result, installs it
 * with mdc_update_attr_cache() and then applies the generation race guard:
 * if attr_generation moved while the SQE was in flight then some mutation
 * completed against this file after our statx was issued, the data we are
 * installing may predate it, and MDCACHE_TRUST_ATTRS must be dropped again.
 *
 * @note Takes attr_lock for WRITE internally and always releases it before
 *       returning. The caller must hold no MDCACHE lock.
 * @note mdc_update_attr_cache() consumes the attribute list (fsal_copy_attrs
 *       with pass_refs == true), so the local list is released afterwards
 *       only to drop anything it still owns; this mirrors the belt-and-
 *       braces fsal_release_attrs() in mdcache_refresh_attrs().
 *
 * @param[in] obj_hdl			Handle whose cache to refresh
 * @param[in] stx			The kernel statx result
 * @param[in] original_generation	Generation sampled before the SQE, from
 *					mdcache_uring_attr_generation()
 */
void mdcache_uring_update_attrs_from_statx(struct fsal_obj_handle *obj_hdl,
					   const struct statx *stx,
					   uint32_t original_generation)
{
	mdcache_entry_t *entry = mdcache_uring_entry(obj_hdl);
	struct fsal_attrlist attrs;

	if (entry == NULL || stx == NULL)
		return;

	fsal_prepare_attrs(&attrs, 0);

	mdcache_uring_statx_to_attrs(stx, &attrs);

	/* mdc_update_attr_cache() replaces entry->attrs wholesale, so only
	 * install a statx result that covers everything struct stat would
	 * have given us. Anything less would silently drop attributes we
	 * already had cached; invalidating instead is both cheaper and safer.
	 */
	if ((attrs.valid_mask & ATTRS_POSIX) != ATTRS_POSIX) {
		LogDebug(COMPONENT_MDCACHE,
			 "Partial statx result (stx_mask %" PRIx32
			 "), invalidating instead of caching",
			 stx->stx_mask);
		fsal_release_attrs(&attrs);
		mdcache_uring_invalidate_attrs(obj_hdl);
		return;
	}

	/* We only ever "requested" what statx actually delivered. Keeping
	 * request_mask == valid_mask is what lets mdc_fixup_md() set
	 * MDCACHE_TRUST_ATTRS while mdcache_test_attrs_trust() still refuses
	 * a later request for an attribute statx did not supply.
	 */
	attrs.request_mask = attrs.valid_mask;

	PTHREAD_RWLOCK_wrlock(&entry->attr_lock);

	/* Same ownership bookkeeping mdcache_refresh_attrs() does before
	 * mdc_update_attr_cache(): that function hands any preserved ACL,
	 * fs_locations and sec_label to the new attribute list, and
	 * fsal_copy_attrs() only carries them over if the corresponding bit
	 * is set in entry->attrs.request_mask. Without these the preserved
	 * references would be dropped on the floor.
	 */
	entry->attrs.request_mask = attrs.request_mask;

	if (entry->attrs.acl != NULL)
		entry->attrs.request_mask |= ATTR_ACL;

	if (entry->attrs.fs_locations != NULL)
		entry->attrs.request_mask |= ATTR4_FS_LOCATIONS;

	if (entry->attrs.sec_label.slai_data.slai_data_val != NULL)
		entry->attrs.request_mask |= ATTR4_SEC_LABEL;

	mdc_update_attr_cache(entry, &attrs);

	/* The io_uring statx was issued with no lock held, so a write,
	 * fallocate or commit may have completed against this file while it
	 * was in flight. If the generation moved, what we just cached may be
	 * stale, so drop the trust flag again.
	 */
	if (atomic_fetch_uint32_t(&entry->attr_generation) !=
	    original_generation) {
		atomic_clear_uint32_t_bits(&entry->mde_flags,
					   MDCACHE_TRUST_ATTRS);
	}

	PTHREAD_RWLOCK_unlock(&entry->attr_lock);

	/* mdc_update_attr_cache() consumed the references above, but release
	 * anyway so this stays easy to scan for correctness.
	 */
	fsal_release_attrs(&attrs);
}

/**
 * @brief Invalidate cached attributes after a mutating io_uring completion
 *
 * Called from the WRITE, ALLOCATE, DEALLOCATE and COMMIT CQE handlers, which
 * change size, mtime and ctime behind MDCACHE's back.
 *
 * The ordering below is mandatory and is lifted from mdc_write_super_cb() in
 * mdcache_file.c: attr_generation must be bumped *before* MDCACHE_TRUST_ATTRS
 * is cleared. If the flag were cleared first, a concurrent refresher could
 * observe the stale generation, go fetch attributes, and then see a matching
 * generation afterwards and wrongly conclude nothing had changed underneath
 * it.
 *
 * @note No locks are taken or required; both updates are atomic, matching
 *       the lock-free invalidation the normal write callback does.
 *
 * @param[in] obj_hdl	Handle that was mutated
 */
void mdcache_uring_invalidate_attrs(struct fsal_obj_handle *obj_hdl)
{
	mdcache_entry_t *entry = mdcache_uring_entry(obj_hdl);

	if (entry == NULL)
		return;

	/*
	 * attr_generation must be increased prior to clearing
	 * MDCACHE_TRUST_ATTRS to prevent a case where this flag
	 * was queried before the generation was updated
	 */
	atomic_inc_uint32_t(&entry->attr_generation);
	atomic_clear_uint32_t_bits(&entry->mde_flags, MDCACHE_TRUST_ATTRS);
}

/**
 * @brief Note that an io_uring READ completed against an object
 *
 * Deliberately a no-op beyond validating the handle.
 *
 * It is tempting to bump the cached atime here, but there is no safe value
 * to bump it to: the kernel decides whether the backing filesystem's atime
 * moved at all (relatime, noatime, lazytime and read-only mounts all change
 * the answer), so any timestamp we invent would be a guess that MDCACHE
 * would then hand out to clients as fact. Invalidating instead would be
 * correct but would turn every READ into a forced GETATTR round trip and
 * defeat the point of the io_uring fast path.
 *
 * Doing nothing leaves the cached atime exactly as the last real statx saw
 * it; it ages out normally via Attr_Expiration_Time, and a client that has
 * actually asked for a fresh atime gets one through the usual refresh path.
 * The hook exists so the READ completion path has a documented, central
 * place to change this policy if a future FSAL needs it.
 *
 * @note No locks are taken or required.
 *
 * @param[in] obj_hdl	Handle that was read
 */
void mdcache_uring_touch_atime(struct fsal_obj_handle *obj_hdl)
{
	mdcache_entry_t *entry = mdcache_uring_entry(obj_hdl);

	if (entry == NULL)
		return;

	LogFullDebug(COMPONENT_MDCACHE,
		     "io_uring read on entry %p, cached atime left as is",
		     entry);
}

#endif /* USE_FSAL_URING */
