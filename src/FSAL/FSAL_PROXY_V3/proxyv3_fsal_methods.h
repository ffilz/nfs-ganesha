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

#ifndef _PROXY_V3_FSAL_METHODS_H_
#define _PROXY_V3_FSAL_METHODS_H_

#include "config.h"
#include "fsal.h"
#include "FSAL/fsal_init.h"

struct proxyv3_fsal_module {
   struct fsal_module module;
   struct fsal_obj_ops handle_ops;
};

// Start with just needing the Srv_Addr parameter to an NFSv3 server.
struct proxyv3_client_params {
   sockaddr_t srv_addr;
};

struct proxyv3_export {
   struct fsal_export export;
   struct proxyv3_client_params params;

   char root_handle[NFS3_FHSIZE];
   size_t root_handle_len;
};

extern struct proxyv3_fsal_module PROXY_V3;

bool proxyv3_rpc_init();

bool proxyv3_nfs_call(const char *host, const struct user_cred *creds,
                      const rpcproc_t nfsProc,
                      const xdrproc_t encodeFunc, const void *args,
                      const xdrproc_t decodeFunc, void *output);

bool proxyv3_mount_call(const char *host, const struct user_cred *creds,
                        const rpcproc_t mountProc,
                        const xdrproc_t encodeFunc, const void *args,
                        const xdrproc_t decodeFunc, void *output);


// Helpers for translating from nfsv3 structs to Ganesha data. These could go in
// Protocols/NFS/nfs_proto_tools.c if someone wanted them.

// Return the closest match from the NFSv3 status to Ganesha's fsal_status_t
// (mostly overlapping).
fsal_status_t nfsstat3_to_fsalstat(nfsstat3 status);

// Check that the mask is just asking for NFSv3 and maybe the error bit.
bool attrmask_is_nfs3(attrmask_t mask);

// Convert from an NFSv3 "fattr3" (Ganesha typedef's this to attrlist, while
// keeping fattr3_wire for the "real" one) to a Ganesha attrlist. This function
// also checks that the fsal_attrs_out destination is only asking for NFSv3
// attributes at most.
bool fattr3_to_fsalattr(const fattr3 *attrs,
                        struct attrlist *fsal_attrs_out);

// Convert from the FSAL attrlist to an NFSv3 setattr3 struct.
bool fsalattr_to_sattr3(const struct attrlist *fsal_attrs, sattr3 *attrs_out);

#endif
