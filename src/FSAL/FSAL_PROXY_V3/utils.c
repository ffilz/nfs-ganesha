/*
 * Copyright 2020 Google LLC
 * Author: Solomon Boulos <boulos@google.com>
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
 * -------------
 */

#include "nfs23.h"

#include "proxyv3_fsal_methods.h"

// Map from nfsstat3 error codes to the FSAL error codes where appropriate.
static fsal_errors_t nfsstat3_to_fsal(nfsstat3 status) {
   switch (status) {
      // Most of these have identical enum values, but do this explicitly anyway.
   case NFS3_OK:        return ERR_FSAL_NO_ERROR;
   case NFS3ERR_PERM:      return ERR_FSAL_PERM;
   case NFS3ERR_NOENT:     return ERR_FSAL_NOENT;
   case NFS3ERR_IO:     return ERR_FSAL_IO;
   case NFS3ERR_NXIO:   return ERR_FSAL_NXIO;
   case NFS3ERR_ACCES:  return ERR_FSAL_ACCESS;
   case NFS3ERR_EXIST:  return ERR_FSAL_EXIST;
   case NFS3ERR_XDEV:   return ERR_FSAL_XDEV;
   // FSAL doesn't have NODEV, but NXIO is "No such device or address"
   case NFS3ERR_NODEV:  return ERR_FSAL_NXIO;
   case NFS3ERR_NOTDIR: return ERR_FSAL_NOTDIR;
   case NFS3ERR_ISDIR:  return ERR_FSAL_ISDIR;
   case NFS3ERR_INVAL:  return ERR_FSAL_INVAL;
   case NFS3ERR_FBIG:   return ERR_FSAL_FBIG;
   case NFS3ERR_NOSPC:  return ERR_FSAL_NOSPC;
   case NFS3ERR_ROFS:   return ERR_FSAL_ROFS;
   case NFS3ERR_MLINK:  return ERR_FSAL_MLINK;
   case NFS3ERR_NAMETOOLONG: return ERR_FSAL_NAMETOOLONG;
   case NFS3ERR_NOTEMPTY:    return ERR_FSAL_NOTEMPTY;
   case NFS3ERR_DQUOT:       return ERR_FSAL_DQUOT;
   case NFS3ERR_STALE:       return ERR_FSAL_STALE;
   // FSAL doesn't have REMOTE (too many remotes), so just return NAMETOOLONG.
   case NFS3ERR_REMOTE:      return ERR_FSAL_NAMETOOLONG;
   case NFS3ERR_BADHANDLE:   return ERR_FSAL_BADHANDLE;
   // FSAL doesn't have NOT_SYNC, so... INVAL?
   case NFS3ERR_NOT_SYNC:    return ERR_FSAL_INVAL;
   case NFS3ERR_BAD_COOKIE:  return ERR_FSAL_BADCOOKIE;
   case NFS3ERR_NOTSUPP:     return ERR_FSAL_NOTSUPP;
   case NFS3ERR_TOOSMALL:    return ERR_FSAL_TOOSMALL;
   case NFS3ERR_SERVERFAULT: return ERR_FSAL_SERVERFAULT;
   case NFS3ERR_BADTYPE:     return ERR_FSAL_BADTYPE;
      // FSAL doesn't have a single JUKEBOX error, so choose ERR_FSAL_LOCKED
   case NFS3ERR_JUKEBOX:     return ERR_FSAL_LOCKED;
   }

   // Shouldn't have gotten here with valid input.
   return ERR_FSAL_INVAL;
}

fsal_status_t nfsstat3_to_fsalstat(nfsstat3 status) {
   fsal_errors_t rc = nfsstat3_to_fsal(status);
   return fsalstat(rc, (rc == ERR_FSAL_INVAL) ? (int) status : 0);
}

// Fill in the FSAL attrlist (fsal_attrs_out) given the input NFSv3
// attributes. This function returns false if the requested attributes
// are greater than those supported by NFSv3.
bool fattr3_to_fsalattr(const fattr3 *attrs,
                        struct attrlist *fsal_attrs_out) {
   // NOTE(boulos): Consider contributing this as FSAL_ONLY_MASK or something.
   attrmask_t requested = fsal_attrs_out->request_mask;
   if (FSAL_UNSET_MASK(requested, ATTRS_NFS3) != 0) {
      LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG,
                  "Requested attrs > NFSv3 ",
                  fsal_attrs_out, false);
      return false;
   }

   // NOTE(boulos): Since nfs23.h typedefs fattr3 to attrlist (leaving
   // fattr3_wire for the real fattr3 from the protocol) this is just a simple
   // copy.
   *fsal_attrs_out = *attrs;

   // Claim that only the NFSv3 attributes are valid.
   FSAL_SET_MASK(fsal_attrs_out->valid_mask, ATTRS_NFS3);
   return true;
}
