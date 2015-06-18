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
#include <sys/acl.h>
#include "posix_acls.h"

/* Checks whether ACE belongs to effective acl (ACCESS TYPE) */
bool
is_ace_valid_for_effective_acl_entry(fsal_ace_t *ace) {
	bool ret;

	if (IS_FSAL_ACE_HAS_INHERITANCE_FLAGS(*ace)) {
		if (IS_FSAL_ACE_APPLICABLE_FOR_BOTH_ACL(*ace))
			ret = true;
		else
			ret = false;
	} else
		ret = true;

	return ret;
}

/* Checks whether ACE belongs to inherited acl (DEFAULT TYPE) */
bool
is_ace_valid_for_inherited_acl_entry(fsal_ace_t *ace) {

	if (IS_FSAL_ACE_APPLICABLE_FOR_BOTH_ACL(*ace)
		|| IS_FSAL_ACE_APPLICABLE_ONLY_FOR_INHERITED_ACL(*ace))
			return  true;
		else
			return false;

}

bool
isallow(fsal_ace_t *ace, acl_permset_t everyone, acl_perm_t perm) {

	bool ret = acl_get_perm(everyone, perm);

	switch (perm) {
	case ACL_READ:
		ret |= IS_FSAL_ACE_READ_DATA(*ace);
		break;
	case ACL_WRITE:
		ret |=	IS_FSAL_ACE_WRITE_DATA(*ace);
		break;
	case ACL_EXECUTE:
		ret |=	IS_FSAL_ACE_EXECUTE(*ace);
		break;
	}

	return ret;
}

bool
isdeny(acl_permset_t deny, acl_permset_t everyone, acl_perm_t perm) {

	return acl_get_perm(deny, perm) | acl_get_perm(everyone, perm);
}

/* Finds ACL entry with help of tag and id */
acl_entry_t
find_entry(acl_t acl, acl_tag_t tag,  unsigned int id) {
	acl_entry_t entry;
	acl_tag_t entryTag;
	int ent, ret;

	if (!acl)
		return NULL;

	for (ent = ACL_FIRST_ENTRY; ; ent = ACL_NEXT_ENTRY) {
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
				if (id != *(unsigned int *)acl_get_qualifier(entry))
					continue;
			break;
		}
	}

	return entry;
}

acl_entry_t
get_entry(acl_t acl, acl_tag_t tag, unsigned int id) {
	acl_entry_t entry;
	int ret;

	if (!acl)
		return NULL;
	entry = find_entry (acl, tag, id);

	if (!entry) {
		ret = acl_create_entry(&acl, &entry);
		if (ret) {
			LogMajor(COMPONENT_FSAL, "Cannot create entry");
			return NULL;
		}
		ret = acl_set_tag_type(entry, tag);
		if (ret)
			LogWarn(COMPONENT_FSAL, "Cannot set tag for Entry");
		ret = acl_set_qualifier(entry, &id);
	}

	return entry;
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

	if (!p_posixacl)
		return fsalstat(ERR_FSAL_FAULT, ret);

	acldata.naces = acl_entries(p_posixacl);

	if (!acldata.naces)
		return fsalstat(ERR_FSAL_FAULT, ret);

	mask = find_entry(p_posixacl, ACL_MASK, 0);
	if (mask) {
		ret = acl_get_permset(mask, &p_permset);
		if (ret)
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the Mask Entry");
		if (acl_get_perm(p_permset, ACL_READ) == 0)
			readmask = false;
		if (acl_get_perm(p_permset, ACL_WRITE) == 0)
			writemask = false;
		if (acl_get_perm(p_permset, ACL_EXECUTE) == 0)
			executemask = false;
		acldata.naces--;
	}
	/* *
	 * Only ALLOW ACL Entries considered right now
	 * TODO : How to display DENY ACL Entries
	 * */
	acldata.aces = (fsal_ace_t *) nfs4_ace_alloc(acldata.naces);

	for (pace = acldata.aces, ent = ACL_FIRST_ENTRY;
		i < acldata.naces; ent = ACL_NEXT_ENTRY) {

		ret = acl_get_entry(p_posixacl, ent, &entry);
		if (ret == 0 || ret == -1) {
			LogWarn(COMPONENT_FSAL,
					"No more ACL entires remaining");
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
			pace->flag = FSAL_ACE_FLAG_GROUP_ID;
			break;
		default:
			LogWarn(COMPONENT_FSAL, "Invalid tag for the acl");
		}

		/* *
		 * Finding permission set for the fsal_acl entry.
		 * Conversion purely is based on
		 * http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
		 * */

		/* *
		 * Unconditionally all ALLOW ACL Entry should
		 * have these permissions
		 * */

		pace->perm = FSAL_ACE_PERM_SET_DEFAULT;
		ret = acl_get_permset(entry, &p_permset);
		if (ret) {
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the ACL Entry");
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
					| FSAL_ACE_PERM_SET_DEFAULT_WRITE;
			if (tag == ACL_USER_OBJ)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_SET_OWNER_WRITE;
		}
		if (acl_get_perm(p_permset, ACL_EXECUTE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						executemask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_EXECUTE;
		}
		i++;
		pace++;
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
acl_t
fsal_acl_2_posix_acl(fsal_acl_t *p_fsalacl, acl_type_t type)
{
	int ret = 0, i;
	fsal_ace_t *f_ace;
	acl_t allow_acl, deny_acl;
	acl_entry_t a_entry, everyone_allow, d_entry, everyone_deny;
	acl_permset_t a_permset, e_a_permset, d_permset, e_d_permset;
	acl_tag_t tag;
	unsigned int id;
	bool mask = false;
        bool deny_e_r = false, deny_e_w = false, deny_e_x = false;


	if (p_fsalacl == NULL || p_fsalacl->naces < 0)
		return NULL;

	/*FIXME: Always allocating more than required, chance of memory leak */
	allow_acl = acl_init(p_fsalacl->naces + 1);
	deny_acl = acl_init(p_fsalacl->naces + 1);

	/* ACE EVERYONE@ should handle first */
	ret = acl_create_entry(&deny_acl, &everyone_deny);
	if (ret) {
		LogMajor(COMPONENT_FSAL, "Cannot create entry for other");
	}

	ret = acl_set_tag_type(everyone_deny, ACL_OTHER);
	if (ret)
		LogWarn(COMPONENT_FSAL, "Cannot set tag for ACL Entry");

	ret = acl_get_permset(everyone_deny, &e_d_permset);

	ret = acl_create_entry(&allow_acl, &everyone_allow);
	if (ret) {
		LogMajor(COMPONENT_FSAL, "Cannot create entry for other");
		return NULL;
	}
	ret = acl_set_tag_type(everyone_allow, ACL_OTHER);
	if (ret)
		LogWarn(COMPONENT_FSAL, "Cannot set tag for ACL Entry");

	ret = acl_get_permset(everyone_allow, &e_a_permset);

	for (f_ace = p_fsalacl->aces;
		f_ace < p_fsalacl->aces + p_fsalacl->naces; f_ace++) {
		if (IS_FSAL_ACE_SPECIAL_EVERYONE(*f_ace)) {
			if ((type == ACL_TYPE_ACCESS &&
			!is_ace_valid_for_effective_acl_entry(f_ace))
			|| (type == ACL_TYPE_DEFAULT &&
			!is_ace_valid_for_inherited_acl_entry(f_ace)))
				continue;
			if (IS_FSAL_ACE_DENY(*f_ace)) {
				if (IS_FSAL_ACE_READ_DATA(*f_ace))
					deny_e_r = true;
				if (IS_FSAL_ACE_WRITE_DATA(*f_ace))
					deny_e_w = true;
				if (IS_FSAL_ACE_EXECUTE(*f_ace))
					deny_e_x = true;
			} else if (IS_FSAL_ACE_ALLOW(*f_ace)) {
				if (IS_FSAL_ACE_READ_DATA(*f_ace) && !deny_e_r)
					acl_add_perm(e_a_permset, ACL_READ);
				if (IS_FSAL_ACE_WRITE_DATA(*f_ace) && !deny_e_w)
					acl_add_perm(e_a_permset, ACL_WRITE);
				if (IS_FSAL_ACE_EXECUTE(*f_ace) && !deny_e_x)
					acl_add_perm(e_a_permset, ACL_EXECUTE);
			}
		}
	}


	/*
	 * TODO: Annoymous users/groups (id = -1) should handle properly
	 */

	for (f_ace = p_fsalacl->aces;
		f_ace < p_fsalacl->aces + p_fsalacl->naces; f_ace++) {
			if ((type == ACL_TYPE_ACCESS &&
			!is_ace_valid_for_effective_acl_entry(f_ace))
			|| (type == ACL_TYPE_DEFAULT &&
			!is_ace_valid_for_inherited_acl_entry(f_ace)))
				continue;
			if (IS_FSAL_ACE_SPECIAL_ID(*f_ace)) {
				id = 0;
				if (IS_FSAL_ACE_SPECIAL_OWNER(*f_ace))
					tag = ACL_USER_OBJ;
				if (IS_FSAL_ACE_SPECIAL_GROUP(*f_ace))
					tag = ACL_GROUP_OBJ;

			} else {
				id = GET_FSAL_ACE_WHO(*f_ace);
				if (IS_FSAL_ACE_GROUP_ID(*f_ace))
					tag = ACL_GROUP;
				else
					tag = ACL_USER;
				mask = true;
			}
			if (IS_FSAL_ACE_SPECIAL_EVERYONE(*f_ace)) {
				if (IS_FSAL_ACE_DENY(*f_ace)) {
					if (deny_e_r)
						acl_add_perm(e_d_permset, ACL_READ);
					if (deny_e_w)
						acl_add_perm(e_d_permset, ACL_WRITE);
					if (deny_e_x)
						acl_add_perm(e_d_permset, ACL_EXECUTE);
				}
					continue;
			}
			a_entry = get_entry(allow_acl, tag, id);
			d_entry = get_entry(deny_acl, tag, id);
			ret = acl_get_permset(d_entry, &d_permset);

			if (IS_FSAL_ACE_DENY(*f_ace)) {
				if (IS_FSAL_ACE_READ_DATA(*f_ace))
					acl_add_perm(d_permset, ACL_READ);
				if (IS_FSAL_ACE_WRITE_DATA(*f_ace))
					acl_add_perm(d_permset, ACL_WRITE);
				if (IS_FSAL_ACE_EXECUTE(*f_ace))
					acl_add_perm(d_permset, ACL_EXECUTE);
			}
			if (IS_FSAL_ACE_ALLOW(*f_ace)) {
				ret = acl_get_permset(a_entry, &a_permset);

				if (isallow(f_ace, e_a_permset, ACL_READ)
				&& !isdeny(d_permset, e_d_permset, ACL_READ))
					acl_add_perm(a_permset, ACL_READ);

				if (isallow(f_ace, e_a_permset, ACL_WRITE)
				&& !isdeny(d_permset, e_d_permset, ACL_WRITE))
					acl_add_perm(a_permset, ACL_WRITE);

				if (isallow(f_ace, e_a_permset, ACL_EXECUTE)
				&& !isdeny(d_permset, e_d_permset, ACL_EXECUTE))
					acl_add_perm(a_permset, ACL_EXECUTE);
			}

	}

	if (mask) {
		ret = acl_calc_mask(&allow_acl);
		if (ret)
			LogWarn(COMPONENT_FSAL,
			"Cannot calculate mask for posix");
	}

	/* A valid acl_t should have only one entry for
	 * ACL_USER_OBJ, ACL_GROUP_OBJ, ACL_OTHER and
	 * ACL_MASK is required only if ACL_USER or
	 * ACL_GROUP exists
	 */

	ret = acl_check(allow_acl, &i);
	if (ret) {
		if (ret > 0) {
			LogWarn(COMPONENT_FSAL,
			"Error converting ACL: %s at entry no %d",
			acl_error(ret), i);
		}
		if (acl_valid(allow_acl))
			return NULL;

	}
	LogDebug(COMPONENT_FSAL, "posix acl = %s ",
				acl_to_any_text(allow_acl, NULL, ',',
					TEXT_ABBREVIATE |
					TEXT_NUMERIC_IDS));
	if (deny_acl)
		acl_free(deny_acl);

	return allow_acl;
}

/*
 *  Given a Posix ACL for directory convert it into an equivalent FSAL ACL
 */
fsal_status_t
posix_acl_2_fsal_acl_for_dir(acl_t e_acl, acl_t i_acl, fsal_acl_t **p_falacl)
{
	/* *
	 * TODO : This api uses same logic as the fsal_acl_2_posix_acl ,
	 * so it will be better to club both api's together
	 */
	int ret = 0, ent, i = 0, ni = 0, ne = 0;
	fsal_acl_status_t status;
	fsal_acl_data_t acldata;
	fsal_ace_t *pace = NULL;
	fsal_acl_t *pacl = NULL;
	acl_entry_t entry, e_mask, i_mask;
	acl_tag_t tag;
	acl_permset_t p_permset;
	bool e_readmask = true;
	bool e_writemask = true;
	bool e_executemask = true;
	bool i_readmask = true;
	bool i_writemask = true;
	bool i_executemask = true;

	if (!e_acl)
		return fsalstat(ERR_FSAL_FAULT, ret);
	/* *
	 * Here both effective acl and default acl need to be converted,
	 * then store it fsal acl, order for ACE entries doesnot matter
	 */
	ne = acl_entries(e_acl);
	ni = acl_entries(i_acl);

	e_mask = find_entry(e_acl, ACL_MASK, 0);
	if (e_mask) {
		ret = acl_get_permset(e_mask, &p_permset);
		if (ret)
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the Mask Entry");
		if (acl_get_perm(p_permset, ACL_READ) == 0)
			e_readmask = false;
		if (acl_get_perm(p_permset, ACL_WRITE) == 0)
			e_writemask = false;
		if (acl_get_perm(p_permset, ACL_EXECUTE) == 0)
			e_executemask = false;
		ne--;
	}
	i_mask = find_entry(i_acl, ACL_MASK, 0);
	if (i_mask) {
		ret = acl_get_permset(i_mask, &p_permset);
		if (ret)
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the Mask Entry");
		if (acl_get_perm(p_permset, ACL_READ) == 0)
			i_readmask = false;
		if (acl_get_perm(p_permset, ACL_WRITE) == 0)
			i_writemask = false;
		if (acl_get_perm(p_permset, ACL_EXECUTE) == 0)
			i_executemask = false;
		ni--;
	}

	acldata.naces = ne + ni;
	if (!acldata.naces)
		return fsalstat(ERR_FSAL_FAULT, ret);

	acldata.aces = (fsal_ace_t *) nfs4_ace_alloc(acldata.naces);

	/* Converting effective acl entry */
	for (pace = acldata.aces, ent = ACL_FIRST_ENTRY; i < ne;
			ent = ACL_NEXT_ENTRY) {

		ret = acl_get_entry(e_acl, ent, &entry);
		if (ret == 0 || ret == -1) {
			LogWarn(COMPONENT_FSAL,
					"No more ACL entires remaining ");
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
			pace->flag = FSAL_ACE_FLAG_GROUP_ID;
			break;
		default:
			LogWarn(COMPONENT_FSAL, "Invalid tag for the acl");
		}

		/* *
		 * Finding permission set for the fsal_acl entry.
		 * Conversion purely is based on
		 * http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
		 * */

		/* *
		 * Unconditionally all ALLOW ACL Entry should
		 * have these permissions
		 * */

		pace->perm = FSAL_ACE_PERM_SET_DEFAULT;
		ret = acl_get_permset(entry, &p_permset);
		if (ret) {
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the ACL Entry");
			continue;
		}
		/* *
		 * Consider Mask bits only for ACL_USER, ACL_GROUP,
		 * ACL_GROUP_OBJ entries
		 * */
		if (acl_get_perm(p_permset, ACL_READ)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						e_readmask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_READ_DATA;
		}
		if (acl_get_perm(p_permset, ACL_WRITE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						e_writemask)
				pace->perm = pace->perm
					| FSAL_ACE_PERM_SET_DEFAULT_WRITE_DIR;
			if (tag == ACL_USER_OBJ)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_SET_OWNER_WRITE;
		}
		if (acl_get_perm(p_permset, ACL_EXECUTE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						e_executemask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_EXECUTE;
		}
		i++;
		pace++;
	}

	/*
	 * Converting inherited acl entry ,here flags should be set
	 * correspondingly
	 */
	for (i = 0, ent = ACL_FIRST_ENTRY; i < ni; ent = ACL_NEXT_ENTRY) {

		ret = acl_get_entry(i_acl, ent, &entry);
		if (ret == 0 || ret == -1) {
			LogWarn(COMPONENT_FSAL,
					"No more ACL entires remaining ");
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
		pace->flag = FSAL_ACE_FLAG_INHERIT;

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
			pace->flag |= FSAL_ACE_FLAG_GROUP_ID;
			break;
		default:
			LogWarn(COMPONENT_FSAL, "Invalid tag for the acl");
		}

		/* *
		 * Finding permission set for the fsal_acl entry.
		 * Conversion purely is based on
		 * http://tools.ietf.org/html/draft-ietf-nfsv4-acl-mapping-05
		 * */

		/* *
		 * Unconditionally all ALLOW ACL Entry should
		 * have these permissions
		 * */

		pace->perm = FSAL_ACE_PERM_SET_DEFAULT;
		ret = acl_get_permset(entry, &p_permset);
		if (ret) {
			LogWarn(COMPONENT_FSAL,
			"Cannot retrieve permission set for the ACL Entry");
			continue;
		}
		/* *
		 * Consider Mask bits only for ACL_USER, ACL_GROUP,
		 * ACL_GROUP_OBJ entries
		 * */
		if (acl_get_perm(p_permset, ACL_READ)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						i_readmask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_READ_DATA;
		}
		if (acl_get_perm(p_permset, ACL_WRITE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						i_writemask)
				pace->perm = pace->perm
					| FSAL_ACE_PERM_SET_DEFAULT_WRITE_DIR;
			if (tag == ACL_USER_OBJ)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_SET_OWNER_WRITE;
		}
		if (acl_get_perm(p_permset, ACL_EXECUTE)) {
			if (tag == ACL_USER_OBJ || tag == ACL_OTHER ||
						i_executemask)
				pace->perm = pace->perm
						| FSAL_ACE_PERM_EXECUTE;
		}
		i++;
		pace++;
	}
	pacl = nfs4_acl_new_entry(&acldata, &status);
	LogMidDebug(COMPONENT_FSAL, "fsal acl = %p, fsal_acl_status = %u", pacl,
		    status);
	if (pacl == NULL) {
		LogCrit(COMPONENT_FSAL,
		"posix_acl_2_fsal_acl_for_dir: failed to create a new acl entry");
		return fsalstat(ERR_FSAL_FAULT, ret);
	} else {
		*p_falacl = pacl;
		return fsalstat(ERR_FSAL_NO_ERROR, ret);
	}
}
