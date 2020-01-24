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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
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
#include <sys/socket.h>

struct proxyv3_fsal_module {
	struct fsal_module module;
	struct fsal_obj_ops handle_ops;
};

// Start with just needing the Srv_Addr parameter to an NFSv3 server.
struct proxyv3_client_params {
	// This is the actual server address.
	sockaddr_t srv_addr;

	// These is *derived* from srv_addr and points to it.
	const struct sockaddr *sockaddr;
	socklen_t socklen;
	char sockname[SOCK_NAME_MAX];

	// Get the ports from portmapper and shove them here.
	uint mountd_port;
	uint nfsd_port;
	uint nlm_port;
	uint readdir_preferred;
};

struct proxyv3_export {
	struct fsal_export export;
	struct proxyv3_client_params params;

	char root_handle[NFS3_FHSIZE];
	size_t root_handle_len;
};

// The little struct we want Ganesha to hold for us.
struct proxyv3_obj_handle {
	struct fsal_obj_handle obj;
	nfs_fh3 fh3;
	fattr3 attrs;
	// Optional pointer to the parent of this object, NULL for the root.
	const struct proxyv3_obj_handle *parent;
};


extern struct proxyv3_fsal_module PROXY_V3;

bool proxyv3_rpc_init();

const struct sockaddr* proxyv3_sockaddr();
const socklen_t proxyv3_socklen();
const uint proxyv3_nlm_port();


bool proxyv3_find_ports(const struct sockaddr *host,
			const socklen_t socklen,
			u_int *mountd_port,
			u_int *nfsd_port,
			u_int *nlm_port);

bool proxyv3_nfs_call(const struct sockaddr *host,
		      const socklen_t socklen,
		      const uint nfsdPort,
		      const struct user_cred *creds,
		      const rpcproc_t nfsProc,
		      const xdrproc_t encodeFunc, const void *args,
		      const xdrproc_t decodeFunc, void *output);

bool proxyv3_mount_call(const struct sockaddr *host,
			const socklen_t socklen,
			const uint mountdPort,
			const struct user_cred *creds,
			const rpcproc_t mountProc,
			const xdrproc_t encodeFunc, const void *args,
			const xdrproc_t decodeFunc, void *output);

bool proxyv3_nlm_call(const struct sockaddr *host,
		      const socklen_t socklen,
		      const uint nlmPort,
		      const struct user_cred *creds,
		      const rpcproc_t nlmProc,
		      const xdrproc_t encodeFunc, const void *args,
		      const xdrproc_t decodeFunc, void *output);

// All the NLM operations funnel through lock_op2, and it's complicated enough
// to need its own file.
fsal_status_t proxyv3_lock_op2(struct fsal_obj_handle *obj_hdl,
			       struct state_t *state,
			       void *owner,
			       fsal_lock_op_t lock_op,
			       fsal_lock_param_t *request_lock,
			       fsal_lock_param_t *conflicting_lock);



// Helpers for translating from nfsv3 structs to Ganesha data. These could go in
// Protocols/NFS/nfs_proto_tools.c if someone wanted them.

// Return the closest match from the NFSv3 status to Ganesha's fsal_status_t
// (mostly overlapping).
fsal_status_t nfsstat3_to_fsalstat(nfsstat3 status);

// Return the closest match from the NLMv4 status to Ganesha's fsal_status_t.
fsal_status_t nlm4stat_to_fsalstat(nlm4_stats status);

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
