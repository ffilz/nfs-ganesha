/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2015-2016 Red Hat, Inc. and/or its affiliates.
 * Author: Daniel Gryniewicz <dang@redhat.com>
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
 * 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**
 * @addtogroup FSAL_MDCACHE
 * @{
 */

/**
 * @file  mdcache_helpers.c
 * @brief Miscellaneous helper functions
 */

#include "config.h"
#include "fsal.h"
#include "nfs4_acls.h"
#include "mdcache_hash.h"
#include "mdcache_int.h"

static fsal_status_t
mdc_up_invalidate(struct fsal_export *sub_export, struct gsh_buffdesc *handle,
		  uint32_t flags)
{
	mdcache_entry_t *entry;
	fsal_status_t status;
	struct req_op_context *save_ctx, req_ctx = {0};
	mdcache_key_t key;
	struct mdcache_fsal_export *export =
		mdc_export(sub_export->super_export);

	req_ctx.fsal_export = &export->export;
	save_ctx = op_ctx;
	op_ctx = &req_ctx;

	key.fsal = sub_export->fsal;
	(void) cih_hash_key(&key, sub_export->fsal, handle,
			    CIH_HASH_KEY_PROTOTYPE);

	status = mdcache_find_keyed(&key, &entry);
	if (status.major == ERR_FSAL_NOENT) {
		/* Not cached, so invalidate is a success */
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	} else if (FSAL_IS_ERROR(status)) {
		/* Real error */
		return status;
	}

	atomic_clear_uint32_t_bits(&entry->mde_flags,
				   flags & FSAL_UP_INVALIDATE_CACHE);

	if (flags & FSAL_UP_INVALIDATE_CLOSE)
		status = fsal_close(&entry->obj_handle);

	mdcache_put(entry);
	op_ctx = save_ctx;
	return status;
}

/**
 * @brief Update cached attributes
 *
 * @param[in] export Export containing object
 * @param[in] handle Export containing object
 * @param[in] attr   New attributes
 * @param[in] flags  Flags to govern update
 *
 * @return FSAL status
 */

static fsal_status_t
mdc_up_update(struct fsal_export *sub_export, struct gsh_buffdesc *handle,
	      struct attrlist *attr, uint32_t flags)
{
	mdcache_entry_t *entry;
	fsal_status_t status;
	/* Have necessary changes been made? */
	bool mutatis_mutandis = false;
	struct req_op_context *save_ctx, req_ctx = {0};
	mdcache_key_t key;
	struct mdcache_fsal_export *export =
		mdc_export(sub_export->super_export);

	/* These cannot be updated, changing any of them is
	   tantamount to destroying and recreating the file. */
	if (FSAL_TEST_MASK
	    (attr->mask,
	     ATTR_TYPE | ATTR_FSID | ATTR_FILEID | ATTR_RAWDEV | ATTR_RDATTR_ERR
	     | ATTR_GENERATION)) {
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	/* Filter out garbage flags */

	if (flags &
	    ~(fsal_up_update_filesize_inc | fsal_up_update_atime_inc |
	      fsal_up_update_creation_inc | fsal_up_update_ctime_inc |
	      fsal_up_update_mtime_inc | fsal_up_update_chgtime_inc |
	      fsal_up_update_spaceused_inc | fsal_up_nlink)) {
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	req_ctx.fsal_export = &export->export;
	save_ctx = op_ctx;
	op_ctx = &req_ctx;

	key.fsal = sub_export->fsal;
	(void) cih_hash_key(&key, sub_export->fsal, handle,
			    CIH_HASH_KEY_PROTOTYPE);

	status = mdcache_find_keyed(&key, &entry);
	if (status.major != ERR_FSAL_NOENT) {
		/* Not cached, so invalidate is a success */
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	} else if (FSAL_IS_ERROR(status)) {
		/* Real error */
		return status;
	}

	/* Knock things out if the link count falls to 0. */

	if ((flags & fsal_up_nlink) && (attr->numlinks == 0)) {
		atomic_clear_uint32_t_bits(&entry->mde_flags,
					   MDCACHE_TRUST_ATTRS |
					   MDCACHE_TRUST_CONTENT |
					   MDCACHE_DIR_POPULATED);

		status = fsal_close(&entry->obj_handle);

		if (FSAL_IS_ERROR(status))
			goto out;
	}

	if (attr->mask == 0) {
		/* Done */
		goto out;
	}

	PTHREAD_RWLOCK_wrlock(&entry->attr_lock);

	if (attr->expire_time_attr != 0)
		entry->attrs.expire_time_attr = attr->expire_time_attr;

	if (FSAL_TEST_MASK(attr->mask, ATTR_SIZE)) {
		if (flags & fsal_up_update_filesize_inc) {
			if (attr->filesize > entry->attrs.filesize) {
				entry->attrs.filesize = attr->filesize;
				mutatis_mutandis = true;
			}
		} else {
			entry->attrs.filesize = attr->filesize;
			mutatis_mutandis = true;
		}
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_SPACEUSED)) {
		if (flags & fsal_up_update_spaceused_inc) {
			if (attr->spaceused > entry->attrs.spaceused) {
				entry->attrs.spaceused = attr->spaceused;
				mutatis_mutandis = true;
			}
		} else {
			entry->attrs.spaceused = attr->spaceused;
			mutatis_mutandis = true;
		}
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_ACL)) {
		/**
		 * @todo Someone who knows the ACL code, please look
		 * over this.  We assume that the FSAL takes a
		 * reference on the supplied ACL that we can then hold
		 * onto.  This seems the most reasonable approach in
		 * an asynchronous call.
		 */

		nfs4_acl_release_entry(entry->attrs.acl);

		entry->attrs.acl = attr->acl;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_MODE)) {
		entry->attrs.mode = attr->mode;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_NUMLINKS)) {
		entry->attrs.numlinks = attr->numlinks;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_OWNER)) {
		entry->attrs.owner = attr->owner;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_GROUP)) {
		entry->attrs.group = attr->group;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_ATIME)
	    && ((flags & ~fsal_up_update_atime_inc)
		||
		(gsh_time_cmp(&attr->atime, &entry->attrs.atime) == 1))) {
		entry->attrs.atime = attr->atime;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_CREATION)
	    && ((flags & ~fsal_up_update_creation_inc)
		||
		(gsh_time_cmp(&attr->creation, &entry->attrs.creation) == 1))) {
		entry->attrs.creation = attr->creation;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_CTIME)
	    && ((flags & ~fsal_up_update_ctime_inc)
		||
		(gsh_time_cmp(&attr->ctime, &entry->attrs.ctime) == 1))) {
		entry->attrs.ctime = attr->ctime;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_MTIME)
	    && ((flags & ~fsal_up_update_mtime_inc)
		||
		(gsh_time_cmp(&attr->mtime, &entry->attrs.mtime) == 1))) {
		entry->attrs.mtime = attr->mtime;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_CHGTIME)
	    && ((flags & ~fsal_up_update_chgtime_inc)
		||
		(gsh_time_cmp(&attr->chgtime, &entry->attrs.chgtime) == 1))) {
		entry->attrs.chgtime = attr->chgtime;
		mutatis_mutandis = true;
	}

	if (FSAL_TEST_MASK(attr->mask, ATTR_CHANGE)) {
		entry->attrs.change = attr->change;
		mutatis_mutandis = true;
	}

	if (mutatis_mutandis) {
		mdc_fixup_md(entry, attr->mask);
		/* If directory can not trust content anymore. */
		if (entry->obj_handle.type == DIRECTORY) {
			atomic_clear_uint32_t_bits(&entry->mde_flags,
						   MDCACHE_TRUST_CONTENT |
						   MDCACHE_DIR_POPULATED);
		}
		status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	} else {
		atomic_clear_uint32_t_bits(&entry->mde_flags,
					   MDCACHE_TRUST_ATTRS);
		status = fsalstat(ERR_FSAL_INVAL, 0);
	}

	PTHREAD_RWLOCK_unlock(&entry->attr_lock);

 out:
	mdcache_put(entry);
	op_ctx = save_ctx;
	return status;
}

/**
 * @brief Invalidate a cached entry
 *
 * @note doesn't need op_ctx, handled in mdc_up_invalidate
 *
 * @param[in] key    Key to specify object
 * @param[in] flags  FSAL_UP_INVALIDATE*
 *
 * @return FSAL status
 */

static fsal_status_t
mdc_up_invalidate_close(struct fsal_export *sub_export,
			struct gsh_buffdesc *handle, uint32_t flags)
{
	fsal_status_t status;

	status = up_async_invalidate(general_fridge, sub_export, handle,
				     flags | FSAL_UP_INVALIDATE_CLOSE,
				     NULL, NULL);
	return status;
}

fsal_status_t
mdcache_export_up_ops_init(struct fsal_up_vector *my_up_ops,
			   const struct fsal_up_vector *super_up_ops)
{
	/* Init with super ops. Struct copy */
	*my_up_ops = *super_up_ops;

	/* Replace cache-related calls */
	my_up_ops->invalidate = mdc_up_invalidate;
	my_up_ops->update = mdc_up_update;
	my_up_ops->invalidate_close = mdc_up_invalidate_close;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/** @} */
