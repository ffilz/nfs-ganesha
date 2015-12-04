/**
 * @file  nfs_rpc_dispatcher.h
 * @brief Contains the rpc_dispatcher_thread support code
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


#include <pthread.h>


#define N_TCP_EVENT_CHAN  3	/*< We don't really want to have too many,
				   relative to the number of available cores. */
#define UDP_EVENT_CHAN    0	/*< Put UDP on a dedicated channel */
#define TCP_RDVS_CHAN     1	/*< Accepts new tcp connections */
#define TCP_EVCHAN_0      2
#define N_EVENT_CHAN (N_TCP_EVENT_CHAN + 2)

#define test_for_additional_nfs_protocols(p) \
	((p != P_MNT && p != P_NLM && p != P_RQUOTA) ||		   \
	 (nfs_param.core_param.core_options & (CORE_OPTION_NFSV3 | \
					       CORE_OPTION_NFSV4)))

#define UDP_REGISTER(prot, vers, netconfig) \
	svc_reg(udp_xprt[prot], nfs_param.core_param.program[prot], \
		(u_long) vers,					    \
		nfs_rpc_dispatch_dummy, netconfig)

#define TCP_REGISTER(prot, vers, netconfig) \
	svc_reg(tcp_xprt[prot], nfs_param.core_param.program[prot], \
		(u_long) vers,					    \
		nfs_rpc_dispatch_dummy, netconfig)


/**
 * TI-RPC event channels.  Each channel is a thread servicing an event
 * demultiplexer.
 */
struct rpc_evchan {
	uint32_t chan_id;	/*< Channel ID */
	pthread_t thread_id;	/*< POSIX thread ID */
};

static const char * const req_q_s[] = {
	"REQ_Q_MOUNT",
	"REQ_Q_CALL",
	"REQ_Q_LOW_LATENCY",
	"REQ_Q_HIGH_LATENCY"
};

static const char * const xprt_stat_s[] = {
	"XPRT_DIED",
	"XPRT_MOREREQS",
	"XPRT_IDLE",
	"XPRT_DESTROYED"
};

static const char * const tags[] = {
	"NFS",
	"MNT",
	"NLM",
	"RQUOTA",
};


struct proto_data {
	struct sockaddr_in sinaddr_udp;
	struct sockaddr_in sinaddr_tcp;
	struct sockaddr_in6 sinaddr_udp6;
	struct sockaddr_in6 sinaddr_tcp6;
	struct netbuf netbuf_udp6;
	struct netbuf netbuf_tcp6;
	struct t_bind bindaddr_udp6;
	struct t_bind bindaddr_tcp6;
	struct __rpc_sockinfo si_udp6;
	struct __rpc_sockinfo si_tcp6;
};


