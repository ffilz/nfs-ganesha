/*
 * posix_acls.c
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Red Hat  Inc., 2015
 * Author: Niels de Vos <ndevos@redhat.com>
 *	   Jiffin Tony Thottan <jthottan@redhat.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *
 * Conversion routines for fsal_acl <-> POSIX ACl
 *
 * Routines based on the description from an Internet Draft that has also been
 * used for the implementation of the conversion in the Linux kernel
 * NFS-server.
 *
 *     Title: Mapping Between NFSv4 and Posix Draft ACLs
 *   Authors: Marius Aamodt Eriksen & J. Bruce Fields
 *       URL: http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
 */


#include <acl/libacl.h>
#include "nfs4_acls.h"
#include "fsal_types.h"

/* Finds ACL entry with help of tag and id */
acl_entry_t
find_entry(acl_t acl, acl_tag_t tag, int id) {
	acl_entry_t entry;
	acl_tag_t entryTag;
	int ent, ret;

	for (ent = ACL_FIRST_ENTRY ; ; ent = ACL_NEXT_ENTRY) {
		ret = acl_get_entry(acl, ent, &entry);
		if (ret == -1) {
			LogWarn(COMPONENT_FSAL, "acl_get entry failed errno %d",
					errno);
		}
		if (ret == 0 || ret == -1)
			return NULL;

		if (acl_get_tag_type(entry, &entryTag) == -1) {
			LogWarn(COMPONENT_FSAL, "No entry tag for ACL Entry");
			continue;
		}
		if (tag == entryTag) {
			if (tag == ACL_USER || tag == ACL_GROUP)
				if (id != *(int *)acl_get_qualifier(entry))
					continue;
			return entry;
		}
	}
}

/*
 *  Given a POSIX ACL convert it into an equivalent FSAL ACL
 */
fsal_status_t
posix_acl_2_fsal_acl(acl_t p_posixacl, fsal_acl_t **p_falacl)
{
	int ret = 0, ent, i = 0;
	fsal_acl_status_t status;
	fsal_acl_data_t acldata;
	fsal_ace_t *pace = NULL;
	fsal_acl_t *pacl = NULL;
	acl_entry_t entry, mask;
	acl_tag_t tag;
	acl_permset_t p_permset;
	bool readmask = true;
	bool writemask = true;
	bool executemask = true;

	acldata.naces = acl_entries(p_posixacl);
	acldata.aces = (fsal_ace_t *) nfs4_ace_alloc(acldata.naces);

	if (!acldata.naces)
		return fsalstat(ERR_FSAL_NO_ERROR, ret);

	mask = find_entry(p_posixacl, ACL_MASK, 0);
	if (mask) {
		ret = acl_get_permset(mask, &p_permset);
		if (ret)
			LogWarn(COMPONENT_FSAL,
				"Cannot retrieve permission "
				"set for the Mask Entry");
		if (acl_get_perm(p_permset, ACL_READ) == 0)
			readmask = false;
		if (acl_get_perm(p_permset, ACL_WRITE) == 0)
			writemask = false;
		if (acl_get_perm(p_permset, ACL_EXECUTE) == 0)
			executemask = false;
	}
	/* *
	 * Only ALLOW ACL Entries converted right now
	 * TODO : How to convert DENY ACL Entries
	 * */
	for (pace = acldata.aces, ent = ACL_FIRST_ENTRY;
		i < acldata.naces; pace++, ent = ACL_NEXT_ENTRY) {

		ret = acl_get_entry(p_posixacl, ent, &entry);
		if (ret == 0 || ret == -1) {
			LogWarn(COMPONENT_FSAL, "No more ACL entires"
					" remaining ");
			break;
		}
		if (acl_get_tag_type(entry, &tag) == -1) {
			LogWarn(COMPONENT_FSAL, "No entry tag for ACL Entry");
			continue;
		}
		/* Mask is not converted to a fsal_acl entry , skipping */
		if (tag == ACL_MASK)
			continue;

		pace->type = FSAL_ACE_TYPE_ALLOW;
		pace->flag = 0;

		/* Finding uid for the fsal_acl entry */
		switch (tag) {
		case  ACL_USER_OBJ:
			pace->who.uid =  FSAL_ACE_SPECIAL_OWNER;
			pace->iflag = FSAL_ACE_IFLAG_SPECIAL_ID;
			break;
		case  ACL_GROUP_OBJ:
			pace->who.uid =  FSAL_ACE_SPECIAL_GROUP;
			pace->iflag = FSAL_ACE_IFLAG_SPECIAL_ID;
			break;
		case  ACL_OTHER:
			pace->who.uid =  FSAL_ACE_SPECIAL_EVERYONE;
			pace->iflag = FSAL_ACE_IFLAG_SPECIAL_ID;
			break;
		case  ACL_USER:
			pace->who.uid =
				*(uid_t *)acl_get_qualifier(entry);
			break;
		case  ACL_GROUP:
			pace->who.gid =
				*(gid_t *)acl_get_qualifier(entry);
			break;
		}

		/* *
		 * Finding permission set for the fsal_acl entry.
		 * Convertion purely is based on
		 * http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
		 * */

		/* *
		 * Unconditionally all ALLOW ACL Entry should
		 * have these permissions
		 * */

		pace->perm = FSAL_ACE_PERM_READ_ACL
				| FSAL_ACE_PERM_READ_ATTR
				| FSAL_ACE_PERM_SYNCHRONIZE;
		ret = acl_get_permset(entry, &p_permset);
		if (ret) {
			LogWarn(COMPONENT_FSAL,
				"Cannot retrieve permission "
				"set for the ACL Entry");
			continue;
		}
		/* *
		 * Consider Mask bits only for ACL_USER, ACL_GROUP,
		 * ACL_GROUP_OBJ entries
		 * */
		if (acl_get_perm(p_permset, ACL_READ)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						readmask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_READ_DATA;
		}
		if (acl_get_perm(p_permset, ACL_WRITE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						writemask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_WRITE_DATA
						| FSAL_ACE_PERM_APPEND_DATA;
			if (tag == ACL_USER_OBJ)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_WRITE_ACL
						| FSAL_ACE_PERM_WRITE_ATTR;
		}
		if (acl_get_perm(p_permset, ACL_EXECUTE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						executemask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_EXECUTE;
		}
		i++;
	}
	pacl = nfs4_acl_new_entry(&acldata, &status);
	LogMidDebug(COMPONENT_FSAL, "fsal acl = %p, fsal_acl_status = %u", pacl,
		    status);
	if (pacl == NULL) {
		LogCrit(COMPONENT_FSAL,
			"posix_acl_2_fsal_acl: failed to create a new acl entry");
		return fsalstat(ERR_FSAL_FAULT, ret);
	} else {
		*p_falacl = pacl;
		return fsalstat(ERR_FSAL_NO_ERROR, ret);
	}
}

/*
 *  Given a FSAL ACL convert it into an equivalent POSIX ACL
 */
fsal_status_t
fsal_acl_2_posix_acl(fsal_acl_t *p_fsalacl, acl_t *p_posixacl)
{
	int ret = 0, i, id;
	fsal_errors_t err = ERR_FSAL_NO_ERROR;
	fsal_ace_t *f_ace;
	acl_entry_t p_entry, prev;
	acl_permset_t p_permset;
	acl_tag_t tag;
	uid_t uid;
	gid_t gid;
	bool user = false;
	bool group = false;

	for (f_ace = p_fsalacl->aces, i = 0;
		f_ace < p_fsalacl->aces + p_fsalacl->naces; f_ace++, i++) {

		if (IS_FSAL_ACE_DENY(*f_ace)) {

			if (IS_FSAL_ACE_SPECIAL_ID(*f_ace)) {

				if (IS_FSAL_ACE_SPECIAL_OWNER(*f_ace))
					tag = ACL_USER_OBJ;

				if (IS_FSAL_ACE_SPECIAL_GROUP(*f_ace))
					tag = ACL_GROUP_OBJ;

				if (IS_FSAL_ACE_SPECIAL_EVERYONE(*f_ace))
					tag = ACL_OTHER;
			id = 0;
			} else {
				if (IS_FSAL_ACE_GROUP_ID(*f_ace)) {
					tag = ACL_GROUP;
					gid = GET_FSAL_ACE_WHO(*f_ace);
					id = gid;
				} else {
					tag = ACL_USER;
					uid = GET_FSAL_ACE_WHO(*f_ace);
					id = uid;
				}
			}
			prev = find_entry(*p_posixacl, tag, id);
			if (prev) {
				LogDebug(COMPONENT_FSAL,
						"found previous acl entry");
				ret = acl_get_permset(prev, &p_permset);

				if (IS_FSAL_ACE_READ_DATA(*f_ace))
					acl_delete_perm(p_permset, ACL_READ);
				if (IS_FSAL_ACE_WRITE_DATA(*f_ace))
					acl_delete_perm(p_permset, ACL_WRITE);
				if (IS_FSAL_ACE_EXECUTE(*f_ace))
					acl_delete_perm(p_permset, ACL_EXECUTE);

				/*
				 * Delete the entry if there is no more acl
				 * permission exists for that entry and also
				 * if it is not ACL_USER_OBJ, ACL_GROUP_OBJ,
				 * ACL_OTHER entry
				 */
				if (!IS_FSAL_ACE_SPECIAL_ID(*f_ace)) {
					if (!acl_get_perm(p_permset, ACL_READ)
					&& !acl_get_perm(p_permset, ACL_WRITE)
				     && !acl_get_perm(p_permset, ACL_EXECUTE))
						acl_delete_entry(*p_posixacl,
									prev);
				}
			}

		} else if (IS_FSAL_ACE_ALLOW(*f_ace)) {

		/* TODO: loop through this a little but differente/more
		 * http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
		 * page 12 suggests adding @EVERYONE first.
		 *
		 *
		 * There is also the difference to account for with inheritance
		 * of ACLs. Directories should have those, but files don't need
		 * them.
		 */

			ret = acl_create_entry(p_posixacl, &p_entry);
			if (ret) {
				err = errno == EINVAL ? ERR_FSAL_INVAL :
							ERR_FSAL_NOMEM;
				return fsalstat(err, -1);
			}

			if (IS_FSAL_ACE_SPECIAL_ID(*f_ace)) {
			/* POSIX ACLs do not contain IDs for the special ACEs */
				if (IS_FSAL_ACE_SPECIAL_OWNER(*f_ace))
					tag = ACL_USER_OBJ;

				if (IS_FSAL_ACE_SPECIAL_GROUP(*f_ace))
					tag = ACL_GROUP_OBJ;

				if (IS_FSAL_ACE_SPECIAL_EVERYONE(*f_ace))
					tag = ACL_OTHER;

				id = 0;
			} else {
				if (IS_FSAL_ACE_GROUP_ID(*f_ace)) {
					tag = ACL_GROUP;
					group = true;
					gid = GET_FSAL_ACE_WHO(*f_ace);
					id = gid;
					ret = acl_set_qualifier(p_entry, &gid);
				} else {
					tag = ACL_USER;
					user = true;
					uid = GET_FSAL_ACE_WHO(*f_ace);
					id = uid;
					ret = acl_set_qualifier(p_entry, &uid);
				}

			}
			prev = find_entry(*p_posixacl, tag, id);
			if (prev) {
				LogDebug(COMPONENT_FSAL,
						"deleting previous acl entry");
				acl_delete_entry(*p_posixacl, prev);
			}
			ret = acl_set_tag_type(p_entry, tag);
			if (ret) {
				LogWarn(COMPONENT_FSAL, "Cannot set tag"
						" for ACL Entry");
				continue;
			}

			ret = acl_get_permset(p_entry, &p_permset);
			if (ret) {
				LogWarn(COMPONENT_FSAL,
					"Cannot retrieve permission "
					"set for the ACL Entry");
				continue;
			}
			if (IS_FSAL_ACE_READ_DATA(*f_ace))
				acl_add_perm(p_permset, ACL_READ);
			if (IS_FSAL_ACE_WRITE_DATA(*f_ace))
				acl_add_perm(p_permset, ACL_WRITE);
			if (IS_FSAL_ACE_EXECUTE(*f_ace))
				acl_add_perm(p_permset, ACL_EXECUTE);
		}
	}

	/* calculate appropriate mask if it is needed*/
	if (user || group)
		ret = acl_calc_mask(p_posixacl);

	/* A valid acl_t should have only one entry for
	 * ACL_USER_OBJ, ACL_GROUP_OBJ, ACL_OTHER and
	 * ACL_MASK is required only if ACL_USER or
	 * ACL_GROUP exists
	 */
	ret = acl_check(*p_posixacl, &i);
	if (ret) {
		if (ret > 0) {
			LogWarn(COMPONENT_FSAL,
			"Error converting ACL: %s at entry no %d",
			acl_error(ret), i);
		}

	err = ERR_FSAL_INVAL;
	ret = -1;
	}

	return fsalstat(err, ret);
}
