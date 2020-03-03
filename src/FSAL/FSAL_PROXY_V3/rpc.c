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

#include <rpc/rpc.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "nfs23.h"

#include "proxyv3_fsal_methods.h"

static unsigned rand_seed = 123451;
static char rpcMachineName[MAXHOSTNAMELEN + 1] = { 0 };

bool proxyv3_rpc_init() {
   // Cache our hostname for client auth later.
   if (gethostname(rpcMachineName, sizeof(rpcMachineName)) != 0) {
      const char* kClientName = "192.168.1.2";

      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: gethostname() failed. Errno %d (%s)."
              "Hardcoding a client IP instead.",
              errno, strerror(errno));
      memcpy(rpcMachineName, kClientName, strlen(kClientName) + 1 /* For NUL */);
   }

   // TODO(boulos): Setup some buffers and mutexes for rand_seed, open
   // sockets, etc.
   return true;
}


// NOTE(boulos): This is basically rpc_call redone by hand, because
// ganesha's "nfsd" hijacks the RPC setup to the point where we can't
// issue our own NFS-related rpcs as a simple client.
bool proxyv3_call(const char *host, uint16_t port,
                  const struct user_cred *creds,
                  const rpcprog_t rpcProgram,
                  const rpcvers_t rpcVersion,
                  const rpcproc_t rpcProc,
                  const xdrproc_t encodeFunc, const void *args,
                  const xdrproc_t decodeFunc, void *output) {
   XDR x;
   struct rpc_msg rmsg;
   struct rpc_msg reply;

   int fd = socket(PF_INET /* IPv4 */,
                   SOCK_STREAM /* TCP */,
                   IPPROTO_TCP /* TCP */);
   if (fd < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to create a socket. %d %s",
              errno, strerror(errno));
      return false;
   }

   // NOTE(boulos): NFS daemons like nfsd in Linux require that the
   // clients come from a privileged port, so that they "must" be run
   // as root on the client.
   if (bindresvport(fd, NULL) < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to reserve a privileged port. %d %s",
              errno, strerror(errno));
      return false;
   }

   struct sockaddr_in hostAddr = {
      .sin_family = AF_INET,
   };

   hostAddr.sin_port = htons(port);

   // Grr, inet_aton et al. return 0 if the input was *invalid*.
   if (inet_aton(host, &hostAddr.sin_addr) == 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to parse host '%s'.", host);
      return false;
   }

   if (connect(fd, &hostAddr, sizeof(struct sockaddr_in)) < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to connect to host '%s'. errno %d (%s)",
              host, errno, strerror(errno));
      close(fd);
      return false;
   }


   // Make a little buffer that's big enough for handling most
   // requests/responses. TODO(boulos): Move this into a
   // per-<something> allocator / reuse it.
   const int kBufSize = 4096;
   char *msgbuf = gsh_calloc(1, kBufSize);

   AUTH *au;
   if (creds != NULL) {
      au = authunix_ncreate(rpcMachineName,
                            creds->caller_uid, creds->caller_gid,
                            creds->caller_glen, creds->caller_garray);
   } else {
      // Let ganesha do lots of syscalls to figure out our machiine name,
      // uid, gid and so on.
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: rpc, no creds given => authunix_ncreate_default()");
      au = authunix_ncreate_default();
   }

   // We need some transaction ID, so how about a random one.
   u_int xid = rand_r(&rand_seed);

   rmsg.rm_xid = xid;
   rmsg.rm_direction = CALL;
   rmsg.rm_call.cb_rpcvers = RPC_MSG_VERSION; /* *RPC* version not NFS */
   rmsg.cb_prog = rpcProgram;
   rmsg.cb_vers = rpcVersion;
   rmsg.cb_proc = rpcProc;
   rmsg.cb_cred = au->ah_cred;
   rmsg.cb_verf = au->ah_verf;

   memset(&x, 0, sizeof(x));

   // Setup x with our buffer for encoding. Keep space at the front
   // for the u_int recmark.
   xdrmem_create(&x,
                 msgbuf + sizeof(u_int),
                 kBufSize - sizeof(u_int),
                 XDR_ENCODE);

   if (!xdr_callmsg(&x, &rmsg)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to Setup xdr_callmsg");
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   if (!encodeFunc(&x, args)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to xdr-encode the args");
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Extract out the position to encode the record marker.
   u_int pos = xdr_getpos(&x);
   u_int recmark = ntohl(pos | (1U << 31));
   // Write the recmark at the start of the buffer
   memcpy(msgbuf, &recmark, sizeof(recmark));
   // Send the message plus the recmark.
   size_t bytes_to_send = pos + sizeof(recmark);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Sending XID %u with %zu bytes",
            rmsg.rm_xid, bytes_to_send);

   size_t total_bytes_written = 0;
   while (total_bytes_written < bytes_to_send) {
      ssize_t bytes_written =
         write(fd,
               msgbuf + total_bytes_written,
               bytes_to_send - total_bytes_written);
      if (bytes_written < 0) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: Write at %zu failed. Errno %d (%s)",
                 total_bytes_written, errno, strerror(errno));
         gsh_free(msgbuf);
         AUTH_DESTROY(au);
         return false;
      }

      total_bytes_written += bytes_written;
   }

   // Aww, short write. Exit.
   if (total_bytes_written != bytes_to_send) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Only wrote %zu bytes out of %zu",
              total_bytes_written, bytes_to_send);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Now flip it around and get the reply.
   struct {
      uint recmark;
      uint xid;
   } response_header;

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Let's go ask for a response.");

   // First try to read just the response "header".
   if (read(fd, &response_header, 8) != 8) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Didn't get a response header. errno %d, errstring %s",
              errno, strerror(errno));
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Flip endian-ness if required
   response_header.recmark = ntohl(response_header.recmark);
   response_header.xid = ntohl(response_header.xid);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Got back recmark %x (%u bytes), xid %u\n",
            response_header.recmark,
            response_header.recmark & ~(1U << 31),
            response_header.xid);

   if (response_header.xid != xid) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Response xid (%u) doesn't match request %u",
              response_header.xid, xid);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Clear the top bit of the recmark
   response_header.recmark &= ~(1U << 31);
   if (response_header.recmark < 8) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Response claims to only have %u bytes",
              response_header.recmark);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // We've already read the header (record mark) and xid.
   size_t bytes_to_read = response_header.recmark;
   size_t total_bytes_read = 4;
   memset(msgbuf, 0, kBufSize);
   // Write the xid into the buffer.
   memcpy(msgbuf, &xid, sizeof(xid));

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Going to read the remaining %zu bytes",
            bytes_to_read - total_bytes_read);

   while (total_bytes_read < bytes_to_read) {
      ssize_t bytes_read =
         read(fd, msgbuf + total_bytes_read, bytes_to_read - total_bytes_read);
      if (bytes_read < 0) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: Read at %zu failed. Errno %d (%s)",
                 total_bytes_read, errno, strerror(errno));
         gsh_free(msgbuf);
         AUTH_DESTROY(au);
         return false;
      }

      total_bytes_read += bytes_read;
   }

   // Aww, short read. Exit.
   if (total_bytes_read != bytes_to_read) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Only read %zu bytes out of %zu",
              total_bytes_read, bytes_to_read);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Got all the bytes, time to decode");

   // Lets decode the reply.
   memset(&x, 0, sizeof(x));
   xdrmem_create(&x, msgbuf, total_bytes_read, XDR_DECODE);

   memset(&reply, 0, sizeof(reply));
   reply.RPCM_ack.ar_results.proc = decodeFunc;
   reply.RPCM_ack.ar_results.where = output;

   if (!xdr_replymsg(&x, &reply)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to do xdr_replymsg");
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Check that it was accepted, if not, say why not.
   if (reply.rm_reply.rp_stat != MSG_ACCEPTED) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Reply received but not accepted. REJ %d",
              reply.rm_reply.rp_rjct.rj_stat);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   // Check that it was accepted with success.
   if (reply.rm_reply.rp_acpt.ar_stat != SUCCESS) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Reply accepted but unsuccesful. Reason %d",
              reply.rm_reply.rp_acpt.ar_stat);
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: RPC completed successfully");

   return true;
}

// Helpful wrappers around the generic RPC call so that we don't need
// to repeatedly pass in the program and version constants.
bool proxyv3_nfs_call(const char *host,
                      const struct user_cred *creds,
                      const rpcproc_t nfsProc,
                      const xdrproc_t encodeFunc, const void *args,
                      const xdrproc_t decodeFunc, void *output) {
   const int kProgramNFS = NFS_PROGRAM;
   const int kVersionNFSv3 = NFS_V3;
   const int kPortNFS = 2049;

   return proxyv3_call(host, kPortNFS, creds,
                       kProgramNFS, kVersionNFSv3,
                       nfsProc, encodeFunc, args, decodeFunc, output);
}

bool proxyv3_mount_call(const char *host,
                        const struct user_cred *creds,
                        const rpcproc_t mountProc,
                        const xdrproc_t encodeFunc, const void *args,
                        const xdrproc_t decodeFunc, void *output) {
   const int kProgramMount = MOUNTPROG;
   const int kVersionMountv3 = MOUNT_V3;
   const int kPortMount = 2050;

   return proxyv3_call(host, kPortMount, creds,
                       kProgramMount, kVersionMountv3,
                       mountProc, encodeFunc, args, decodeFunc, output);
}
