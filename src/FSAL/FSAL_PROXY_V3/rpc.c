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

#include <rpc/pmap_prot.h>
#include <rpc/rpc.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "nfs23.h"

#include "proxyv3_fsal_methods.h"

static unsigned rand_seed = 123451;
static char rpcMachineName[MAXHOSTNAMELEN + 1] = { 0 };

static pthread_mutex_t rpcLock;

const unsigned kMaxSockets = 32;

struct fd_entry {
   bool in_use;
   bool is_open;

   // Need to match the socket/socklen/port.
   sockaddr_t socket;
   socklen_t socklen;
   uint16_t port;

   int fd;
};

// TODO(boulos): Replace with free list / hash table / whatever.
struct fd_entry *fd_entries;

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

   if (pthread_mutex_init(&rpcLock, NULL) != 0) {
      LogCrit(COMPONENT_FSAL,
              "Failed to initialize a mutex... Errno %d (%s).",
              errno, strerror(errno));
      return false;
   }

   // Initialize the fd_entries with not in use sockets.
   fd_entries = gsh_calloc(kMaxSockets, sizeof(struct fd_entry));
   return true;
}

// Do the actual raw socket opening of an fd of host:port.
static int
proxyv3_openfd(const struct sockaddr *host,
               const socklen_t socklen,
               uint16_t port) {
   LogDebug(COMPONENT_FSAL,
            "Opening a new socket");

   if (host->sa_family != AF_INET &&
       host->sa_family != AF_INET6) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: passed a host with sa_family %u",
              host->sa_family);
      return -1;
   }

   char addrForErrors[INET6_ADDRSTRLEN] = { 0 };
   // Strangely, inet_ntop takes the length of the *buffer* not the length of
   // the socket (perhaps it just uses the sa_family for socklen)
   if (!inet_ntop(host->sa_family, host, addrForErrors, INET6_ADDRSTRLEN)) {
      LogCrit(COMPONENT_FSAL,
              "Couldn't decode host socket for debugging");
      return -1;
   }

   bool ipv6 = host->sa_family == AF_INET6;
   if ((ipv6 && socklen != sizeof(struct sockaddr_in6)) ||
       (!ipv6 && socklen != sizeof(struct sockaddr_in))) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Given an ipv%s sockaddr (%s) with len %u != %zu",
              (ipv6) ? "6" : "4",
              addrForErrors,
              socklen,
              (ipv6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
      return -1;
   }

   struct sockaddr hostAndPort;
   memset(&hostAndPort, 0, sizeof(hostAndPort));
   // Copy the input, and then override the port.
   memcpy(&hostAndPort, host, socklen);
   struct sockaddr_in  *hostv4 = (struct sockaddr_in*) &hostAndPort;
   struct sockaddr_in6 *hostv6 = (struct sockaddr_in6*) &hostAndPort;

   // Check that the caller is letting us slip the port in.
   if ((ipv6 && hostv6->sin6_port != 0) ||
       (!ipv6 && hostv4->sin_port != 0)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: passed an address (%s) with non-zero port %u",
              addrForErrors,
              (ipv6) ? hostv6->sin6_port : hostv4->sin_port);
      return -1;
   }

   int fd = socket((ipv6) ? PF_INET6 /* IPv6 */ : PF_INET /* IPv4 */,
                   SOCK_STREAM /* TCP */,
                   0 /* Pick it up from TCP */);
   if (fd < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to create a socket. %d %s",
              errno, strerror(errno));
      return -1;
   }

   // NOTE(boulos): NFS daemons like nfsd in Linux require that the
   // clients come from a privileged port, so that they "must" be run
   // as root on the client.
   if (bindresvport_sa(fd, NULL) < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to reserve a privileged port. %d %s",
              errno, strerror(errno));
      close(fd);
      return -1;
   }

   if (ipv6) {
      hostv6->sin6_port = htons(port);
   } else {
      hostv4->sin_port = htons(port);
   }

   if (connect(fd, &hostAndPort, socklen) < 0) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Failed to connect to host '%s'. errno %d (%s)",
              addrForErrors, errno, strerror(errno));
      close(fd);
      return -1;
   }

   LogDebug(COMPONENT_FSAL,
            "Got a new socket (%d) open to host %s",
            fd, addrForErrors);

   return fd;
}

static int proxyv3_getfd(const struct sockaddr *host,
                         const socklen_t socklen,
                         uint16_t port) {
   if (pthread_mutex_lock(&rpcLock) != 0) {
      LogCrit(COMPONENT_FSAL,
              "pthread_mutex_lock failed %d %s",
              errno, strerror(errno));
      return -1;
   }

   int fd = -1;
   LogDebug(COMPONENT_FSAL,
            "Looking for an open socket for port %u",
            port);

   // Find the first free, preferably open socket
   struct fd_entry *first_free = NULL;
   struct fd_entry *first_open = NULL;

   for (size_t i = 0; i < kMaxSockets; i++) {
      struct fd_entry *entry = &fd_entries[i];

      if (entry->in_use) continue;

      // Remember that we saw a free slot.
      if (first_free == NULL) {
         first_free = entry;
      }

      if (!entry->is_open) {
         // This is a free and not even opened slot, prefer that so other people
         // can get socket reuse.
         first_free = entry;
      } else {
         // See if this open socket matches what we need.
         if (entry->socklen == socklen &&
             entry->port == port &&
             memcmp(&entry->socket, host, socklen) == 0) {
            LogDebug(COMPONENT_FSAL,
                     "Found an already open socket, will reuse that");
            first_open = entry;
            break;
         }
      }
   }

   // The list is full! The caller needs to block.
   if (first_free == NULL) {
      LogDebug(COMPONENT_FSAL,
               "No available sockets, tell the caller to wait a while");

      if (pthread_mutex_unlock(&rpcLock) != 0) {
         LogCrit(COMPONENT_FSAL,
                 "pthread_mutex_unlock failed %d %s",
                 errno, strerror(errno));
      }

      return -2;
   }

   if (first_open != NULL) {
      // If we found an open one for us, use that.
      first_open->in_use = true;
      fd = first_open->fd;
   } else {
      // Otherwise, replace first_free with a new one.
      if (first_free->is_open) {
         // We should first close the existing socket.
         LogDebug(COMPONENT_FSAL,
                  "Closing fd %d before we re-use the slot",
                  first_free->fd);

         if (close(first_free->fd) != 0) {
            LogCrit(COMPONENT_FSAL,
                    "close(%d) of re-used fd failed. Continuing. Errno %d (%s)",
                    first_free->fd, errno, strerror(errno));
         }
      }

      // Clear all the fields.
      memset(first_free, 0, sizeof(*first_free));

      fd = proxyv3_openfd(host, socklen, port);
      if (fd > 0) {
         // Record the entry in our list.
         first_free->fd = fd;
         first_free->port = port;
         first_free->in_use = true;
         first_free->is_open = true;
         memcpy(&first_free->socket, host, socklen);
         first_free->socklen = socklen;
         first_free->port = port;
      } // otherwise, fall through to cleanup below.
   }

   // Release the lock before we exit.
   if (pthread_mutex_unlock(&rpcLock) != 0) {
      LogCrit(COMPONENT_FSAL,
              "pthread_mutex_unlock failed %d %s",
              errno, strerror(errno));
      return -1;
   }

   return fd;
}


static int
proxyv3_getfd_blocking(const struct sockaddr *host,
                       const socklen_t socklen,
                      uint16_t port) {
   // Keep trying to get an fd with exponential backoff up to kMaxIterations.
   const unsigned kMaxIterations = 100;
   // So, within a datacenter, it's likely that we'll need to wait about 1
   // millisecond for someone to finish. Let's start the backoff sooner though
   // at 256 microseconds, because while an end-to-end op is 1ms, people should
   // be finishing all the time.
   size_t numMicros = 256;

   for (size_t i = 0; i < kMaxIterations; i++) {
      int fd = proxyv3_getfd(host, socklen, port);

      // If we got back a valid fd, return it immediately.
      if (fd > 0) return fd;

      // We got back an error, return it.
      if (fd != -2) return fd;

      // We were told to pause.
      struct timespec how_long = {
         .tv_sec  = numMicros / 1000000, /* 1M micros per second */
         .tv_nsec = (numMicros % 1000000) * 1000 /* Remainder => nanoseconds */
      };

      LogDebug(COMPONENT_FSAL,
               "Going to sleep for %zu microseconds",
               numMicros);

      if (nanosleep(&how_long, NULL) != 0) {
         // Let interrupts wake us up and not care. Anything else should be fatal.
         if (errno != EINTR) {
            LogCrit(COMPONENT_FSAL,
                    "nanosleep failed. Asked for %zu micros. Errno %d (%s)",
                    numMicros, errno, strerror(errno));
            return -1;
         }
      }

      // Next time around, double it. TODO(boulos): Jitter?
      numMicros *= 2;
   }

   LogCrit(COMPONENT_FSAL,
           "Failed to ever acquire a new fd, dying");
   return -1;
}


// Release an fd back to our pool.
static bool proxyv3_releasefd(int fd, bool force_close) {
   LogDebug(COMPONENT_FSAL,
            "Releasing fd %d back into the pool (close = %s)",
            fd, (force_close) ? "T" : "F");

   if (pthread_mutex_lock(&rpcLock) != 0) {
      LogCrit(COMPONENT_FSAL,
              "pthread_mutex_lock failed %d %s",
              errno, strerror(errno));
      return false;
   }

   struct fd_entry *entry = NULL;
   for (size_t i = 0; i < kMaxSockets; i++) {
      if (!fd_entries[i].in_use) continue;
      if (fd_entries[i].fd != fd) continue;

      entry = &fd_entries[i];
      break;
   }

   if (entry == NULL) {
      LogCrit(COMPONENT_FSAL,
              "proxyv3_closefd: fd %d wasn't in the list",
              fd);
   } else {
      // Mark it as no longer in use. (But leave it open, unless asked not to).
      entry->in_use = false;
      if (force_close) {
         // Close the socket first.
         if (close(entry->fd) < 0) {
            LogCrit(COMPONENT_FSAL,
                    "close(%d) failed. Errno %d (%s)",
                    entry->fd, errno, strerror(errno));
         }
         // Blast all the state (marks it as neither open nor in use).
         memset(entry, 0, sizeof(*entry));
      }
   }

   if (pthread_mutex_unlock(&rpcLock) != 0) {
      LogCrit(COMPONENT_FSAL,
              "pthread_mutex_unlock failed %d %s",
              errno, strerror(errno));
      return false;
   }

   // If we didn't find it, return false.
   return entry != NULL;
}




// NOTE(boulos): This is basically rpc_call redone by hand, because
// ganesha's "nfsd" hijacks the RPC setup to the point where we can't
// issue our own NFS-related rpcs as a simple client.
bool proxyv3_call(const struct sockaddr *host,
                  const socklen_t socklen,
                  uint16_t port,
                  const struct user_cred *creds,
                  const rpcprog_t rpcProgram,
                  const rpcvers_t rpcVersion,
                  const rpcproc_t rpcProc,
                  const xdrproc_t encodeFunc, const void *args,
                  const xdrproc_t decodeFunc, void *output) {
   XDR x;
   struct rpc_msg rmsg;
   struct rpc_msg reply;

   // Make a little buffer that's big enough for handling most
   // requests/responses. TODO(boulos): Move this into a
   // per-<something> allocator / reuse it.
   const uint kHeaderPadding = 512;
   const uint kBufSize = PROXY_V3.module.fs_info.maxwrite + kHeaderPadding;
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

   // Ready to start sending, let's get an open socket.
   int fd = proxyv3_getfd_blocking(host, socklen, port);
   if (fd < 0) {
      LogCrit(COMPONENT_FSAL,
              "Failed to get open fd");
      gsh_free(msgbuf);
      AUTH_DESTROY(au);
      return false;
   }

   size_t total_bytes_written = 0;
   while (total_bytes_written < bytes_to_send) {
      size_t remaining = bytes_to_send - total_bytes_written;
      ssize_t bytes_written =
         write(fd,
               msgbuf + total_bytes_written,
               remaining);
      if (bytes_written < 0) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: Write at %zu failed (remaining was %zu). Errno %d (%s)",
                 total_bytes_written, remaining, errno, strerror(errno));
         gsh_free(msgbuf);
         AUTH_DESTROY(au);
         proxyv3_releasefd(fd, true /* force close, likely busted */);
         return false;
      }

      total_bytes_written += bytes_written;
   }

   // We can cleanup the auth struct, we'll just be reading from here on out.
   AUTH_DESTROY(au);

   // Aww, short write. Exit.
   if (total_bytes_written != bytes_to_send) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Only wrote %zu bytes out of %zu",
              total_bytes_written, bytes_to_send);
      gsh_free(msgbuf);
      proxyv3_releasefd(fd, true /* force close, likely busted */);
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
      proxyv3_releasefd(fd, true /* force close, likely busted */);
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
      proxyv3_releasefd(fd, true /* force close, likely busted */);
      return false;
   }

   // Clear the top bit of the recmark
   response_header.recmark &= ~(1U << 31);
   if (response_header.recmark < 8) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Response claims to only have %u bytes",
              response_header.recmark);
      gsh_free(msgbuf);
      proxyv3_releasefd(fd, true /* force close, likely busted */);
      return false;
   }

   // We've already read the header (record mark) and xid.
   size_t bytes_to_read = response_header.recmark;
   size_t total_bytes_read = 4;
   size_t read_buffer_size = bytes_to_read + sizeof(xid);

   // Resize the buffer to let us slurp the whole response back.
   msgbuf = gsh_realloc(msgbuf, read_buffer_size);
   memset(msgbuf, 0, read_buffer_size);
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
         proxyv3_releasefd(fd, true /* force close, likely busted */);
         return false;
      }

      total_bytes_read += bytes_read;
   }

   // All done reading, release the fd back to the pool.
   proxyv3_releasefd(fd, false /* the socket is reusable */);

   // Aww, short read. Exit.
   if (total_bytes_read != bytes_to_read) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Only read %zu bytes out of %zu",
              total_bytes_read, bytes_to_read);
      gsh_free(msgbuf);
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
      return false;
   }

   // Check that it was accepted, if not, say why not.
   if (reply.rm_reply.rp_stat != MSG_ACCEPTED) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Reply received but not accepted. REJ %d",
              reply.rm_reply.rp_rjct.rj_stat);
      gsh_free(msgbuf);
      return false;
   }

   // Check that it was accepted with success.
   if (reply.rm_reply.rp_acpt.ar_stat != SUCCESS) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Reply accepted but unsuccesful. Reason %d",
              reply.rm_reply.rp_acpt.ar_stat);
      gsh_free(msgbuf);
      return false;
   }

   gsh_free(msgbuf);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: RPC completed successfully");

   return true;
}

// Helpful wrappers around the generic RPC call so that we don't need
// to repeatedly pass in the program and version constants.
bool proxyv3_nfs_call(const struct sockaddr *host,
                      const socklen_t socklen,
                      const uint nfsdPort,
                      const struct user_cred *creds,
                      const rpcproc_t nfsProc,
                      const xdrproc_t encodeFunc, const void *args,
                      const xdrproc_t decodeFunc, void *output) {
   const int kProgramNFS = NFS_PROGRAM;
   const int kVersionNFSv3 = NFS_V3;

   return proxyv3_call(host, socklen, nfsdPort, creds,
                       kProgramNFS, kVersionNFSv3,
                       nfsProc, encodeFunc, args, decodeFunc, output);
}

bool proxyv3_mount_call(const struct sockaddr *host,
                        const socklen_t socklen,
                        const uint mountdPort,
                        const struct user_cred *creds,
                        const rpcproc_t mountProc,
                        const xdrproc_t encodeFunc, const void *args,
                        const xdrproc_t decodeFunc, void *output) {
   const int kProgramMount = MOUNTPROG;
   const int kVersionMountv3 = MOUNT_V3;

   return proxyv3_call(host, socklen, mountdPort, creds,
                       kProgramMount, kVersionMountv3,
                       mountProc, encodeFunc, args, decodeFunc, output);
}

bool proxyv3_nlm_call(const struct sockaddr *host,
                      const socklen_t socklen,
                      const uint nlmPort,
                      const struct user_cred *creds,
                      const rpcproc_t nlmProc,
                      const xdrproc_t encodeFunc, const void *args,
                      const xdrproc_t decodeFunc, void *output) {
   const int kProgramNLM = NLMPROG;
   const int kVersionNLMv4 = NLM4_VERS;

   return proxyv3_call(host, socklen, nlmPort, creds,
                       kProgramNLM, kVersionNLMv4,
                       nlmProc, encodeFunc, args, decodeFunc, output);
}


// Ask portmapd for where MOUNTD and NFSD are running.
bool proxyv3_find_ports(const struct sockaddr *host,
                        const socklen_t socklen,
                        u_int *mountd_port,
                        u_int *nfsd_port,
                        u_int *nlm_port) {
   struct pmap mountd_query = {
      .pm_prog = MOUNTPROG,
      .pm_vers = MOUNT_V3,
      .pm_prot = IPPROTO_TCP,
      .pm_port = 0 /* ignored for getport */
   };

   struct pmap nfsd_query = {
      .pm_prog = NFS_PROGRAM,
      .pm_vers = NFS_V3,
      .pm_prot = IPPROTO_TCP,
      .pm_port = 0 /* ignored */
   };

   struct pmap nlm_query = {
      .pm_prog = NLMPROG,
      .pm_vers = NLM4_VERS,
      .pm_prot = IPPROTO_TCP,
      .pm_port = 0 /* ignored */
   };

   struct {
      struct pmap *input;
      u_int *port;
      const char *name;
   } queries[] = {
      { &mountd_query, mountd_port, "mountd" },
      { &nfsd_query, nfsd_port, "nfsd" },
      // If we put NLM last, we can technically let it just warn in debug mode.
      { &nlm_query, nlm_port, "nlm" }
   };

   for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); i++) {
      LogDebug(COMPONENT_FSAL,
               "Asking portmap to tell us what the %s/tcp port is",
               queries[i].name);

      if (!proxyv3_call(host, socklen, PMAPPORT, NULL /* no auth for portmapd */,
                        PMAPPROG, PMAPVERS,
                        PMAPPROC_GETPORT,
                        (xdrproc_t) xdr_pmap, queries[i].input,
                        (xdrproc_t) xdr_u_int, queries[i].port)) {
         LogDebug(COMPONENT_FSAL,
                  "Failed to find %s", queries[i].name);
         return false;
      }

      LogDebug(COMPONENT_FSAL,
               "Got back %s port %u",
               queries[i].name, *queries[i].port);
   }

   return true;
}
