/**
 * @file  nfs_rpc_dispatcher_thread.c
 * @brief Contains the @c rpc_dispatcher_thread routine and support code
 *
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 * ---------------------------------------
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>		/* for having FNDELAY */
#include <sys/select.h>
#include <poll.h>
#include <assert.h>
#include "hashtable.h"
#include "log.h"
#include "gsh_rpc.h"
#include "abstract_atomic.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nlm4.h"
#include "rquota.h"
#include "nfs_init.h"
#include "nfs_core.h"
#include "nfs_convert.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_proto_functions.h"
#include "nfs_req_queue.h"
#include "nfs_dupreq.h"
#include "nfs_file_handle.h"
#include "fridgethr.h"
#include "nfs_rpc_dispatcher.h"

static struct rpc_evchan rpc_evchan[N_EVENT_CHAN];

struct fridgethr *req_fridge;	/*< Decoder thread pool */
struct nfs_req_st nfs_req_st;	/*< Shared request queues */

static bool nfs_rpc_getreq_ng(SVCXPRT *xprt /*, int chan_id */);
static void nfs_rpc_free_user_data(SVCXPRT *xprt);

struct proto_data pdata[P_COUNT];
struct netconfig *netconfig_udpv4;
struct netconfig *netconfig_tcpv4;
struct netconfig *netconfig_udpv6;
struct netconfig *netconfig_tcpv6;

/* RPC Service Sockets and Transports */
int udp_socket[P_COUNT];
int tcp_socket[P_COUNT];
SVCXPRT *udp_xprt[P_COUNT];
SVCXPRT *tcp_xprt[P_COUNT];

/* Flag to indicate if V6 interfaces on the host are enabled */
#ifdef __FreeBSD__
bool v6disabled = true;
#else
bool v6disabled = false;
#endif

/**
 * @brief Function never called, but the symbol is needed for svc_register.
 *
 * @param[in] ptr_req Unused
 * @param[in] ptr_svc Unused
 */
void nfs_rpc_dispatch_dummy(struct svc_req *ptr_req, SVCXPRT *ptr_svc)
{
	LogMajor(COMPONENT_DISPATCH,
		 "NFS DISPATCH DUMMY: Possible error, function nfs_rpc_dispatch_dummy should never be called");
}

/**
 * @brief Unregister an RPC program.
 *
 * @param[in] prog  Program to unregister
 * @param[in] vers1 Lowest version
 * @param[in] vers2 Highest version
 */
static void unregister(const rpcprog_t prog, const rpcvers_t vers1,
		       const rpcvers_t vers2)
{
	rpcvers_t vers;

	for (vers = vers1; vers <= vers2; vers++) {
		rpcb_unset(prog, vers, netconfig_udpv4);
		rpcb_unset(prog, vers, netconfig_tcpv4);
		if (netconfig_udpv6)
			rpcb_unset(prog, vers, netconfig_udpv6);
		if (netconfig_tcpv6)
			rpcb_unset(prog, vers, netconfig_tcpv6);
	}
}

static void unregister_rpc(void)
{
	if ((nfs_param.core_param.core_options & CORE_OPTION_NFSV3) != 0) {
		unregister(nfs_param.core_param.program[P_NFS], NFS_V2, NFS_V4);
		unregister(nfs_param.core_param.program[P_MNT], MOUNT_V1,
			   MOUNT_V3);
	} else {
		unregister(nfs_param.core_param.program[P_NFS], NFS_V4, NFS_V4);
	}
	if (nfs_param.core_param.enable_NLM)
		unregister(nfs_param.core_param.program[P_NLM], 1, NLM4_VERS);
	if (nfs_param.core_param.enable_RQUOTA) {
		unregister(nfs_param.core_param.program[P_RQUOTA], RQUOTAVERS,
			   EXT_RQUOTAVERS);
	}
}

/**
 * @brief Close file descriptors used for RPC services.
 *
 * So that restarting the NFS server wont encounter issues of "Addres
 * Already In Use" - this has occured even though we set the
 * SO_REUSEADDR option when restarting the server with a single export
 * (i.e.: a small config) & no logging at all, making the restart very
 * fast.  when closing a listening socket it will be closed
 * immediately if no connection is pending on it, hence drastically
 * reducing the probability for trouble.
 */
static void close_rpc_fd(void)
{
	enum protos p;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (udp_socket[p] != -1)
			close(udp_socket[p]);
		if (tcp_socket[p] != -1)
			close(tcp_socket[p]);
	}
}

/**
 * @brief Rendezvous callout.  This routine will be called by TI-RPC
 *        after newxprt has been accepted.
 *
 * Register newxprt on a TCP event channel.  Balancing events/channels
 * could become involved.  To start with, just cycle through them as
 * new connections are accepted.
 *
 * @param[in] xprt    Transport
 * @param[in] newxprt Newly created transport
 * @param[in] flags   Unused
 * @param[in] u_data  Whatever
 *
 * @return Always returns 0.
 */
static u_int nfs_rpc_recv_user_data(SVCXPRT *xprt, SVCXPRT *newxprt,
				    const u_int flags, void *u_data)
{
	static uint32_t next_chan = TCP_EVCHAN_0;
	static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	uint32_t tchan;

	PTHREAD_MUTEX_lock(&mtx);

	tchan = next_chan;
	assert((next_chan >= TCP_EVCHAN_0) && (next_chan < N_EVENT_CHAN));
	if (++next_chan >= N_EVENT_CHAN)
		next_chan = TCP_EVCHAN_0;

	/* setup private data (freed when xprt is destroyed) */
	newxprt->xp_u1 =
	    alloc_gsh_xprt_private(newxprt, XPRT_PRIVATE_FLAG_NONE);

	/* NB: xu->drc is allocated on first request--we need shared
	 * TCP DRC for v3, but per-connection for v4 */

	PTHREAD_MUTEX_unlock(&mtx);

	(void)svc_rqst_evchan_reg(rpc_evchan[tchan].chan_id, newxprt,
				  SVC_RQST_FLAG_NONE);

	return 0;
}

static void create_udp(enum protos prot)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;

	udp_xprt[prot] = svc_dg_create(udp_socket[prot],
				       ncp->rpc.max_send_buffer_size,
				       ncp->rpc.max_recv_buffer_size);

	if (udp_xprt[prot] == NULL)
		LogFatal(COMPONENT_DISPATCH, "Cannot allocate %s/UDP SVCXPRT",
			 tags[prot]);

	/* Hook xp_getreq */
	(void)SVC_CONTROL(udp_xprt[prot], SVCSET_XP_GETREQ, nfs_rpc_getreq_ng);

	/* Hook xp_free_user_data (finalize/free private data) */
	(void)SVC_CONTROL(udp_xprt[prot], SVCSET_XP_FREE_USER_DATA,
			  nfs_rpc_free_user_data);

	/* Setup private data */
	(udp_xprt[prot])->xp_u1 =
		alloc_gsh_xprt_private(udp_xprt[prot], XPRT_PRIVATE_FLAG_NONE);

	/* bind xprt to channel--unregister it from the global event
	 * channel (if applicable) */
	(void)svc_rqst_evchan_reg(rpc_evchan[UDP_EVENT_CHAN].chan_id,
				  udp_xprt[prot], SVC_RQST_FLAG_XPRT_UREG);
}

static void create_tcp(enum protos prot)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;

	tcp_xprt[prot] = svc_vc_create2(tcp_socket[prot],
					ncp->rpc.max_send_buffer_size,
					ncp->rpc.max_recv_buffer_size,
					SVC_VC_CREATE_LISTEN);

	if (tcp_xprt[prot] == NULL)
		LogFatal(COMPONENT_DISPATCH, "Cannot allocate %s/TCP SVCXPRT",
			 tags[prot]);

	/* bind xprt to channel--unregister it from the global event
	 * channel (if applicable) */
	(void)svc_rqst_evchan_reg(rpc_evchan[TCP_RDVS_CHAN].chan_id,
				  tcp_xprt[prot], SVC_RQST_FLAG_XPRT_UREG);

	/* Hook xp_getreq */
	(void)SVC_CONTROL(tcp_xprt[prot], SVCSET_XP_GETREQ, nfs_rpc_getreq_ng);

	/* Hook xp_recv_user_data -- allocate new xprts to event channels */
	(void)SVC_CONTROL(tcp_xprt[prot], SVCSET_XP_RECV_USER_DATA,
			  nfs_rpc_recv_user_data);

	/* Hook xp_free_user_data (finalize/free private data) */
	(void)SVC_CONTROL(tcp_xprt[prot], SVCSET_XP_FREE_USER_DATA,
			  nfs_rpc_free_user_data);

	/* Setup private data */
	(tcp_xprt[prot])->xp_u1 =
		alloc_gsh_xprt_private(tcp_xprt[prot], XPRT_PRIVATE_FLAG_NONE);
}

/**
 * @brief Create the SVCXPRT for each protocol in use
 */
static void create_SVCXPRTs(void)
{
	enum protos p;

	LogFullDebug(COMPONENT_DISPATCH, "Allocation of the SVCXPRT");
	for (p = P_NFS; p < P_COUNT; p++)
		if (test_for_additional_nfs_protocols(p)) {
			create_udp(p);
			create_tcp(p);
		}
}

/**
 * @brief Bind the udp and tcp sockets for V6 Interfaces
 */
static void bind_sockets_V6(void)
{
	struct proto_data *pdatap = NULL;
	enum protos p;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (!test_for_additional_nfs_protocols(p))
			continue;

		pdatap = &pdata[p];
		memset(&pdatap->sinaddr_udp6, 0, sizeof(pdatap->sinaddr_udp6));

		pdatap->sinaddr_udp6.sin6_family = AF_INET6;
		/* all interfaces */
		pdatap->sinaddr_udp6.sin6_addr = in6addr_any;
		pdatap->sinaddr_udp6.sin6_port =
		    htons(nfs_param.core_param.port[p]);

		pdatap->netbuf_udp6.maxlen = sizeof(pdatap->sinaddr_udp6);
		pdatap->netbuf_udp6.len = sizeof(pdatap->sinaddr_udp6);
		pdatap->netbuf_udp6.buf = &pdatap->sinaddr_udp6;

		pdatap->bindaddr_udp6.qlen = SOMAXCONN;
		pdatap->bindaddr_udp6.addr = pdatap->netbuf_udp6;

		if (!__rpc_fd2sockinfo(udp_socket[p], &pdatap->si_udp6)) {
			LogWarn(COMPONENT_DISPATCH,
				 "Cannot get %s socket info for udp6 socket errno=%d (%s)",
				 tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V4 interface. Cannot continue.");
		}

		if (bind(udp_socket[p],
			 (struct sockaddr *)pdatap->bindaddr_udp6.addr.buf,
			 (socklen_t) pdatap->si_udp6.si_alen)) {
			LogWarn(COMPONENT_DISPATCH,
				 "Cannot bind %s udp6 socket, error %d (%s)",
				 tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V4 interface. Cannot continue.");
		}

		memset(&pdatap->sinaddr_tcp6, 0, sizeof(pdatap->sinaddr_tcp6));

		pdatap->sinaddr_tcp6.sin6_family = AF_INET6;
		/* all interfaces */
		pdatap->sinaddr_tcp6.sin6_addr = in6addr_any;
		pdatap->sinaddr_tcp6.sin6_port =
		    htons(nfs_param.core_param.port[p]);

		pdatap->netbuf_tcp6.maxlen = sizeof(pdatap->sinaddr_tcp6);
		pdatap->netbuf_tcp6.len = sizeof(pdatap->sinaddr_tcp6);
		pdatap->netbuf_tcp6.buf = &pdatap->sinaddr_tcp6;

		pdatap->bindaddr_tcp6.qlen = SOMAXCONN;
		pdatap->bindaddr_tcp6.addr = pdatap->netbuf_tcp6;

		if (!__rpc_fd2sockinfo(tcp_socket[p], &pdatap->si_tcp6)) {
			LogWarn(COMPONENT_DISPATCH,
				 "Cannot get %s socket info for tcp6 socket errno=%d (%s)",
				 tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V4 interface. Cannot continue.");
		}

		if (bind(tcp_socket[p],
			 (struct sockaddr *) pdatap->bindaddr_tcp6.addr.buf,
			 (socklen_t) pdatap->si_tcp6.si_alen)) {
			LogWarn(COMPONENT_DISPATCH,
				"Cannot bind %s tcp6 socket, error %d (%s)",
				tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V4 interface. Cannot continue.");
		}
	}
}

/**
 * @brief Bind the udp and tcp sockets for V4 Interfaces
 */
static void bind_sockets_V4(void)
{
	struct proto_data *pdatap = NULL;
	enum protos p;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (!test_for_additional_nfs_protocols(p))
			continue;

		pdatap = &pdata[p];
		memset(&pdatap->sinaddr_udp, 0, sizeof(pdatap->sinaddr_udp));

		pdatap->sinaddr_udp.sin_family = AF_INET;
		/* all interfaces */
		pdatap->sinaddr_udp.sin_addr.s_addr = htonl(INADDR_ANY);
		pdatap->sinaddr_udp.sin_port =
		    htons(nfs_param.core_param.port[p]);

		pdatap->netbuf_udp6.maxlen = sizeof(pdatap->sinaddr_udp);
		pdatap->netbuf_udp6.len = sizeof(pdatap->sinaddr_udp);
		pdatap->netbuf_udp6.buf = &pdatap->sinaddr_udp;

		pdatap->bindaddr_udp6.qlen = SOMAXCONN;
		pdatap->bindaddr_udp6.addr = pdatap->netbuf_udp6;

		if (!__rpc_fd2sockinfo(udp_socket[p], &pdatap->si_udp6)) {
			LogWarn(COMPONENT_DISPATCH,
				"Cannot get %s socket info for udp6 socket errno=%d (%s)",
				tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V6 interface. Cannot continue.");
		}

		if (bind(udp_socket[p],
			 (struct sockaddr *) pdatap->bindaddr_udp6.addr.buf,
			 (socklen_t) pdatap->si_udp6.si_alen)) {
			LogWarn(COMPONENT_DISPATCH,
				"Cannot bind %s udp6 socket, error %d (%s)",
				tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V6 interface. Cannot continue.");
		}

		memset(&pdatap->sinaddr_tcp, 0, sizeof(pdatap->sinaddr_tcp));
		pdatap->sinaddr_tcp.sin_family = AF_INET;
		/* all interfaces */
		pdatap->sinaddr_tcp.sin_addr.s_addr = htonl(INADDR_ANY);
		pdatap->sinaddr_tcp.sin_port =
		    htons(nfs_param.core_param.port[p]);

		pdatap->netbuf_tcp6.maxlen = sizeof(pdatap->sinaddr_tcp);
		pdatap->netbuf_tcp6.len = sizeof(pdatap->sinaddr_tcp);
		pdatap->netbuf_tcp6.buf = &pdatap->sinaddr_tcp;

		pdatap->bindaddr_tcp6.qlen = SOMAXCONN;
		pdatap->bindaddr_tcp6.addr = pdatap->netbuf_tcp6;

		if (!__rpc_fd2sockinfo(tcp_socket[p], &pdatap->si_tcp6)) {
			LogWarn(COMPONENT_DISPATCH,
				"V4 : Cannot get %s socket info for tcp socket error %d(%s)",
				tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V6 interface. Cannot continue.");
		}

		if (bind(tcp_socket[p],
			 (struct sockaddr *) pdatap->bindaddr_tcp6.addr.buf,
			 (socklen_t) pdatap->si_tcp6.si_alen)) {
			LogWarn(COMPONENT_DISPATCH,
				"Cannot bind %s tcp socket, error %d(%s)",
				tags[p], errno, strerror(errno));
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V6 interface. Cannot continue.");
		}
	}
}

/**
 * @brief Function to set the socket options on the allocated
 *	  udp and tcp sockets
 *
 */
static void alloc_socket_setopts(int p)
{
	int one = 1;

	/* Use SO_REUSEADDR in order to avoid wait
	 * the 2MSL timeout */
	if (setsockopt(udp_socket[p], SOL_SOCKET, SO_REUSEADDR, &one,
		       sizeof(one))) {
		LogWarn(COMPONENT_DISPATCH,
			"Bad udp socket options for %s, error %d(%s)",
			tags[p], errno, strerror(errno));
		LogFatal(COMPONENT_DISPATCH,
			 "Error setting socket option for proto %d, %s",
			 p, tags[p]);
	}

	if (setsockopt(tcp_socket[p], SOL_SOCKET, SO_REUSEADDR, &one,
		       sizeof(one))) {
		LogWarn(COMPONENT_DISPATCH,
			"Bad tcp socket options for %s, error %d(%s)",
			tags[p], errno, strerror(errno));
		LogFatal(COMPONENT_DISPATCH,
			 "Error setting socket option for proto %d, %s",
			 p, tags[p]);
	}

	/* We prefer using non-blocking socket
	 * in the specific case */
	if (fcntl(udp_socket[p], F_SETFL, FNDELAY) == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"Cannot set udp socket for %s as non blocking, error %d(%s)",
			tags[p], errno, strerror(errno));
		LogFatal(COMPONENT_DISPATCH,
			 "Error setting socket option for proto %d, %s",
			 p, tags[p]);
	}
}

/**
 * @brief Allocate the tcp and udp sockets for the nfs daemon
 * using V4 interfaces
 */
static void allocate_sockets_V4(int p)
{
	udp_socket[p] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_socket[p] == -1) {
		if (errno == EAFNOSUPPORT) {
			LogInfo(COMPONENT_DISPATCH,
				"No V6 and V4 intfs configured?!");
		}

		LogWarn(COMPONENT_DISPATCH,
			"Cannot allocate a udp socket for %s, error %d(%s)",
			tags[p], errno, strerror(errno));
		LogFatal(COMPONENT_DISPATCH,
			 "Error allocating V4 socket for proto %d, %s",
			 p, tags[p]);
	}

	tcp_socket[p] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (tcp_socket[p] == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"Cannot allocate a tcp socket for %s, error %d(%s)",
			tags[p], errno, strerror(errno));
		LogFatal(COMPONENT_DISPATCH,
			 "Error allocating V4 socket for proto %d, %s",
			 p, tags[p]);
	}
}

/**
 * @brief Allocate the tcp and udp sockets for the nfs daemon
 */
static void allocate_sockets(void)
{
	enum protos p;
	int	rc = 0;

	LogFullDebug(COMPONENT_DISPATCH, "Allocation of the sockets");

	for (p = P_NFS; p < P_COUNT; p++) {
		if (!test_for_additional_nfs_protocols(p))
			continue;

		/* Initialize all the sockets to -1 because
		 * it makes some code later easier */
		udp_socket[p] = -1;
		tcp_socket[p] = -1;

		if (v6disabled) {
			allocate_sockets_V4(p);
			alloc_socket_setopts(p);
		}

		udp_socket[p] = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

		if (udp_socket[p] == -1) {
			/*
			 * We assume that EAFNOSUPPORT points to the likely
			 * case when the host has V6 interfaces disabled.
			 * So we will try to use the existing V4 interfaces
			 * instead.
			 */
			if (errno == EAFNOSUPPORT) {
				v6disabled = true;
				LogWarn(COMPONENT_DISPATCH,
					"System may not have V6 intfs configured error %d(%s)",
					errno, strerror(errno));
				allocate_sockets_V4(p);
				alloc_socket_setopts(p);
				return;
			}

			LogFatal(COMPONENT_DISPATCH,
				 "Cannot allocate a udp socket for %s, error %d(%s)",
				 tags[p], errno, strerror(errno));
		}

		tcp_socket[p] = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

		/* We fail with LogFatal here on error because it shouldn't be
		 * that we have managed to create a V6 based udp socket and
		 * have failed for the tcp sock. If it were a case of V6 being
		 * disabled, then we would have encountered that case with the
		 * first udp sock create and would have moved on to create the
		 * V4 sockets.
		 */
		if (tcp_socket[p] == -1)
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot allocate a tcp socket for %s, error %d(%s)",
				 tags[p], errno, strerror(errno));

		alloc_socket_setopts(p);
	}
}

/* The following routine must ONLY be called from the shutdown
 * thread */
void Clean_RPC(void)
{
  /**
   * @todo Consider the need to call Svc_dg_destroy for UDP & ?? for
   * TCP based services
   */
	unregister_rpc();
	close_rpc_fd();
}

static void register_program(protos prot, int flag, int vers)
{
	if ((nfs_param.core_param.core_options & flag) == 0)
		return;

	LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/UDP", tags[prot], vers);

	/* XXXX fix svc_register! */
	if (!UDP_REGISTER(prot, vers, netconfig_udpv4))
		LogFatal(COMPONENT_DISPATCH, "Cannot register %s V%d on UDP",
			 tags[prot], vers);

	if (netconfig_udpv6) {
		LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/UDPv6",
			tags[prot], vers);
		if (!UDP_REGISTER(prot, vers, netconfig_udpv6))
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot register %s V%d on UDPv6", tags[prot],
				 vers);
	}

#ifndef _NO_TCP_REGISTER
	LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/TCP", tags[prot], vers);

	if (!TCP_REGISTER(prot, vers, netconfig_tcpv4))
		LogFatal(COMPONENT_DISPATCH, "Cannot register %s V%d on TCP",
			 tags[prot], vers);

	if (netconfig_tcpv6) {
		LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/TCPv6",
			tags[prot], vers);
		if (!TCP_REGISTER(prot, vers, netconfig_tcpv6))
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot register %s V%d on TCPv6", tags[prot],
				 vers);
	}
#endif				/* _NO_TCP_REGISTER */
}

static void nfs_init_svc_params(void)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;
	svc_init_params svc_params = {0};

	/* New TI-RPC package init function */
	svc_params.flags = SVC_INIT_EPOLL;	      /* use EPOLL event mgmt */
	svc_params.flags |= SVC_INIT_NOREG_XPRTS; /* don't call xprt_register */
	svc_params.max_connections = ncp->rpc.max_connections;
	svc_params.max_events = 1024;	       /* length of epoll event queue */
	svc_params.svc_ioq_maxbuf = ncp->rpc.max_send_buffer_size;
	svc_params.idle_timeout = ncp->rpc.idle_timeout_s;
	svc_params.warnx = NULL;
	svc_params.gss_ctx_hash_partitions = 17;
	svc_params.gss_max_idle_gen = 1024;       /* GSS ctx cache expiration */
	svc_params.gss_max_gc = 200;
	svc_params.ioq_thrd_max = ncp->rpc.ioq_thrd_max;

	if (!svc_init(&svc_params))
		LogFatal(COMPONENT_INIT, "SVC initialization failed");
}

static void nfs_init_netconfig(void)
{
	/* Get the netconfig entries from /etc/netconfig */
	netconfig_udpv4 = (struct netconfig *)getnetconfigent("udp");
	if (netconfig_udpv4 == NULL)
		LogFatal(COMPONENT_DISPATCH,
			 "Cannot get udp4 netconfig. Check file /etc/netconfig...");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_tcpv4 = (struct netconfig *)getnetconfigent("tcp");
	if (netconfig_tcpv4 == NULL)
		LogFatal(COMPONENT_DISPATCH,
			 "Cannot get tcp4 netconfig. Check file /etc/netconfig...");

	/* A short message to show that /etc/netconfig parsing was a success */
	LogFullDebug(COMPONENT_DISPATCH, "netconfig found for UDPv4 and TCPv4");

	LogInfo(COMPONENT_DISPATCH, "NFS INIT: Using IPv6");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_udpv6 = (struct netconfig *)getnetconfigent("udp6");
	if (netconfig_udpv6 == NULL)
		LogInfo(COMPONENT_DISPATCH,
			"Cannot get udp6 netconfig. Check file /etc/netconfig...");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_tcpv6 = (struct netconfig *)getnetconfigent("tcp6");
	if (netconfig_tcpv6 == NULL)
		LogInfo(COMPONENT_DISPATCH,
			"Cannot get tcp6 netconfig. Check file /etc/netconfig...");

	/* A short message to show that /etc/netconfig parsing was a success
	 * for ipv6
	 */
	if (netconfig_udpv6 && netconfig_tcpv6)
		LogFullDebug(COMPONENT_DISPATCH,
			     "netconfig found for UDPv6 and TCPv6");
}

static void nfs_init_tirpc(void)
{
	uint32_t tirpc_debug_flags = nfs_param.core_param.rpc.debug_flags;

	/* Redirect TI-RPC allocators, log channel */
	if (!tirpc_control(TIRPC_SET_WARNX, (warnx_t) rpc_warnx))
		LogCrit(COMPONENT_INIT, "Failed redirecting TI-RPC __warnx");

	if (!tirpc_control(TIRPC_SET_DEBUG_FLAGS, &tirpc_debug_flags))
		LogCrit(COMPONENT_INIT, "Failed setting TI-RPC debug flags");

#define TIRPC_SET_ALLOCATORS 0
#if TIRPC_SET_ALLOCATORS
	if (!tirpc_control(TIRPC_SET_MALLOC, (mem_alloc_t) gsh_malloc))
		LogCrit(COMPONENT_INIT, "Failed redirecting TI-RPC alloc");

	if (!tirpc_control(TIRPC_SET_MEM_FREE, (mem_free_t) gsh_free_size))
		LogCrit(COMPONENT_INIT, "Failed redirecting TI-RPC mem_free");

	if (!tirpc_control(TIRPC_SET_FREE, (std_free_t) gsh_free))
		LogCrit(COMPONENT_INIT, "Failed redirecting TI-RPC __free");
#endif				/* TIRPC_SET_ALLOCATORS */

}

static void nfs_have_gssapi(void)
{
	const struct nfs_krb5_param *nkp = &nfs_param.krb5_param;
#ifdef _HAVE_GSSAPI
	/* Acquire RPCSEC_GSS basis if needed */
	if (!nkp->active_krb5)
		return;

	if (!svcauth_gss_import_name(nkp->svc.principal)) {
		LogFatal(COMPONENT_DISPATCH,
			 "Could not import principal name %s into GSSAPI",
			 nkp->svc.principal);
	}

	LogInfo(COMPONENT_DISPATCH,
		"Successfully imported principal %s into GSSAPI",
		nkp->svc.principal);

	/* Trying to acquire a credentials
	 * for checking name's validity */
	if (!svcauth_gss_acquire_cred())
		LogCrit(COMPONENT_DISPATCH,
			"Cannot acquire credentials for principal %s",
			nkp->svc.principal);
	else
		LogDebug(COMPONENT_DISPATCH,
			 "Principal %s is suitable for acquiring credentials",
			 nkp->svc.principal);
#endif				/* _HAVE_GSSAPI */
}

/**
 * @brief Init the svc descriptors for the nfs daemon
 *
 * Perform all the required initialization for the RPC subsystem and event
 * channels.
 */
void nfs_Init_svc(void)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;
	int code = 0;
	int i = 0;

	LogDebug(COMPONENT_DISPATCH, "NFS INIT: Core options = %d",
		 ncp->core_options);
	nfs_rpc_queue_init();  /* Init request queue before RPC stack */
	nfs_init_svc_params();

	LogInfo(COMPONENT_DISPATCH, "NFS INIT: using TIRPC");
	nfs_init_tirpc();

	for (i = 0; i < N_EVENT_CHAN; ++i) {
		rpc_evchan[i].chan_id = 0;
		code = svc_rqst_new_evchan(&rpc_evchan[i].chan_id, NULL,
					   SVC_RQST_FLAG_NONE);
		if (code)
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot create TI-RPC event channel (%d, %d)",
				 i, code);
		/* XXX bail?? */
	}

	nfs_init_netconfig();
	allocate_sockets();  /* Allocate the UDP and TCP sockets for the RPC */
	socket_setoptions(tcp_socket[P_NFS]);

	/* Log that can be useful when debug ONC/RPC and RPCSEC_GSS matter */
	LogDebug(COMPONENT_DISPATCH,
		 "Socket numbers are: nfs_udp=%u  nfs_tcp=%u",
		 udp_socket[P_NFS], tcp_socket[P_NFS]);

	if ((ncp->core_options & CORE_OPTION_NFSV3) != 0) {
		LogDebug(COMPONENT_DISPATCH,
			 "Socket numbers are: mnt_udp=%u mnt_tcp=%u nlm_tcp=%u nlm_udp=%u",
			 udp_socket[P_MNT], tcp_socket[P_MNT],
			 udp_socket[P_NLM], tcp_socket[P_NLM]);
	}

	LogDebug(COMPONENT_DISPATCH,
		 "Socket numbers are: rquota_udp=%u  rquota_tcp=%u",
		 udp_socket[P_RQUOTA], tcp_socket[P_RQUOTA]);

	if ((ncp->core_options & CORE_OPTION_ALL_NFS_VERS) != 0) {
		/* Bind the tcp and udp sockets */
		(v6disabled) ? bind_sockets_V4() : bind_sockets_V6();

		LogInfo(COMPONENT_DISPATCH,
			"bind_sockets() successful, v6disabled = %d",
			v6disabled);

		unregister_rpc();  /* Unregister from portmapper/rpcbind */
		create_SVCXPRTs();  /* Set up well-known xprt handles */
	}

	nfs_have_gssapi();

#ifndef _NO_PORTMAPPER
	/* Perform all the RPC registration, for UDP and TCP,
	 * for NFS_V2, NFS_V3 and NFS_V4 */
	register_program(P_NFS, CORE_OPTION_NFSV3, NFS_V3);
	register_program(P_NFS, CORE_OPTION_NFSV4, NFS_V4);
	register_program(P_MNT, CORE_OPTION_NFSV3, MOUNT_V1);
	register_program(P_MNT, CORE_OPTION_NFSV3, MOUNT_V3);

	if (ncp->enable_NLM)
		register_program(P_NLM, CORE_OPTION_NFSV3, NLM4_VERS);

	if (ncp->enable_RQUOTA && (ncp->core_options & (CORE_OPTION_NFSV3 |
							CORE_OPTION_NFSV4))) {
		register_program(P_RQUOTA, CORE_OPTION_ALL_VERS, RQUOTAVERS);
		register_program(P_RQUOTA, CORE_OPTION_ALL_VERS,
				 EXT_RQUOTAVERS);
	}
#endif				/* _NO_PORTMAPPER */

}

/**
 * @brief Thread used to service an (epoll, etc) event channel.
 *
 * @param[in] arg Poitner to ID of the associated event channel
 *
 * @return Pointer to the result (but this function will mostly loop forever).
 *
 */
static void *rpc_dispatcher_thread(void *arg)
{
	int32_t chan_id = *((int32_t *) arg);

	SetNameFunction("disp");

	/* Calling dispatcher main loop */
	LogInfo(COMPONENT_DISPATCH, "Entering nfs/rpc dispatcher");

	LogDebug(COMPONENT_DISPATCH, "My pthread id is %p",
		 (caddr_t) pthread_self());

	svc_rqst_thrd_run(chan_id, SVC_RQST_FLAG_NONE);

	return NULL;
}				/* rpc_dispatcher_thread */

/**
 * @brief Start service threads
 *
 * @param[in] attr_thr Attributes for started threads
 */
void nfs_rpc_dispatch_threads(pthread_attr_t *attr_thr)
{
	int i;

	/* Start event channel service threads */
	for (i = 0; i < N_EVENT_CHAN; ++i) {
		if (pthread_create(&rpc_evchan[i].thread_id, attr_thr,
				   rpc_dispatcher_thread,
				   (void *)&rpc_evchan[i].chan_id))
			continue;

		LogFatal(COMPONENT_THREAD,
			 "Could not create rpc_dispatcher_thread #%u, error = %d (%s)",
			 i, errno, strerror(errno));
	}
	LogInfo(COMPONENT_THREAD,
		"%d rpc dispatcher threads were started successfully",
		N_EVENT_CHAN);
}

void nfs_rpc_dispatch_stop(void)
{
	int i;

	for (i = 0; i < N_EVENT_CHAN; ++i) {
		svc_rqst_thrd_signal(rpc_evchan[i].chan_id,
				     SVC_RQST_SIGNAL_SHUTDOWN);
	}
}

/**
 * @brief xprt destructor callout
 *
 * @param[in] xprt Transport to destroy
 */
static void nfs_rpc_free_user_data(SVCXPRT *xprt)
{
	if (xprt->xp_u2) {
		nfs_dupreq_put_drc(xprt, xprt->xp_u2, DRC_FLAG_RELEASE);
		xprt->xp_u2 = NULL;
	}
	free_gsh_xprt_private(xprt);
}

static uint32_t nfs_rpc_outstanding_reqs_est(void)
{
	static uint32_t ctr;
	static uint32_t nreqs;
	struct req_q_pair *qpair = NULL;
	uint32_t treqs = 0;
	int i;

	if ((atomic_inc_uint32_t(&ctr) % 10) != 0)
		return atomic_fetch_uint32_t(&nreqs);

	for (i = 0; i < N_REQ_QUEUES; ++i) {
		qpair = &(nfs_req_st.reqs.nfs_request_q.qset[i]);
		treqs += atomic_fetch_uint32_t(&qpair->producer.size);
		treqs += atomic_fetch_uint32_t(&qpair->consumer.size);
	}

	atomic_store_uint32_t(&nreqs, treqs);
	return treqs;
}

static inline bool stallq_should_unstall(SVCXPRT *xprt)
{
	return ((xprt->xp_requests <
		 nfs_param.core_param.dispatch_max_reqs_xprt / 2) ||
		(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED));
}

void thr_stallq(struct fridgethr_context *thr_ctx)
{
	gsh_xprt_private_t *xu;
	struct glist_head *l = NULL;
	struct glist_head *temp = NULL;
	SVCXPRT *xprt;

	while (1) {
		thread_delay_ms(1000);
		PTHREAD_MUTEX_lock(&nfs_req_st.stallq.mtx);

		if (glist_empty(&nfs_req_st.stallq.q)) {
			nfs_req_st.stallq.active = false;
			PTHREAD_MUTEX_unlock(&nfs_req_st.stallq.mtx);
			break;
		}

		glist_for_each_safe(l, temp, &nfs_req_st.stallq.q) {
			xu = glist_entry(l, gsh_xprt_private_t, stallq);
			xprt = xu->xprt;

			if (!stallq_should_unstall(xprt))
				continue;

			/* handle stalled xprts that idle out */

			/* lock ordering (cf. nfs_rpc_cond_stall_xprt) */
			PTHREAD_MUTEX_unlock(&nfs_req_st.stallq.mtx);
			LogDebug(COMPONENT_DISPATCH,
				 "unstalling stalled xprt %p", xprt);
			PTHREAD_MUTEX_lock(&xprt->xp_lock);
			PTHREAD_MUTEX_lock(&nfs_req_st.stallq.mtx);

			/* check that we're still stalled */
			if (xu->flags & XPRT_PRIVATE_FLAG_STALLED) {
				glist_del(&xu->stallq);
				atomic_clear_uint16_t_bits(&xu->flags,
					XPRT_PRIVATE_FLAG_STALLED);
				(void)svc_rqst_rearm_events(xprt,
							    SVC_RQST_FLAG_NONE);
				/* drop stallq ref */
				gsh_xprt_unref(xprt, XPRT_PRIVATE_FLAG_LOCKED,
					       __func__, __LINE__);
			}
		}
		PTHREAD_MUTEX_unlock(&nfs_req_st.stallq.mtx);
	}
	LogDebug(COMPONENT_DISPATCH, "stallq idle, thread exit");
}

static bool nfs_rpc_cond_stall_xprt(SVCXPRT *xprt)
{
	gsh_xprt_private_t *xu;
	uint32_t nreqs = xprt->xp_requests;

	/* check per-xprt quota */
	if (likely(nreqs < nfs_param.core_param.dispatch_max_reqs_xprt)) {
		LogDebug(COMPONENT_DISPATCH,
			 "xprt %p xp_refs %" PRIu32 " has %" PRIu32
			 " reqs active (max %d)", xprt, xprt->xp_refs, nreqs,
			 nfs_param.core_param.dispatch_max_reqs_xprt);
		return false;
	}

	PTHREAD_MUTEX_lock(&xprt->xp_lock);
	xu = (gsh_xprt_private_t *) xprt->xp_u1;

	/* XXX can't happen */
	if (unlikely(xu->flags & XPRT_PRIVATE_FLAG_STALLED)) {
		PTHREAD_MUTEX_unlock(&xprt->xp_lock);
		LogDebug(COMPONENT_DISPATCH, "xprt %p already stalled (oops)",
			 xprt);
		return true;
	}

	LogDebug(COMPONENT_DISPATCH, "xprt %p has %u reqs, marking stalled",
		 xprt, nreqs);

	/* ok, need to stall */
	PTHREAD_MUTEX_lock(&nfs_req_st.stallq.mtx);

	glist_add_tail(&nfs_req_st.stallq.q, &xu->stallq);
	atomic_set_uint16_t_bits(&xu->flags, XPRT_PRIVATE_FLAG_STALLED);

	PTHREAD_MUTEX_unlock(&xprt->xp_lock);

	/* if no thread is servicing the stallq, start one */
	if (!nfs_req_st.stallq.active) {
		LogDebug(COMPONENT_DISPATCH, "starting stallq service thread");
		if (fridgethr_submit(req_fridge, thr_stallq, NULL) == 0)
			nfs_req_st.stallq.active = true;
		else
			LogCrit(COMPONENT_DISPATCH, "Failed to start stallq");
	}

	PTHREAD_MUTEX_unlock(&nfs_req_st.stallq.mtx);

	/* stalled */
	return true;
}

void nfs_rpc_queue_init(void)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;
	struct fridgethr_params reqparams = {0};
	struct req_q_pair *qpair;
	int i;

	/** @todo Add a configuration parameter to set a max. */

	reqparams.thr_max = 0;
	reqparams.thr_min = 1;
	reqparams.thread_delay = ncp->decoder_fridge_expiration_delay;
	reqparams.block_delay = ncp->decoder_fridge_block_timeout;
	reqparams.deferment = fridgethr_defer_block;

	/* decoder thread pool */
	if (fridgethr_init(&req_fridge, "decoder", &reqparams))
		LogFatal(COMPONENT_DISPATCH,
			 "Unable to initialize decoder thread pool");

	/* queues */
	pthread_spin_init(&nfs_req_st.reqs.sp, PTHREAD_PROCESS_PRIVATE);
	nfs_req_st.reqs.size = 0;
	for (i = 0; i < N_REQ_QUEUES; ++i) {
		qpair = &(nfs_req_st.reqs.nfs_request_q.qset[i]);
		qpair->s = req_q_s[i];
		nfs_rpc_q_init(&qpair->producer);
		nfs_rpc_q_init(&qpair->consumer);
	}

	/* waitq */
	glist_init(&nfs_req_st.reqs.wait_list);
	nfs_req_st.reqs.waiters = 0;

	/* stallq */
	gsh_mutex_init(&nfs_req_st.stallq.mtx, NULL);
	glist_init(&nfs_req_st.stallq.q);
	nfs_req_st.stallq.active = false;
}

static uint32_t enqueued_reqs;
static uint32_t dequeued_reqs;

uint32_t get_enqueue_count(void)
{
	return enqueued_reqs;
}

uint32_t get_dequeue_count(void)
{
	return dequeued_reqs;
}

void nfs_rpc_enqueue_req(request_data_t *reqdata)
{
	struct req_q_set *nfs_req_q = &nfs_req_st.reqs.nfs_request_q;
	struct req_q_pair *qpair = NULL;
	struct req_q *q = NULL;

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(&reqdata->r_u.req.svc.bl_trace,
			&reqdata->r_u.req.xprt->blkin.endp, "enqueue-enter");
#endif

	switch (reqdata->rtype) {
	case NFS_REQUEST:
		LogFullDebug(COMPONENT_DISPATCH,
			     "enter rq_xid=%u lookahead.flags=%u",
			     reqdata->r_u.req.svc.rq_xid,
			     reqdata->r_u.req.lookahead.flags);
		if (reqdata->r_u.req.lookahead.flags & NFS_LOOKAHEAD_MOUNT)
			qpair = &(nfs_req_q->qset[REQ_Q_MOUNT]);
		else if (NFS_LOOKAHEAD_HIGH_LATENCY(reqdata->r_u.req.lookahead))
			qpair = &(nfs_req_q->qset[REQ_Q_HIGH_LATENCY]);
		else
			qpair = &(nfs_req_q->qset[REQ_Q_LOW_LATENCY]);
		break;
	case NFS_CALL:
		qpair = &(nfs_req_q->qset[REQ_Q_CALL]);
		break;
#ifdef _USE_9P
	case _9P_REQUEST:
		/* XXX identify high-latency requests and allocate
		 * to the high-latency queue, as above */
		qpair = &(nfs_req_q->qset[REQ_Q_LOW_LATENCY]);
		break;
#endif
	default:
		return;
	}

	now(&reqdata->time_queued);  /* this one is real, timestamp it */
	q = &qpair->producer;  /* always append to producer queue */

	pthread_spin_lock(&q->sp);
	glist_add_tail(&q->q, &reqdata->req_q);
	++(q->size);
	pthread_spin_unlock(&q->sp);

	atomic_inc_uint32_t(&enqueued_reqs);

#if defined(HAVE_BLKIN)
	/* log the queue depth */
	BLKIN_KEYVAL_INTEGER(&reqdata->r_u.req.svc.bl_trace,
			     &reqdata->r_u.req.xprt->blkin.endp, "reqs-est",
			     nfs_rpc_outstanding_reqs_est());

	BLKIN_TIMESTAMP(&reqdata->r_u.req.svc.bl_trace,
			&reqdata->r_u.req.xprt->blkin.endp, "enqueue-exit");
#endif
	LogDebug(COMPONENT_DISPATCH,
		 "enqueued req, q %p (%s %p:%p) size is %d (enq %u deq %u)", q,
		 qpair->s, &qpair->producer, &qpair->consumer, q->size,
		 enqueued_reqs, dequeued_reqs);

	/* potentially wakeup some thread */

	pthread_spin_lock(&nfs_req_st.reqs.sp);
	if (nfs_req_st.reqs.waiters) {
		struct wait_q_entry *wqe =
			glist_first_entry(&nfs_req_st.reqs.wait_list,
					  wait_q_entry_t, waitq);

		LogFullDebug(COMPONENT_DISPATCH,
			     "nfs_req_st.reqs.waiters %u signal wqe %p (for q %p)",
			     nfs_req_st.reqs.waiters, wqe, q);

		/* release 1 waiter */
		glist_del(&wqe->waitq);
		--(nfs_req_st.reqs.waiters);
		--(wqe->waiters);

		pthread_spin_unlock(&nfs_req_st.reqs.sp);

		PTHREAD_MUTEX_lock(&wqe->lwe.mtx);
		/* XXX reliable handoff */
		wqe->flags |= Wqe_LFlag_SyncDone;
		if (wqe->flags & Wqe_LFlag_WaitSync)
			pthread_cond_signal(&wqe->lwe.cv);
		PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
	} else
		pthread_spin_unlock(&nfs_req_st.reqs.sp);
}

static request_data_t *nfs_rpc_consume_req(struct req_q_pair *qpair)
{
	request_data_t *reqdata = NULL;
	char *s = NULL;
	uint32_t csize = ~0U;
	uint32_t psize = ~0U;

	pthread_spin_lock(&qpair->consumer.sp);
	if (qpair->consumer.size > 0) {
		reqdata = glist_first_entry(&qpair->consumer.q, request_data_t,
					    req_q);
		glist_del(&reqdata->req_q);
		--(qpair->consumer.size);
		pthread_spin_unlock(&qpair->consumer.sp);
		return reqdata;
	}

	pthread_spin_lock(&qpair->producer.sp);
	if (isFullDebug(COMPONENT_DISPATCH)) {
		s = (char *)qpair->s;
		csize = qpair->consumer.size;
		psize = qpair->producer.size;
	}

	if (qpair->producer.size > 0) {
		/* splice */
		glist_splice_tail(&qpair->consumer.q, &qpair->producer.q);
		qpair->consumer.size = qpair->producer.size;
		qpair->producer.size = 0;
		/* consumer.size > 0 */
		pthread_spin_unlock(&qpair->producer.sp);
		reqdata = glist_first_entry(&qpair->consumer.q, request_data_t,
					    req_q);
		glist_del(&reqdata->req_q);
		--(qpair->consumer.size);
	} else
		pthread_spin_unlock(&qpair->producer.sp);

	pthread_spin_unlock(&qpair->consumer.sp);

	if (s)
		LogFullDebug(COMPONENT_DISPATCH,
			     "try splice, qpair %s consumer qsize=%u producer qsize=%u",
			     s, csize, psize);
	return reqdata;
}

request_data_t *nfs_rpc_dequeue_req(nfs_worker_data_t *worker)
{
	request_data_t *reqdata = NULL;
	struct req_q_set *nfs_request_q = &nfs_req_st.reqs.nfs_request_q;
	struct req_q_pair *qpair = NULL;
	uint32_t slot = (nfs_rpc_q_next_slot() % 4);
	wait_q_entry_t *wqe = &worker->wqe;
	struct fridgethr_context *ctx =
		container_of(worker, struct fridgethr_context, wd);
	uint32_t i;

	/* XXX: the following stands in for a more robust/flexible
	 * weighting function */

	for (i = 0; i < 4; ++i) {
		switch (slot) {
		case 0: /* MOUNT */
			qpair = &(nfs_request_q->qset[REQ_Q_MOUNT]);
			break;
		case 1: /* NFS_CALL */
			qpair = &(nfs_request_q->qset[REQ_Q_CALL]);
			break;
		case 2: /* LL */
			qpair = &(nfs_request_q->qset[REQ_Q_LOW_LATENCY]);
			break;
		case 3: /* HL */
			qpair = &(nfs_request_q->qset[REQ_Q_HIGH_LATENCY]);
			break;
		default: /* not here */
			abort();
			break;
		}

		LogFullDebug(COMPONENT_DISPATCH,
			     "dequeue_req try qpair %s %p:%p", qpair->s,
			     &qpair->producer, &qpair->consumer);

		/* anything? */
		reqdata = nfs_rpc_consume_req(qpair);
		if (reqdata) {
			atomic_inc_uint32_t(&dequeued_reqs);
#if defined(HAVE_BLKIN)
			/* thread id */
			BLKIN_KEYVAL_INTEGER(&reqdata->r_u.req.svc.bl_trace,
					     &reqdata->r_u.req.xprt->blkin.endp,
					     "worker-id", worker->worker_index);

			BLKIN_TIMESTAMP(&reqdata->r_u.req.svc.bl_trace,
					&reqdata->r_u.req.xprt->blkin.endp,
					"dequeue-req");
#endif
			return reqdata;
		}

		++slot;
		slot = slot % 4;
	}			/* for */

	/* wait */
	assert(wqe->waiters == 0); /* wqe is not on any wait queue */

	PTHREAD_MUTEX_lock(&wqe->lwe.mtx);
	wqe->flags = Wqe_LFlag_WaitSync;
	wqe->waiters = 1;

	pthread_spin_lock(&nfs_req_st.reqs.sp);
	glist_add_tail(&nfs_req_st.reqs.wait_list, &wqe->waitq);
	++(nfs_req_st.reqs.waiters);
	pthread_spin_unlock(&nfs_req_st.reqs.sp);

	while (!(wqe->flags & Wqe_LFlag_SyncDone)) {
		struct timespec timeout = {time(NULL) + 5, 0};

		pthread_cond_timedwait(&wqe->lwe.cv, &wqe->lwe.mtx, &timeout);

		if (fridgethr_you_should_break(ctx)) {
			/* We are returning;
			 * so take us out of the waitq */
			pthread_spin_lock(&nfs_req_st.reqs.sp);
			if (wqe->waitq.next || wqe->waitq.prev) {
				/* Element is still in waitq, remove it */
				glist_del(&wqe->waitq);
				--(nfs_req_st.reqs.waiters);
				--(wqe->waiters);
				wqe->flags &= ~(Wqe_LFlag_WaitSync |
						Wqe_LFlag_SyncDone);
			}
			pthread_spin_unlock(&nfs_req_st.reqs.sp);
			PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
			return NULL;
		}
	}

	/* XXX wqe was removed from nfs_req_st.waitq (by signalling thread) */
	wqe->flags &= ~(Wqe_LFlag_WaitSync | Wqe_LFlag_SyncDone);
	PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
	LogFullDebug(COMPONENT_DISPATCH, "wqe wakeup %p", wqe);
	return nfs_rpc_dequeue_req(worker);  /** try again */
}

/**
 * @brief Allocate a new request
 *
 * @param[in] xprt Transport to use
 *
 * @return New request data
 */
static inline request_data_t *alloc_nfs_request(SVCXPRT *xprt)
{
	request_data_t *reqdata = pool_alloc(request_pool, NULL);

	/* set the request as NFS already-read */
	reqdata->rtype = NFS_REQUEST;

	/* set up req */
	reqdata->r_u.req.svc.rq_xprt = xprt;
	reqdata->r_u.req.svc.rq_daddr_len = 0;
	reqdata->r_u.req.svc.rq_raddr_len = 0;

	/* set up xprt */
	reqdata->r_u.req.xprt = xprt;

	return reqdata;
}

static inline void free_nfs_request(request_data_t *reqdata)
{
	switch (reqdata->rtype) {
	case NFS_REQUEST:
		/* dispose RPC header */
		if (reqdata->r_u.req.svc.rq_msg)
			(void)free_rpc_msg(reqdata->r_u.req.svc.rq_msg);
		if (reqdata->r_u.req.svc.rq_auth)
			SVCAUTH_RELEASE(reqdata->r_u.req.svc.rq_auth,
					&(reqdata->r_u.req.svc));
		break;
	default:
		break;
	}
	pool_free(request_pool, reqdata);
}

/**
 * Extract RPC argument.
 */
static bool nfs_rpc_get_args(nfs_request_t *reqnfs)
{
	SVCXPRT *xprt = reqnfs->xprt;
	nfs_arg_t *arg_nfs = &reqnfs->arg_nfs;
	bool rlocked = true;
	bool slocked = false;

	memset(arg_nfs, 0, sizeof(nfs_arg_t));

	LogFullDebug(COMPONENT_DISPATCH,
		     "Before svc_getargs on socket %d, xprt=%p", xprt->xp_fd,
		     xprt);

	if (svc_getargs(xprt, &reqnfs->svc, reqnfs->funcdesc->xdr_decode_func,
			(caddr_t) arg_nfs, &reqnfs->lookahead))
		return true;

	LogInfo(COMPONENT_DISPATCH,
		"svc_getargs failed for Program %d, Version %d, Function %d xid=%u",
		(int)reqnfs->svc.rq_prog, (int)reqnfs->svc.rq_vers,
		(int)reqnfs->svc.rq_proc, reqnfs->svc.rq_xid);

	/* XXX move this, removing need for thr_ctx */
	DISP_SLOCK2(xprt);
	svcerr_decode(xprt, &reqnfs->svc);
	DISP_SUNLOCK(xprt);

	return false;
}

static inline enum auth_stat AuthenticateRequest(nfs_request_t *reqnfs,
						 bool *no_dispatch)
{
	struct rpc_msg *msg = reqnfs->svc.rq_msg;
	SVCXPRT *xprt = reqnfs->xprt;
	enum auth_stat why;
	bool rlocked = true;
	bool slocked = false;

	/* A few words of explanation are required here:
	 * In authentication is AUTH_NONE or AUTH_UNIX, then the value of
	 * no_dispatch remains false and the request is proceeded normally.
	 * If authentication is RPCSEC_GSS, no_dispatch may have value true,
	 * this means that gc->gc_proc != RPCSEC_GSS_DATA and that the message
	 * is in fact an internal negociation message from RPCSEC_GSS using
	 * GSSAPI. It then should not be proceed by the worker and SVC_STAT
	 * should be returned to the dispatcher.
	 */

	*no_dispatch = false;

	reqnfs->svc.rq_xprt = reqnfs->xprt;
	reqnfs->svc.rq_prog = msg->rm_call.cb_prog;
	reqnfs->svc.rq_vers = msg->rm_call.cb_vers;
	reqnfs->svc.rq_proc = msg->rm_call.cb_proc;
	reqnfs->svc.rq_xid = msg->rm_xid;

	LogFullDebug(COMPONENT_DISPATCH,
		     "About to authenticate Prog=%d, vers=%d, proc=%d xid=%u xprt=%p",
		     (int)reqnfs->svc.rq_prog, (int)reqnfs->svc.rq_vers,
		     (int)reqnfs->svc.rq_proc, reqnfs->svc.rq_xid,
		     reqnfs->svc.rq_xprt);

	why = svc_auth_authenticate(&reqnfs->svc, msg, no_dispatch);
	if (why != AUTH_OK) {
		LogInfo(COMPONENT_DISPATCH,
			"Could not authenticate request... rejecting with AUTH_STAT=%s",
			auth_stat2str(why));
		DISP_SLOCK2(xprt);
		svcerr_auth(xprt, &reqnfs->svc, why);
		DISP_SUNLOCK(xprt);
		*no_dispatch = true;
		return why;
	}

#ifdef _HAVE_GSSAPI
	struct rpc_gss_cred *gc =
		(struct rpc_gss_cred *)reqnfs->svc.rq_clntcred;

	if (reqnfs->svc.rq_verf.oa_flavor == RPCSEC_GSS)
		LogFullDebug(COMPONENT_DISPATCH,
			     "AuthenticateRequest no_dispatch=%d gc->gc_proc=(%u) %s",
			     *no_dispatch, gc->gc_proc,
			     str_gc_proc(gc->gc_proc));
#endif
	return AUTH_OK;
}

/**
 * @brief Helper function to validate rpc calls.
 *
 * Validate the rpc call as proper program,version, and within range proc
 * Reply at svc level on errors.  On return false will bypass straight to
 * returning error.
 * This function is only ever called from one point, and the
 * read-lock is always held at that call. If this changes,
 * we'll have to pass in the value of rlocked.
 *
 * @param[in] reqnfs Request to validate
 *
 * @return True if the request is valid, false otherwise.
 */
static bool is_rpc_call_valid(nfs_request_t *reqnfs)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;
	int lo_vers;
	int hi_vers;
	bool slocked = false;
	bool rlocked = true;

	if (reqnfs->svc.rq_prog == ncp->program[P_NFS]) {
		switch (reqnfs->svc.rq_vers) {
		case NFS_V3:
			if ((ncp->core_options & CORE_OPTION_NFSV3) &&
			    reqnfs->svc.rq_proc <= NFSPROC3_COMMIT)
				return true;
			goto noproc_err;
		case NFS_V4:
			if ((ncp->core_options & CORE_OPTION_NFSV4) &&
			    reqnfs->svc.rq_proc <= NFSPROC4_COMPOUND)
				return true;
			goto noproc_err;
		default:  /* version error, set the range and throw the error */
			lo_vers = NFS_V4;
			hi_vers = NFS_V3;
			if ((ncp->core_options & CORE_OPTION_NFSV3) != 0)
				lo_vers = NFS_V3;
			if ((ncp->core_options & CORE_OPTION_NFSV4) != 0)
				hi_vers = NFS_V4;
			goto progvers_err;
		}
	} else if (reqnfs->svc.rq_prog == ncp->program[P_NLM] &&
		   ((ncp->core_options & CORE_OPTION_NFSV3) != 0)) {
		if (reqnfs->svc.rq_vers == NLM4_VERS) {
			if (reqnfs->svc.rq_proc <= NLMPROC4_FREE_ALL)
				return true;
			goto noproc_err;
		}
		lo_vers = NLM4_VERS;
		hi_vers = NLM4_VERS;
		goto progvers_err;
	} else if (reqnfs->svc.rq_prog == ncp->program[P_MNT] &&
		   ((ncp->core_options & CORE_OPTION_NFSV3) != 0)) {
		/* Some clients may use the wrong mount version to umount, so
		 * always allow umount, otherwise only allow request if the
		 * appropriate mount version is enabled.  Also need to allow
		 * dump and export, so just disallow mount if version not
		 * supported.
		 */
		switch (reqnfs->svc.rq_vers) {
		case MOUNT_V3:
			if (reqnfs->svc.rq_proc <= MOUNTPROC3_EXPORT)
				return true;
			goto noproc_err;
		case MOUNT_V1:
			if (reqnfs->svc.rq_proc <= MOUNTPROC2_EXPORT &&
			    reqnfs->svc.rq_proc != MOUNTPROC2_MNT)
				return true;
			goto noproc_err;
		default:
			lo_vers = MOUNT_V1;
			hi_vers = MOUNT_V3;
			goto progvers_err;
		}
	} else if (reqnfs->svc.rq_prog == ncp->program[P_RQUOTA]) {
		switch (reqnfs->svc.rq_vers) {
		case RQUOTAVERS:
			if (reqnfs->svc.rq_proc <= RQUOTAPROC_SETACTIVEQUOTA)
				return true;
			goto noproc_err;
		case EXT_RQUOTAVERS:
			if (reqnfs->svc.rq_proc <= RQUOTAPROC_SETACTIVEQUOTA)
				return true;
			goto noproc_err;
		default:
			lo_vers = RQUOTAVERS;
			hi_vers = EXT_RQUOTAVERS;
			goto progvers_err;
		}
	} else {		/* No such program */
		if (reqnfs->xprt != NULL) {
			LogFullDebug(COMPONENT_DISPATCH,
				     "Invalid Program number #%d",
				     (int)reqnfs->svc.rq_prog);
			DISP_SLOCK2(reqnfs->xprt);
			svcerr_noprog(reqnfs->xprt, &reqnfs->svc);
			DISP_SUNLOCK2(reqnfs->xprt);
		}
		return false;
	}

 progvers_err:
	if (reqnfs->xprt != NULL) {
		LogFullDebug(COMPONENT_DISPATCH,
			     "Invalid protocol Version #%d for program number #%d",
			     (int)reqnfs->svc.rq_vers,
			     (int)reqnfs->svc.rq_prog);
		DISP_SLOCK(reqnfs->xprt);
		svcerr_progvers(reqnfs->xprt, &reqnfs->svc, lo_vers, hi_vers);
		DISP_SUNLOCK(reqnfs->xprt);
	}
	return false;

 noproc_err:
	if (reqnfs->xprt != NULL) {
		LogFullDebug(COMPONENT_DISPATCH,
			     "Invalid protocol program number #%d",
			     (int)reqnfs->svc.rq_prog);
		DISP_SLOCK(reqnfs->xprt);
		svcerr_noproc(reqnfs->xprt, &reqnfs->svc);
		DISP_SUNLOCK(reqnfs->xprt);
	}
	return false;
}				/* is_rpc_call_valid */

enum xprt_stat thr_decode_rpc_request(void *context, SVCXPRT *xprt)
{
	request_data_t *reqdata = alloc_nfs_request(xprt);  /* ! NULL */
	enum xprt_stat stat = XPRT_IDLE;
	bool no_dispatch = true;
	bool rlocked = false;
	bool enqueued = false;
	bool recv_status;

	LogDebug(COMPONENT_DISPATCH, "%p context %p", xprt, context);

	reqdata = alloc_nfs_request(xprt);	/* ! NULL */
#if HAVE_BLKIN
	blkin_init_new_trace(&reqdata->r_u.req.svc.bl_trace, "nfs-ganesha",
			     &xprt->blkin.endp);
#endif

	/* pass private context */
	reqdata->r_u.req.svc.rq_context = context;

	DISP_RLOCK(xprt);

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(&reqdata->r_u.req.svc.bl_trace, &xprt->blkin.endp,
			"pre-recv");
#endif

	recv_status = SVC_RECV(xprt, &reqdata->r_u.req.svc);

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(&reqdata->r_u.req.svc.bl_trace, &xprt->blkin.endp,
			"post-recv");

	BLKIN_KEYVAL_INTEGER(&reqdata->r_u.req.svc.bl_trace,
			     &reqdata->r_u.req.xprt->blkin.endp, "rq-xid",
			     reqdata->r_u.req.svc.rq_xid);
#endif

	LogFullDebug(COMPONENT_DISPATCH,
		     "SVC_RECV on socket %d returned %s, xid=%u", xprt->xp_fd,
		     (recv_status) ? "true" : "false",
		     (reqdata->r_u.req.svc.rq_msg) ?
			reqdata->r_u.req.svc.rq_msg->rm_xid : 0);

	if (unlikely(!recv_status)) {
		/* RPC over TCP specific: RPC/UDP's xprt know only one state:
		 * XPRT_IDLE, because UDP is mostly a stateless protocol.
		 * With RPC/TCP, they can be XPRT_DIED especially when the
		 * client closes the peer's socket.
		 * We have to cope with this aspect in the next lines.  Finally,
		 * xdrrec uses XPRT_MOREREQS to indicate that additional
		 * records are ready to be consumed immediately. */

		char addrbuf[SOCK_NAME_MAX + 1];

		if (isDebug(COMPONENT_DISPATCH)) {
			sockaddr_t addr;
			if (copy_xprt_addr(&addr, xprt) == 1)
				sprint_sockaddr(&addr, addrbuf,
						sizeof(addrbuf));
			else
				sprintf(addrbuf, "<unresolved>");
		}

		stat = SVC_STAT(xprt);
		DISP_RUNLOCK(xprt);

		switch (stat) {
		case XPRT_IDLE:  /* typically, a new connection */
			LogDebug(COMPONENT_DISPATCH,
				 "Client on socket=%d, addr=%s has status XPRT_IDLE",
				 xprt->xp_fd, addrbuf);
			break;
		case XPRT_DIED:
			LogDebug(COMPONENT_DISPATCH,
				 "Client on socket=%d, addr=%s disappeared (XPRT_DIED)",
				 xprt->xp_fd, addrbuf);
			break;
		case XPRT_MOREREQS:  /* unexpected case */
			LogDebug(COMPONENT_DISPATCH,
				 "Client on socket=%d, addr=%s has status XPRT_MOREREQS",
				 xprt->xp_fd, addrbuf);
			break;
		default:
			LogDebug(COMPONENT_DISPATCH,
				 "Client on socket=%d, addr=%s has unknown status (%d)",
				 xprt->xp_fd, addrbuf, stat);
		}
		goto done;
	}

	/* XXX so long as nfs_rpc_get_funcdesc calls is_rpc_call_valid
	 * and fails if that call fails, there is no reason to call that
	 * function again, below */
	if (!is_rpc_call_valid(&reqdata->r_u.req))
		goto finish;

	reqdata->r_u.req.funcdesc = nfs_rpc_get_funcdesc(&reqdata->r_u.req);
	if (AuthenticateRequest(&reqdata->r_u.req, &no_dispatch) != AUTH_OK ||
	    no_dispatch)
		goto finish;

	if (!nfs_rpc_get_args(&reqdata->r_u.req))
		goto finish;

	if (context) {
		/* already running worker thread, do not enqueue */
		DISP_RUNLOCK(xprt);
		nfs_rpc_execute(reqdata);
		return XPRT_IDLE;
	}

	gsh_xprt_ref(xprt, XPRT_PRIVATE_FLAG_INCREQ, __func__, __LINE__);

	/* XXX as above, the call has already passed is_rpc_call_valid,
	 * the former check here is removed. */
	nfs_rpc_enqueue_req(reqdata);
	enqueued = true;

 finish:
	stat = SVC_STAT(xprt);
	DISP_RUNLOCK(xprt);

 done:
	/* if recv failed, request is not enqueued */
	if (!enqueued)
		free_nfs_request(reqdata);

	return stat;
}

static inline bool thr_continue_decoding(SVCXPRT *xprt, enum xprt_stat stat)
{
	const struct nfs_core_param *ncp = &nfs_param.core_param;

	return (unlikely(xprt->xp_requests > ncp->dispatch_max_reqs_xprt)) ?
			false : (stat == XPRT_MOREREQS);
}

static void thr_decode_rpc_requests(struct fridgethr_context *thr_ctx)
{
	enum xprt_stat stat;
	SVCXPRT *xprt = (SVCXPRT *) thr_ctx->arg;

	LogFullDebug(COMPONENT_RPC, "enter xprt=%p", xprt);

	do {
		stat = thr_decode_rpc_request(NULL, xprt);
	} while (thr_continue_decoding(xprt, stat));

	LogDebug(COMPONENT_DISPATCH, "exiting, stat=%s", xprt_stat_s[stat]);

	/* order MUST be SVC_DESTROY, gsh_xprt_unref
	 * (current refcnt balancing) */
	if (stat != XPRT_DIED)
		(void)svc_rqst_rearm_events(xprt, SVC_RQST_FLAG_NONE);
	else
		SVC_DESTROY(xprt);

	/* update accounting, clear decoding flag */
	gsh_xprt_unref(xprt, XPRT_PRIVATE_FLAG_DECODING, __func__, __LINE__);
}

/**
 * Ok, in the new world, TI-RPC's job is merely to tell us there is
 * activity on a specific xprt handle.
 *
 * Note that we have a builtin mechanism to bind, unbind, and (in
 * response to connect events, through a new callout made from within
 * the rendezvous in vc xprts) rebind/rebalance xprt handles to
 * independent event channels, each with their own platform event
 * demultiplexer.  The current callout is one event (request, or, if
 * applicable, new vc connect) on the active xprt handle xprt.
 *
 * We are a blocking call from the svc_run thread specific to our
 * current event channel (whatever it is).  Our goal is to hand off
 * processing of xprt to a request dispatcher thread as quickly as
 * possible, to minimize latency of all xprts on this channel.
 *
 * Next, the preferred dispatch thread should be, I speculate, one
 * which has (most) recently handled a request for this xprt.
 *
 * UDP RPCs are quite simple: everything comes to the same socket, so
 * several SVCXPRT can be defined, one per tbuf to handle the stuff
 * TCP RPCs are more complex:
 *   - a unique SVCXPRT exists that deals with initial tcp rendez vous.
 *     It does the accept with the client, but recv no message from the
 *     client. But SVC_RECV on it creates a new SVCXPRT dedicated to the
 *     client. This specific SVXPRT is bound on TCPSocket
 *
 * while receiving, I must know if this is a UDP request, an initial TCP
 * request or a TCP socket from an already connected client.
 *
 * This is how to distinguish the cases:
 * UDP connections are bound to socket NFS_UDPSocket
 * TCP initial connections are bound to socket NFS_TCPSocket
 * all the other cases are requests from already connected TCP Clients
 */
static bool nfs_rpc_getreq_ng(SVCXPRT *xprt /*, int chan_id */)
{
	/* The following actions are now purely diagnostic, the only side effect
	 * is a message to the log. */
	int code = 0;
	int rpc_fd = xprt->xp_fd;
	uint32_t nreqs;

	LogFullDebug(COMPONENT_RPC, "enter xprt=%p", xprt);

	if (udp_socket[P_NFS] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH, "A NFS UDP request fd %d",
			     rpc_fd);
	else if (udp_socket[P_MNT] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH, "A MOUNT UDP request %d",
			     rpc_fd);
	else if (udp_socket[P_NLM] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH, "A NLM UDP request %d",
			     rpc_fd);
	else if (udp_socket[P_RQUOTA] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH, "A RQUOTA UDP request %d",
			     rpc_fd);
	else if (tcp_socket[P_NFS] == rpc_fd)
		/* In this case, the SVC_RECV only produces a new connected
		 * socket (it does just a call to accept) */
		LogFullDebug(COMPONENT_DISPATCH,
			     "An initial NFS TCP request from a new client %d",
			     rpc_fd);
	else if (tcp_socket[P_MNT] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH,
			     "An initial MOUNT TCP request from a new client %d",
			     rpc_fd);
	else if (tcp_socket[P_NLM] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH,
			     "An initial NLM request from a new client %d",
			     rpc_fd);
	else if (tcp_socket[P_RQUOTA] == rpc_fd)
		LogFullDebug(COMPONENT_DISPATCH,
			     "An initial RQUOTA request from a new client %d",
			     rpc_fd);
	else
		LogFullDebug(COMPONENT_DISPATCH,
			     "An NFS TCP request from an already connected client %d",
			     rpc_fd);

	/* XXX
	 * Decoder backpressure.  There are multiple considerations here.
	 * One is to avoid decoding if doing so would cause the server to exceed
	 * global resource constraints.  Another is to adjust flow parameters on
	 * underlying network resources, to avoid moving the problem back into
	 * the kernel.  The latter requires continuous, but low overhead, flow
	 * measurement with hysteretic control.  For now, just do global and
	 * per-xprt request quotas.
	 */

	/* check max outstanding quota */
	nreqs = nfs_rpc_outstanding_reqs_est();
	if (unlikely(nreqs > nfs_param.core_param.dispatch_max_reqs)) {
		/* request queue is flow controlled */
		LogDebug(COMPONENT_DISPATCH,
			 "global outstanding reqs quota exceeded (have %u, allowed %u)",
			 nreqs, nfs_param.core_param.dispatch_max_reqs);
		thread_delay_ms(5);	/* don't busy-wait */
		(void)svc_rqst_rearm_events(xprt, SVC_RQST_FLAG_NONE);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		return true;
	}

	LogFullDebug(COMPONENT_RPC, "before decoder guard %p", xprt);

	/* clock duplicate, queued+stalled wakeups, queued wakeups */
	if (!gsh_xprt_decoder_guard(xprt, XPRT_PRIVATE_FLAG_NONE)) {
		LogFullDebug(COMPONENT_RPC, "already decoding %p", xprt);
		thread_delay_ms(5);
		(void)svc_rqst_rearm_events(xprt, SVC_RQST_FLAG_NONE);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		return true;
	}

	LogFullDebug(COMPONENT_RPC, "before cond stall %p", xprt);

	/* Check per-xprt max outstanding quota */
	if (nfs_rpc_cond_stall_xprt(xprt)) {
		/* Xprt stalled--bail.  Stall queue owns xprt ref and state. */
		LogDebug(COMPONENT_DISPATCH, "stalled, bail");
		/* clear decoding flag */
		gsh_xprt_clear_flag(xprt, XPRT_PRIVATE_FLAG_DECODING);
		return true;
	}

	LogFullDebug(COMPONENT_DISPATCH, "before fridgethr_get");

	/* schedule a thread to decode */
	code = fridgethr_submit(req_fridge, thr_decode_rpc_requests, xprt);
	if (code == ETIMEDOUT) {
		LogFullDebug(COMPONENT_RPC,
			     "Decode dispatch timed out, rearming. xprt=%p",
			     xprt);

		(void)svc_rqst_rearm_events(xprt, SVC_RQST_FLAG_NONE);
		gsh_xprt_unref(xprt, XPRT_PRIVATE_FLAG_DECODING, __func__,
			       __LINE__);
	} else if (code != 0) {
		LogMajor(COMPONENT_DISPATCH, "Unable to get decode thread: %d",
			 code);
	}

	LogFullDebug(COMPONENT_DISPATCH, "after fridgethr_get");

	return true;
}

