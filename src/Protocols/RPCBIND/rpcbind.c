// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright CEA/DAM/DIF  2010
 *  Author: Philippe Deniel (philippe.deniel@cea.fr)
 *
 * --------------------------
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
 */

#include <ifaddrs.h>
#include <net/if.h>
#include "config.h"
#include "log.h"
#include "nfs_core.h"
#include "nfs_proto_functions.h"
#include "rpc/rpc_com.h"
#include "nlm4.h"
#include "sal_functions.h"

static enum protos find_rpc_program(rpcprog_t pm_prog)
{
	enum protos proto;

	for (proto = 0; proto < P_COUNT; proto++)
		if (NFS_program[proto] == pm_prog)
			return proto;
	LogFullDebug(COMPONENT_DISPATCH, "Could not find RPC program %" PRIu32,
		     pm_prog);
	return P_COUNT;
}

static uint32_t get_tcp_port(enum protos proto)
{
	if (tcp_xprt[proto] == NULL)
		return 0;

	return get_port(svc_getrpclocal(tcp_xprt[proto]));
}

static uint32_t get_udp_port(enum protos proto)
{
	if (udp_xprt[proto] == NULL)
		return 0;

	return get_port(svc_getrpclocal(udp_xprt[proto]));
}

char *nullstr = "";

struct netbuf empty_netbuf = { 0, 0, NULL };

/*******************************************************************************
 * STATISTICS
 ******************************************************************************/

static rpcb_stat_byvers inf;

void rpcbs_procinfo(rpcvers_t rtype, rpcproc_t proc)
{
	switch (rtype + 2) {
	case PMAPVERS: /* version 2 */
		if (proc > rpcb_highproc_2)
			return;
		break;
	case RPCBVERS: /* version 3 */
		if (proc > rpcb_highproc_3)
			return;
		break;
	case RPCBVERS4: /* version 4 */
		if (proc > rpcb_highproc_4)
			return;
		break;
	default:
		return;
	}

	inf[rtype].info[proc]++;
}

static inline void rpcbs_set(rpcvers_t rtype, bool_t success)
{
	if (rtype < RPCBVERS_STAT && success)
		inf[rtype].setinfo++;
}

static inline void rpcbs_unset(rpcvers_t rtype, bool_t success)
{
	if (rtype < RPCBVERS_STAT && success)
		inf[rtype].unsetinfo++;
}

void rpcbs_getaddr(rpcvers_t rtype, rpcprog_t prog, rpcvers_t vers, char *netid,
		   char *uaddr)
{
	rpcbs_addrlist *al;
	struct netconfig *nconf;

	if (rtype >= RPCBVERS_STAT)
		return;

	for (al = inf[rtype].addrinfo; al; al = al->next) {
		if (al->netid == NULL)
			return;

		if ((al->prog == prog) && (al->vers == vers) &&
		    (strcmp(al->netid, netid) == 0)) {
			if ((uaddr == NULL) || (uaddr[0] == 0))
				al->failure++;
			else
				al->success++;
			return;
		}
	}
	nconf = nfs_Get_netconfig(netid);

	if (nconf == NULL)
		return;

	al = (rpcbs_addrlist *)gsh_calloc(1, sizeof(rpcbs_addrlist));

	al->prog = prog;
	al->vers = vers;
	al->netid = nconf->nc_netid;

	if ((uaddr == NULL) || (uaddr[0] == 0))
		al->failure = 1;
	else
		al->success = 1;

	al->next = inf[rtype].addrinfo;

	inf[rtype].addrinfo = al;
}

void rpcbs_rmtcall(rpcvers_t rtype, rpcproc_t rpcbproc, rpcprog_t prog,
		   rpcvers_t vers, rpcproc_t proc, char *netid)
{
	rpcbs_rmtcalllist *rl;
	struct netconfig *nconf;

	if (rtype >= RPCBVERS_STAT)
		return;

	for (rl = inf[rtype].rmtinfo; rl; rl = rl->next) {
		if (rl->netid == NULL)
			return;

		if ((rl->prog == prog) && (rl->vers == vers) &&
		    (rl->proc == proc) && (strcmp(rl->netid, netid) == 0)) {
			rl->failure++;
			if (rpcbproc == RPCBPROC_INDIRECT)
				rl->indirect++;
			return;
		}
	}

	nconf = nfs_Get_netconfig(netid);

	if (nconf == NULL)
		return;

	rl = (rpcbs_rmtcalllist *)gsh_calloc(1, sizeof(rpcbs_rmtcalllist));

	rl->prog = prog;
	rl->vers = vers;
	rl->proc = proc;
	rl->netid = nconf->nc_netid;
	rl->failure = 1;

	if (rpcbproc == RPCBPROC_INDIRECT)
		rl->indirect = 1;

	rl->next = inf[rtype].rmtinfo;
	inf[rtype].rmtinfo = rl;
}

/*******************************************************************************
 * UTILITY FUNCTIONS FOR PMAP
 ******************************************************************************/

bool xdr_pmap_dump_res(XDR *xdrs, struct pmap *regs)
{
	enum protos proto;
	struct pmap pmap;
	int vers;
	bool_t more = true;

	/* DECODE will fail, nothing to free so FREE will succeed */
	if (xdrs->x_op != XDR_ENCODE)
		return xdrs->x_op == XDR_FREE;

	for (proto = 0; proto < P_COUNT; proto++) {
		pmap.pm_prog = NFS_program[proto];

		if (tcp_xprt[proto] != NULL) {
			/* Do TCP registrations first */
			pmap.pm_prot = IPPROTO_TCP;
			pmap.pm_port = get_tcp_port(proto);

			for (vers = 1; vers <= MAX_PROTO_VERS; vers++)
				if (tcp_registrations[proto][vers]) {
					pmap.pm_vers = vers;
					if (!xdr_bool(xdrs, &more))
						return false;
					if (!xdr_pmap(xdrs, &pmap))
						return false;
				}
		}

		if (udp_xprt[proto] != NULL) {
			/* Do UDP registreations second */
			pmap.pm_prot = IPPROTO_UDP;
			pmap.pm_port = get_udp_port(proto);

			for (vers = 1; vers <= MAX_PROTO_VERS; vers++)
				if (udp_registrations[proto][vers]) {
					pmap.pm_vers = vers;
					if (!xdr_bool(xdrs, &more))
						return false;
					if (!xdr_pmap(xdrs, &pmap))
						return false;
				}
		}
	}

	/* Now encode the bool for no more... */
	more = false;
	return xdr_bool(xdrs, &more);
}

/*******************************************************************************
 * @brief The RPCBIND proc null function, for all versions.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_null(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling PMAPPROC_NULL");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* 0 is success */
	return 0;
}

/**
 * @brief Frees the result structure allocated for rpcbind_proc_null
 *
 * Does Nothing in fact.
 *
 * @param res        [INOUT]   Pointer to the result structure.
 *
 */
void rpcbind_nothing_free(nfs_res_t *res)
{
	/* Nothing to do */
}

/*******************************************************************************
 * @brief The PMAP proc set function, for version 2.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int pmap_proc_set(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	sockaddr_t *caller = svc_getrpccaller(req->rq_xprt);

	LogInfo(COMPONENT_DISPATCH, "REQUEST PROCESSING: Calling PMAPPROC_SET");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	if (!is_loopback(caller)) {
		/* Not from loopback */
		LogInfo(COMPONENT_DISPATCH,
			"PMAPPROC_SET called from other than localhost");
		goto failure;
	}

	LogInfo(COMPONENT_DISPATCH, "PMAPPROC_SET not supported");

failure:

	res->res_pmap_set_unset = false;

	rpcbs_set(req->rq_msg.cb_vers - PMAPVERS, res->res_pmap_set_unset);

	return res->res_pmap_set_unset ? 0 : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The PMAP proc unset function, for version 2.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int pmap_proc_unset(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	sockaddr_t *caller = svc_getrpccaller(req->rq_xprt);

	LogInfo(COMPONENT_DISPATCH,
		"REQUEST PROCESSING: Calling PMAPPROC_UNSET");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	if (!is_loopback(caller)) {
		/* Not from loopback */
		LogInfo(COMPONENT_DISPATCH,
			"PMAPPROC_UNSET called from other than localhost");
		goto failure;
	}

	LogInfo(COMPONENT_DISPATCH, "PMAPPROC_UNSET not supported");

failure:

	res->res_pmap_set_unset = false;

	rpcbs_unset(req->rq_msg.cb_vers - PMAPVERS, res->res_pmap_set_unset);

	return res->res_pmap_set_unset ? 0 : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The PMAP proc getport function, for version 2.
 *
 * @param[in]  arg    arg_pmap used for parameters
 * @param[in]  req    Ignored
 * @param[out] res    res_pmap_getport used for result
 */

int pmap_proc_getport(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	enum protos proto = find_rpc_program(arg->arg_pmap.pm_prog);

	LogDebug(COMPONENT_DISPATCH,
		 "REQUEST PROCESSING: Calling PMAPPROC_GETPORT for %" PRIu32,
		 arg->arg_pmap.pm_prog);

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_pmap_getport = 0;

	if ((proto == P_COUNT) || (arg->arg_pmap.pm_vers > MAX_PROTO_VERS)) {
		LogFullDebug(COMPONENT_DISPATCH,
			     "Invalid program or version %" PRIu32,
			     arg->arg_pmap.pm_vers);
	} else {
		switch (arg->arg_pmap.pm_prot) {
		case IPPROTO_TCP:
			res->res_pmap_getport = get_tcp_port(proto);
			break;
		case IPPROTO_UDP:
			res->res_pmap_getport = get_udp_port(proto);
			break;
		}
	}

	rpcbs_getaddr(RPCBVERS_2_STAT, arg->arg_pmap.pm_prog,
		      arg->arg_pmap.pm_vers, "tcp",
		      res->res_pmap_getport != 0 ? "1" : "");

	return res->res_pmap_getport != 0 ? NFS_REQ_OK : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The PMAP proc dump function, for version 2.
 *
 * @param[in]  arg    Ignored (void)
 * @param[in]  req    Ignored
 * @param[out] res    Ignored (XDR function does all the work)
 */

int pmap_proc_dump(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling PMAPPROC_DUMP");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* Nothing to do - all the magic is in the xdr encode above */

	/* 0 is success */
	return 0;
}

/*******************************************************************************
 * @brief The PMAP proc callit function, for version 2.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int pmap_proc_callit(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogInfo(COMPONENT_DISPATCH,
		"REQUEST PROCESSING: Calling PMAPPROC_CALLIT");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* Do nothing - don't support this, collect stats on failure */
	rpcbs_rmtcall(req->rq_msg.cb_vers, req->rq_msg.cb_proc,
		      arg->arg_callit.prog, arg->arg_callit.vers,
		      arg->arg_callit.proc, req->rq_xprt->xp_netid);

	/* 0 is success */
	return NFS_REQ_ERROR;
}

/*******************************************************************************
 * UTILITY FUNCTIONS FOR RPCBIND
 ******************************************************************************/

#define SA2SIN(sa) ((struct sockaddr_in *)(sa))
#define SA2SINADDR(sa) (SA2SIN(sa)->sin_addr)
#define SA2SIN6(sa) ((struct sockaddr_in6 *)(sa))
#define SA2SIN6ADDR(sa) (SA2SIN6(sa)->sin6_addr)

/*
 * For all bits set in "mask", compare the corresponding bits in
 * "dst" and "src", and see if they match. Returns 0 if the addresses
 * match.
 */
static bool bitmaskcmp(void *dst, void *src, void *mask, int bytelen)
{
	int i;
	u_int8_t *p1 = dst, *p2 = src, *netmask = mask;

	for (i = 0; i < bytelen; i++)
		if ((p1[i] & netmask[i]) != (p2[i] & netmask[i]))
			return true;
	return false;
}

/* 16 bytes for IPv6 */
u_int8_t maskAllAddrBits[16] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*
 * Find a server address that can be used by `caller' to contact
 * the local service specified by `serv_uaddr'. If `clnt_uaddr' is
 * non-NULL, it is used instead of `caller' as a hint suggesting
 * the best address (e.g. the `r_addr' field of an rpc, which
 * contains the rpcbind server address that the caller used).
 *
 * Returns the best server address as a malloc'd "universal address"
 * string which should be freed by the caller. On error, returns NULL.
 */
char *mergeaddr(SVCXPRT *xprt, struct rpcb *request, SVCXPRT *srv_xprt, int af)
{
	char *serv_uaddr, *ret = NULL;
	sockaddr_t *caller = svc_getrpccaller(xprt);
	sockaddr_t *hint_sa = NULL, *serv_sa, ss;
	struct ifaddrs *ifap, *ifp = NULL, *bestif, *exactif;
	struct netbuf *serv_nbp, *hint_nbp = NULL, tbuf;
	int size = 0;

	if (isFullDebug(COMPONENT_DISPATCH)) {
		char ipstring[SOCK_NAME_MAX];
		struct display_buffer dspbuf = { sizeof(ipstring), ipstring,
						 ipstring };

		display_sockip(&dspbuf, caller);

		LogFullDebug(COMPONENT_DISPATCH, "Caller %s Family %d",
			     ipstring, caller->ss_family);
	}

	/* DUMP needs to pass in the specific SVCXPRT to use to get the port...
	 */
	if (srv_xprt != NULL)
		serv_nbp = svc_getlocal_netbuf(srv_xprt);
	else
		serv_nbp = svc_getlocal_netbuf(xprt);

	serv_uaddr = __rpc_taddr2uaddr_af(af, serv_nbp);

	LogFullDebug(COMPONENT_DISPATCH, "Server %s", serv_uaddr);
	LogFullDebug(COMPONENT_DISPATCH, "request->r_addr %s",
		     request ? request->r_addr : "No request");

	serv_nbp = NULL;

	/* If it's a local caller, just return the server address just
	 * allocated. Skip the rest of the logic.
	 */
	if (is_af_local(caller) || is_inaddrany(caller) || is_loopback(caller))
		return serv_uaddr;

	/* If saddr is not NULL, the remote client may have included the
	 * address by which it contacted us.  Use that for the "client" uaddr,
	 * otherwise use the info from the SVCXPRT.
	 */
	if (request != NULL && request->r_addr != NULL &&
	    *request->r_addr != '\0') {
		hint_nbp = __rpc_uaddr2taddr_af(af, request->r_addr);

		if (hint_nbp == NULL)
			goto freeit;

		hint_sa = hint_nbp->buf;

		if (hint_sa->ss_family != caller->ss_family) {
			/* ignore if address family is different from that of
			 * the caller. Keep hint_nbp to be freed later.
			 */
			LogFullDebug(
				COMPONENT_DISPATCH,
				"request->r_addr family %d different from caller",
				hint_sa->ss_family);
			hint_sa = NULL;
		} else {
			/* otherwise use request->r_addr as the hint. */
			LogFullDebug(COMPONENT_DISPATCH, "hint_sa %p", hint_sa);
		}
	}

	if (hint_sa == NULL) {
		/* request->r_addr was not provided or was wrong address family.
		 * Use xp_remote as the hint.
		 * Don't set hint_nbp - we don't need to free it.
		 */
		hint_sa = caller;
		LogFullDebug(COMPONENT_DISPATCH, "Using caller as hint_sa %p",
			     hint_sa);
	}

	if (getifaddrs(&ifp) < 0)
		goto freeit;

	/*
	 * Loop through all interfaces. For each interface, see if the
	 * network portion of its address is equal to that of the client.
	 * If so, we have found the interface that we want to use.
	 */
	bestif = NULL; /* first interface UP with same network & family */
	exactif = NULL; /* the interface requested by the client */

	for (ifap = ifp; ifap != NULL; ifap = ifap->ifa_next) {
		sockaddr_t *ifsa, *ifmasksa;

		ifsa = (sockaddr_t *)ifap->ifa_addr;
		ifmasksa = (sockaddr_t *)ifap->ifa_netmask;

		if (isFullDebug(COMPONENT_DISPATCH)) {
			char str_ifsa[SOCK_NAME_MAX];
			char str_ifmasksa[SOCK_NAME_MAX];
			struct display_buffer dspbuf_ifsa = { sizeof(str_ifsa),
							      str_ifsa,
							      str_ifsa };
			struct display_buffer dspbuf_ifmasksa = {
				sizeof(str_ifmasksa), str_ifmasksa, str_ifmasksa
			};

			if (ifsa)
				display_sockip(&dspbuf_ifsa, ifsa);
			else
				display_cat(&dspbuf_ifsa, "(null)");

			if (ifmasksa)
				display_sockip(&dspbuf_ifmasksa, ifmasksa);
			else
				display_cat(&dspbuf_ifmasksa, "(null)");

			LogFullDebug(
				COMPONENT_DISPATCH,
				"Interface %s addr %s family %d mask %s %s hint family %d",
				ifap->ifa_name, str_ifsa, ifsa->ss_family,
				str_ifmasksa,
				ifap->ifa_flags & IFF_UP ? "UP" : "DOWN",
				hint_sa->ss_family);
		}

		if (ifsa == NULL || ifsa->ss_family != hint_sa->ss_family ||
		    !(ifap->ifa_flags & IFF_UP))
			continue;

		switch (hint_sa->ss_family) {
		case AF_INET:
			/*
			 * If the hint address matches this interface
			 * address/netmask, then we're done.
			 */
			if (!bitmaskcmp(&SA2SINADDR(ifsa), &SA2SINADDR(hint_sa),
					&SA2SINADDR(ifmasksa),
					sizeof(struct in_addr))) {
				if (!bestif) {
					/* for compatibility with previous code
					*/
					bestif = ifap;
				}

				/* Is this an exact match? */
				if (!bitmaskcmp(&SA2SINADDR(ifsa),
						&SA2SINADDR(hint_sa),
						maskAllAddrBits,
						sizeof(struct in_addr))) {
					exactif = ifap;
					goto found;
				}
				/* else go-on looking for an exact match */
			}
			break;

		case AF_INET6:
			/*
			 * For v6 link local addresses, if the caller is on
			 * a link-local address then use the scope id to see
			 * which one.
			 */
			if (IN6_IS_ADDR_LINKLOCAL(&SA2SIN6ADDR(ifsa)) &&
			    IN6_IS_ADDR_LINKLOCAL(&SA2SIN6ADDR(caller)) &&
			    IN6_IS_ADDR_LINKLOCAL(&SA2SIN6ADDR(hint_sa))) {
				if (SA2SIN6(ifsa)->sin6_scope_id ==
				    SA2SIN6(caller)->sin6_scope_id) {
					bestif = ifap;
					goto found;
				}
			} else if (!bitmaskcmp(&SA2SIN6ADDR(ifsa),
					       &SA2SIN6ADDR(hint_sa),
					       &SA2SIN6ADDR(ifmasksa),
					       sizeof(struct in6_addr))) {
				if (!bestif) {
					/* for compatibility with previous code
					 */
					bestif = ifap;
				}

				/* Is this an exact match? */
				if (!bitmaskcmp(&SA2SIN6ADDR(ifsa),
						&SA2SIN6ADDR(hint_sa),
						maskAllAddrBits,
						sizeof(struct in6_addr))) {
					exactif = ifap;
					goto found;
				}
				/* else go-on looking for an exact match */
			}
			break;

		default:
			continue;
		}

		/*
		 * Remember the first possibly useful interface, preferring
		 * "normal" to point-to-point and loopback ones.
		 */
		if (bestif == NULL ||
		    (!(ifap->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) &&
		     (bestif->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))))
			bestif = ifap;
	}

	if (bestif == NULL)
		goto freeit;

found:

	if (exactif)
		bestif = exactif;

	/* Construct the new address using the address from `bestif', and the
	 * port number from `serv_uaddr'.
	 */
	serv_nbp = __rpc_uaddr2taddr_af(af, serv_uaddr);

	if (serv_nbp == NULL)
		goto freeit;

	serv_sa = serv_nbp->buf;

	switch (bestif->ifa_addr->sa_family) {
	case AF_INET:
		size = sizeof(struct sockaddr_in);
		break;

	case AF_INET6:
		size = sizeof(struct sockaddr_in6);
		break;
	}

	memcpy(&ss, bestif->ifa_addr, size);

	switch (ss.ss_family) {
	case AF_INET:
		SA2SIN(&ss)->sin_port = SA2SIN(serv_sa)->sin_port;
		tbuf.len = sizeof(struct sockaddr_in);
		break;

	case AF_INET6:
		SA2SIN6(&ss)->sin6_port = SA2SIN6(serv_sa)->sin6_port;
		tbuf.len = sizeof(struct sockaddr_in6);
		break;
	}

	tbuf.maxlen = sizeof(ss);
	tbuf.buf = &ss;
	ret = __rpc_taddr2uaddr_af(af, &tbuf);

freeit:

	if (hint_nbp != NULL) {
		gsh_free(hint_nbp->buf);
		gsh_free(hint_nbp);
	}

	if (serv_nbp != NULL) {
		gsh_free(serv_nbp->buf);
		gsh_free(serv_nbp);
	}

	if (ifp != NULL)
		freeifaddrs(ifp);

	gsh_free(serv_uaddr);

	return ret;
}

bool xdr_rpcbind_dump_res(XDR *xdrs, SVCXPRT **caller_xprt)
{
	enum protos proto;
	struct rpcb rpcb;
	int vers;
	bool_t more = true;

	/* DECODE will fail, nothing to free so FREE will succeed */
	if (xdrs->x_op != XDR_ENCODE)
		return xdrs->x_op == XDR_FREE;

	rpcb.r_owner = "superuser";

	for (proto = 0; proto < P_COUNT; proto++) {
		rpcb.r_prog = NFS_program[proto];

		/* Do AF_INET registrations first */
		rpcb.r_addr =
			mergeaddr(*caller_xprt, NULL, tcp_xprt[proto], AF_INET);

		for (vers = 1; vers <= MAX_PROTO_VERS; vers++) {
			if (tcp_registrations[proto][vers]) {
				rpcb.r_netid = "tcp";
				rpcb.r_vers = vers;
				if (!xdr_bool(xdrs, &more))
					goto free_addr;
				if (!xdr_rpcb(xdrs, &rpcb))
					goto free_addr;
			}
		}

		gsh_free(rpcb.r_addr);

		rpcb.r_addr =
			mergeaddr(*caller_xprt, NULL, udp_xprt[proto], AF_INET);

		for (vers = 1; vers <= MAX_PROTO_VERS; vers++) {
			if (udp_registrations[proto][vers]) {
				rpcb.r_netid = "udp";
				rpcb.r_vers = vers;
				if (!xdr_bool(xdrs, &more))
					goto free_addr;
				if (!xdr_rpcb(xdrs, &rpcb))
					goto free_addr;
			}
		}

		gsh_free(rpcb.r_addr);

		if (v6disabled)
			continue;

		/* Do AF_INET6 registrations second */
		rpcb.r_addr = mergeaddr(*caller_xprt, NULL, tcp_xprt[proto],
					AF_INET6);

		for (vers = 1; vers <= MAX_PROTO_VERS; vers++) {
			if (tcp_registrations[proto][vers]) {
				rpcb.r_netid = "tcp6";
				rpcb.r_vers = vers;
				if (!xdr_bool(xdrs, &more))
					goto free_addr;
				if (!xdr_rpcb(xdrs, &rpcb))
					goto free_addr;
			}
		}

		gsh_free(rpcb.r_addr);

		rpcb.r_addr = mergeaddr(*caller_xprt, NULL, udp_xprt[proto],
					AF_INET6);

		for (vers = 1; vers <= MAX_PROTO_VERS; vers++) {
			if (udp_registrations[proto][vers]) {
				rpcb.r_netid = "udp6";
				rpcb.r_vers = vers;
				if (!xdr_bool(xdrs, &more))
					goto free_addr;
				if (!xdr_rpcb(xdrs, &rpcb))
					goto free_addr;
			}
		}

		gsh_free(rpcb.r_addr);
	}

	/* Now encode the bool for no more... */
	more = false;
	return xdr_bool(xdrs, &more);

free_addr:

	gsh_free(rpcb.r_addr);
	return false;
}

static inline void fill_rpcb_entry_from_netconfig(rpcb_entry *ent,
						  struct netconfig *net)
{
	ent->r_nc_netid = net->nc_netid;
	ent->r_nc_semantics = net->nc_semantics;
	ent->r_nc_protofmly = net->nc_protofmly;
	ent->r_nc_proto = net->nc_proto;
}

bool xdr_rpcbind_getaddrlist_res(XDR *xdrs, nfs_request_t **req)
{
	struct rpcb *caller_rpcb = &(*req)->arg_nfs.arg_rpcb;
	struct rpcb_entry ent;
	enum protos proto = find_rpc_program(caller_rpcb->r_prog);
	SVCXPRT *caller_xprt = (*req)->svc.rq_xprt;
	int vers = caller_rpcb->r_vers;
	bool_t more = true;

	/* DECODE will fail, nothing to free so FREE will succeed */
	if (xdrs->x_op != XDR_ENCODE)
		return xdrs->x_op == XDR_FREE;

	if (proto == P_COUNT) {
		/* r_prog not found - exit with empty list */
		goto exit;
	}

	/* Do AF_INET registrations first */
	ent.r_nc_protofmly = NC_INET;

	ent.r_maddr = mergeaddr(caller_xprt, NULL, tcp_xprt[proto], AF_INET);

	if (tcp_registrations[proto][vers]) {
		fill_rpcb_entry_from_netconfig(&ent, netconfig_tcpv4);
		if (!xdr_bool(xdrs, &more))
			goto free_addr;
		if (!xdr_rpcb_entry(xdrs, &ent))
			goto free_addr;
	}

	gsh_free(ent.r_maddr);

	ent.r_maddr = mergeaddr(caller_xprt, NULL, udp_xprt[proto], AF_INET);

	if (udp_registrations[proto][vers]) {
		fill_rpcb_entry_from_netconfig(&ent, netconfig_udpv4);
		if (!xdr_bool(xdrs, &more))
			goto free_addr;
		if (!xdr_rpcb_entry(xdrs, &ent))
			goto free_addr;
	}

	gsh_free(ent.r_maddr);

	if (v6disabled)
		goto exit;

	/* Do AF_INET6 registrations second */
	ent.r_nc_protofmly = NC_INET6;

	ent.r_maddr = mergeaddr(caller_xprt, NULL, tcp_xprt[proto], AF_INET6);

	if (tcp_registrations[proto][vers]) {
		fill_rpcb_entry_from_netconfig(&ent, netconfig_tcpv6);
		if (!xdr_bool(xdrs, &more))
			goto free_addr;
		if (!xdr_rpcb_entry(xdrs, &ent))
			goto free_addr;
	}

	gsh_free(ent.r_maddr);

	ent.r_maddr = mergeaddr(caller_xprt, NULL, udp_xprt[proto], AF_INET6);

	if (udp_registrations[proto][vers]) {
		fill_rpcb_entry_from_netconfig(&ent, netconfig_udpv6);
		if (!xdr_bool(xdrs, &more))
			goto free_addr;
		if (!xdr_rpcb_entry(xdrs, &ent))
			goto free_addr;
	}

	gsh_free(ent.r_maddr);

exit:

	/* Now encode the bool for no more... */
	more = false;
	return xdr_bool(xdrs, &more);

free_addr:

	gsh_free(ent.r_maddr);
	return false;
}

/*
 * XDR remote call arguments.  It ignores the address part.
 * written for XDR_DECODE direction only
 */
bool xdr_rmtcall_args2(XDR *xdrs, struct rpcb_rmtcallargs *cap)
{
	/* does not get the address or the arguments */
	if (xdr_u_int32_t(xdrs, &cap->prog) &&
	    xdr_u_int32_t(xdrs, &cap->vers) &&
	    xdr_u_int32_t(xdrs, &cap->proc)) {
		return xdr_bytes(xdrs, (void *)&cap->args.args_val,
				 &cap->args.args_len, UINT32_MAX);
	}
	return (FALSE);
}

bool xdr_rpcb_statistics(XDR *xdrs, rpcb_stat *objp)
{
	/* DECODE will fail, nothing to free so FREE will succeed */
	if (xdrs->x_op != XDR_ENCODE)
		return xdrs->x_op == XDR_FREE;

	return xdr_rpcb_stat_byvers(xdrs, inf);
}

/*******************************************************************************
 * @brief The RPCBIND proc set function, for all versions.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_set(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	sockaddr_t *caller = svc_getrpccaller(req->rq_xprt);

	LogInfo(COMPONENT_DISPATCH, "REQUEST PROCESSING: Calling RPCBPROC_SET");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	if (!is_loopback(caller)) {
		/* Not from loopback */
		LogInfo(COMPONENT_DISPATCH,
			"RPCBPROC_SET called from other than localhost");
		goto failure;
	}

	LogInfo(COMPONENT_DISPATCH, "RPCBPROC_SET not supported");

failure:

	res->res_pmap_set_unset = false;

	rpcbs_set(req->rq_msg.cb_vers - PMAPVERS, res->res_pmap_set_unset);

	return res->res_pmap_set_unset ? 0 : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The RPCBIND proc unset function, for all versions.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_unset(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	sockaddr_t *caller = svc_getrpccaller(req->rq_xprt);

	LogInfo(COMPONENT_DISPATCH,
		"REQUEST PROCESSING: Calling RPCBPROC_UNSET");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	if (!is_loopback(caller)) {
		/* Not from loopback */
		LogInfo(COMPONENT_DISPATCH,
			"RPCBPROC_UNSET called from other than localhost");
		goto failure;
	}

	LogInfo(COMPONENT_DISPATCH, "RPCBPROC_UNSET not supported");

failure:

	res->res_pmap_set_unset = false;

	rpcbs_unset(req->rq_msg.cb_vers - PMAPVERS, res->res_pmap_set_unset);

	return res->res_pmap_set_unset ? 0 : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The RPCBIND proc getaddr function, for versions 3 and 4.
 *
 * @param[in]  arg    Uses arg_rpcb for parameters
 * @param[in]  req    Used tp fetch calling SVCXPRT
 * @param[out] res    res_uaddr filled in
 */

int rpcbind_proc_getaddr(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	enum protos proto = find_rpc_program(arg->arg_rpcb.r_prog);
	int af;

	LogDebug(COMPONENT_DISPATCH,
		 "REQUEST PROCESSING: Calling RPCBPROC_GETADDR for %" PRIu32,
		 arg->arg_rpcb.r_prog);

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_uaddr = nullstr;

	if ((proto == P_COUNT) || (arg->arg_rpcb.r_vers > MAX_PROTO_VERS)) {
		LogFullDebug(COMPONENT_DISPATCH,
			     "Invalid program or version %" PRIu32,
			     arg->arg_rpcb.r_vers);
		goto failure;
	}

	if (strncasecmp(arg->arg_rpcb.r_netid, "tcp", 3) == 0) {
		if (arg->arg_rpcb.r_netid[3] == '6')
			af = AF_INET6;
		else
			af = AF_INET;
	} else if (strncasecmp(arg->arg_rpcb.r_netid, "udp", 3) == 0) {
		if (arg->arg_rpcb.r_netid[3] == '6')
			af = AF_INET6;
		else
			af = AF_INET;
	} else if ((strcasecmp(arg->arg_rpcb.r_netid, "local") == 0) ||
		   (strcasecmp(arg->arg_rpcb.r_netid, "unix") == 0)) {
		af = AF_LOCAL;
	} else {
		/* Don't support any other netids */
		goto failure;
	}

	if ((af == AF_INET6) && v6disabled) {
		/* Request was for IPv6 but it's disabled. */
		goto failure;
	}

	res->res_uaddr = mergeaddr(req->rq_xprt, &arg->arg_rpcb, NULL, af);

	if (res->res_uaddr == NULL)
		res->res_uaddr = nullstr;

failure:

	rpcbs_getaddr(req->rq_msg.cb_vers - PMAPVERS, arg->arg_rpcb.r_prog,
		      arg->arg_rpcb.r_vers, arg->arg_rpcb.r_netid,
		      res->res_uaddr);

	return res->res_uaddr != nullstr ? NFS_REQ_OK : NFS_REQ_ERROR;
}

/**
 * @brief Frees the result structure allocated for rpcbind_proc_getaddr
 *
 * @param res        [INOUT]   Pointer to the result structure.
 *
 */
void rpcbind_res_uaddr_free(nfs_res_t *res)
{
	if (res->res_uaddr != nullstr)
		gsh_free(res->res_uaddr);
}

/*******************************************************************************
 * @brief The RPCBIND proc dump function, for versions 3 and 4.
 *
 * @param[in]  arg    Ignored (void)
 * @param[in]  req    Used to fetch calling SVCXPRT
 * @param[out] res    Used to pass calling SVCXPRT to XDR function
 */

int rpcbind_proc_dump(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_DUMP");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* (almost) Nothing to do, all the magic is in the XDR function but
	 * the XDR function needs our caller SVCXPRT.
	 */
	res->rpcbind_caller_xprt = req->rq_xprt;

	return NFS_REQ_OK;
}

/*******************************************************************************
 * @brief The RPCBIND proc callit function, for versions 3 and 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_callit(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogInfo(COMPONENT_DISPATCH,
		"REQUEST PROCESSING: Calling RPCBPROC_CALLIT");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* Do nothing - don't support this, collect stats on failure */
	rpcbs_rmtcall(req->rq_msg.cb_vers, req->rq_msg.cb_proc,
		      arg->arg_callit.prog, arg->arg_callit.vers,
		      arg->arg_callit.proc, req->rq_xprt->xp_netid);

	/* 0 is success */
	return NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The RPCBIND proc gettime function, for versions 3 and 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_gettime(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	time_t t1;

	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_GETTIME");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_time = time(&t1);

	/* 0 is success */
	return 0;
}

/*******************************************************************************
 * @brief The RPCBIND proc uaddr2taddr function, for versions 3 and 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_uaddr2taddr(nfs_arg_t *arg, struct svc_req *req,
			     nfs_res_t *res)
{
	struct netconfig *nconf = nfs_Get_netconfig(req->rq_xprt->xp_netid);

	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_UADDR2TADDR");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_taddr = uaddr2taddr(nconf, arg->arg_uaddr);

	if (res->res_taddr == NULL)
		res->res_taddr = &empty_netbuf;

	/* 0 is success */
	return 0;
}

/**
 * @brief Frees the result structure allocated for rpcbind_proc_uaddr2taddr
 *
 * @param res        [INOUT]   Pointer to the result structure.
 *
 */
void rpcbind_res_taddr_free(nfs_res_t *res)
{
	if (res->res_taddr != &empty_netbuf) {
		gsh_free(res->res_taddr->buf);
		gsh_free(res->res_taddr);
	}
}

/*******************************************************************************
 * @brief The RPCBIND proc taddr2uaddr function, for versions 3 and 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_taddr2uaddr(nfs_arg_t *arg, struct svc_req *req,
			     nfs_res_t *res)
{
	struct netconfig *nconf = nfs_Get_netconfig(req->rq_xprt->xp_netid);

	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_TADDR2UADDR");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_uaddr = taddr2uaddr(nconf, &arg->arg_taddr);

	if (res->res_uaddr == NULL)
		res->res_uaddr = nullstr;

	/* 0 is success */
	return 0;
}

/*******************************************************************************
 * @brief The RPCBIND proc getversaddr function, for version 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_getversaddr(nfs_arg_t *arg, struct svc_req *req,
			     nfs_res_t *res)
{
	enum protos proto = find_rpc_program(arg->arg_rpcb.r_prog);
	int af;

	LogDebug(COMPONENT_DISPATCH,
		 "REQUEST PROCESSING: Calling RPCBPROC_GETVERSADDR");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	res->res_uaddr = nullstr;

	if ((proto == P_COUNT) || (arg->arg_rpcb.r_vers > MAX_PROTO_VERS))
		goto failure;

	if (strncasecmp(arg->arg_rpcb.r_netid, "tcp", 3) == 0) {
		if (arg->arg_rpcb.r_netid[3] == '6')
			af = AF_INET6;
		else
			af = AF_INET;
		if (!tcp_registrations[proto][arg->arg_rpcb.r_vers])
			goto failure;
	} else if (strncasecmp(arg->arg_rpcb.r_netid, "udp", 3) == 0) {
		if (arg->arg_rpcb.r_netid[3] == '6')
			af = AF_INET6;
		else
			af = AF_INET;
		if (!udp_registrations[proto][arg->arg_rpcb.r_vers])
			goto failure;
	} else if ((strcasecmp(arg->arg_rpcb.r_netid, "local") == 0) ||
		   (strcasecmp(arg->arg_rpcb.r_netid, "unix") == 0)) {
		af = AF_LOCAL;
	} else {
		/* Don't support any other netids */
		goto failure;
	}

	if ((af == AF_INET6) && v6disabled) {
		/* Request was for IPv6 but it's disabled. */
		goto failure;
	}

	res->res_uaddr = mergeaddr(req->rq_xprt, &arg->arg_rpcb, NULL, af);

	if (res->res_uaddr == NULL)
		res->res_uaddr = nullstr;

failure:

	if (res->res_uaddr == nullstr)
		LogFullDebug(COMPONENT_DISPATCH,
			     "Invalid program or version %" PRIu32,
			     arg->arg_rpcb.r_vers);

	rpcbs_getaddr(req->rq_msg.cb_vers - PMAPVERS, arg->arg_rpcb.r_prog,
		      arg->arg_rpcb.r_vers, arg->arg_rpcb.r_netid,
		      res->res_uaddr);

	return res->res_uaddr != nullstr ? NFS_REQ_OK : NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The RPCBIND indirect null function, for version 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_indirect(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogInfo(COMPONENT_DISPATCH,
		"REQUEST PROCESSING: Calling RPCBPROC_INDIRECT");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* Do nothing - don't support this, collect stats on failure */
	rpcbs_rmtcall(req->rq_msg.cb_vers, req->rq_msg.cb_proc,
		      arg->arg_callit.prog, arg->arg_callit.vers,
		      arg->arg_callit.proc, req->rq_xprt->xp_netid);

	/* 0 is success */
	return NFS_REQ_ERROR;
}

/*******************************************************************************
 * @brief The RPCBIND proc getaddrlist function, for version 4.
 *
 * @param[in]  arg    Ignored in this function (used indirectly in XDR function)
 * @param[in]  req    nfs_request is derived from this
 * @param[out] res    Used to pass nfs_request to XDR function
 */

int rpcbind_proc_getaddrlist(nfs_arg_t *arg, struct svc_req *req,
			     nfs_res_t *res)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_GETADDRLIST");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* All the magic is in the XDR function, but we need stuff from the
	 * nfs_request.
	 */
	res->nfs_request = container_of(req, struct nfs_request, svc);
	return 0;
}

/*******************************************************************************
 * @brief The RPCBIND proc getstat function, for version 4.
 *
 * @param[in]  arg    Ignored
 * @param[in]  req    Ignored
 * @param[out] res    Ignored
 */

int rpcbind_proc_getstat(nfs_arg_t *arg, struct svc_req *req, nfs_res_t *res)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "REQUEST PROCESSING: Calling RPCBPROC_GETSTAT");

	rpcbs_procinfo(req->rq_msg.cb_vers - PMAPVERS, req->rq_msg.cb_proc);

	/* Nothing to do - all the magic is in the xdr encode above */

	/* 0 is success */
	return 0;
}
