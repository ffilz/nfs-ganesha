#include <stdlib.h>
#include "nfs_core.h"
#include "transparent_recovery.h"
#include "tsm.h"

unsigned int tsm_initialized;
bool tsm_disabled_source;
struct glist_head tsm_hosts = GLIST_HEAD_INIT(tsm_hosts);
struct glist_head export_ids = GLIST_HEAD_INIT(export_ids);
static const struct timespec msg_tout = { 0, 150000000 };
sockaddr_t tsm_my_addr;

#define FNV_PRIME32 16777619U
#define FNV_OFFSET32 2166136261U

#define FNV_PRIME64 1099511628211ULL
#define FNV_OFFSET64 14695981039346656037ULL

/* Pending ACK: mutex and msg_id counter (struct tsm_pending_ack is in transparent_recovery.h) */
pthread_mutex_t tsm_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
uint64_t tsm_msg_id_counter;

/* ACK events: delivered by TSM RPC handlers, processed in TSM control thread */
struct tsm_ack_event {
	tsm_ceph_nodes_t *node;
	uint64_t msg_id;
	struct glist_head list;
};

static struct glist_head tsm_ack_events = GLIST_HEAD_INIT(tsm_ack_events);
static pthread_mutex_t tsm_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tsm_ack_cond = PTHREAD_COND_INITIALIZER;

pthread_t tsm_thread;
pthread_t tsm_thread1;
pthread_t tsm_thread2;

bool is_self_ip(const char *in_addr)
{
	struct ifaddrs *ifaddr;
	int family, s;

	if (getifaddrs(&ifaddr) == -1) {
		return false;
	}

	struct ifaddrs *ifa = ifaddr;

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr != NULL) {
			family = ifa->ifa_addr->sa_family;

			if (family == AF_INET || family == AF_INET6) {
				char ip_addr[NI_MAXHOST];

				s = getnameinfo(
					ifa->ifa_addr,
					((family == AF_INET)
						 ? sizeof(struct sockaddr_in)
						 : sizeof(struct sockaddr_in6)),
					ip_addr, sizeof(ip_addr), NULL, 0,
					NI_NUMERICHOST);

				if (s != 0) {
					freeifaddrs(ifaddr);
					return false;
				} else {
					if (!strcmp(in_addr, ip_addr)) {
						freeifaddrs(ifaddr);
						return true;
					}
				}
			}
		}
	}

	freeifaddrs(ifaddr);

	return false;
}

/* Find export node corresponding to given export id */
state_export_ll_t *tsm_find_export_node(uint16_t export_id)
{
	struct glist_head *glist;
	state_export_ll_t *exp_node;

	if (glist_empty(&export_ids))
		return NULL;

	glist_for_each(glist, &export_ids) {
		exp_node = glist_entry(glist, state_export_ll_t, export_list);

		if (exp_node->tsm_export_info.export_id == export_id)
			return exp_node;
	}

	return NULL;
}

/* Disable TSM for a specific export */
void tsm_disable_export(uint16_t curr_exp)
{
	struct glist_head *glist;
	state_export_ll_t *exp_node;

	if (glist_empty(&export_ids))
		return;

	glist_for_each(glist, &export_ids) {
		exp_node = glist_entry(glist, state_export_ll_t, export_list);

		if (exp_node->tsm_export_info.export_id == curr_exp) {
			LogDebug(COMPONENT_TSM,
				 "Disabling TSM for the export id: %u",
				 curr_exp);

			exp_node->tsm_export_info.export_tsm_enabled = false;
		}
	}
}

/* Broadcast export list. Update to all cluster nodes */
void tsm_broadcast_export_list(uint16_t curr_exp, tsm_export_event_t exp_ev,
			       bool tsm_enabled)
{
	tsm_rpc_info msg;
	memset(&msg, 0, sizeof(msg));

	/* Update local export list */
	tsm_store_export_state(curr_exp, exp_ev, tsm_enabled);

	/* Populate export notification message */
	msg.export_info.export_id = curr_exp;
	msg.export_info.export_event = exp_ev;
	msg.export_info.export_tsm_enabled = tsm_enabled;
	msg.msg_type = TSM_EXPORT_ID_NOTIFY;

	LogDebug(COMPONENT_TSM, "Broadcasting export list");
	/* Send export update to peer nodes */
	tsm_send_msg(&msg);
}

void tsm_handle_peer_node_selection(tsm_ceph_nodes_t *my_node,
				    tsm_ceph_nodes_t **node_array,
				    int cluster_size, int my_index)
{
	/* We will reach here only for the first time a tsm node is booting up */
	tsm_rpc_info tsm_msg1 = { 0 };

	int prim_count;
	int second_count;
	int base;
	int primary_retries = 0;
	int secondary_retries = 0;

	tsm_ceph_nodes_t *prim_node_addr = NULL;
	tsm_ceph_nodes_t *second_node_addr = NULL;

	/* Populate source address of current node */
	memcpy(&tsm_msg1.source_addr, &tsm_my_addr, sizeof(sockaddr_t));

	/* PRIMARY NODE SELECTION */
	base = my_index;

	while (1) {
		/* Traverse cluster ring in reverse direction */
		prim_count = (base - 1 + cluster_size) % cluster_size;

		/* Skip self node */
		if (prim_count == my_index) {
			base = prim_count;
			continue;
		}

		prim_node_addr = node_array[prim_count];

		tsm_msg1.msg_type = TSM_PEER_PING;

		/* Select first reachable node as primary */
		if (tsm_process_send_msg(&prim_node_addr->fd,
					 &prim_node_addr->clnt,
					 prim_node_addr->node_addr, tsm_msg1)) {
			memcpy(&my_node->primary_node_addr,
			       &prim_node_addr->node_addr, sizeof(sockaddr_t));

			memcpy(&tsm_msg1.primary_node_addr,
			       &prim_node_addr->node_addr, sizeof(sockaddr_t));

			break;
		}

		base = prim_count;
		primary_retries++;

		/* Disable TSM if no active primary found */
		if (primary_retries > TSM_MAX_RETRIES) {
			LogDebug(COMPONENT_TSM,
				 "Disabling tsm: No primary node is up");
			tsm_initialized = 0;
			my_node->recovery.has_primary = false;
			my_node->recovery.has_secondary = false;
			goto tsm_out;
		}
	}

	/*SECONDARY NODE SELECTION*/
	base = prim_count;

	while (1) {
		/* Continue reverse traversal for secondary selection */
		second_count = (base - 1 + cluster_size) % cluster_size;

		/* Skip self node */
		if (second_count == my_index) {
			base = second_count;
			continue;
		}

		second_node_addr = node_array[second_count];

		/* Select first reachable node as secondary */
		if (tsm_process_send_msg(&second_node_addr->fd,
					 &second_node_addr->clnt,
					 second_node_addr->node_addr,
					 tsm_msg1)) {
			memcpy(&my_node->secondary_node_addr,
			       &second_node_addr->node_addr,
			       sizeof(sockaddr_t));

			memcpy(&tsm_msg1.secondary_node_addr,
			       &second_node_addr->node_addr,
			       sizeof(sockaddr_t));

			break;
		}

		base = second_count;
		secondary_retries++;

		/* Disable TSM if no active secondary found */
		if (secondary_retries > TSM_MAX_RETRIES) {
			LogDebug(COMPONENT_TSM,
				 "Disabling tsm: No Secondary node is up");
			tsm_initialized = 0;
			my_node->recovery.has_primary = false;
			my_node->recovery.has_secondary = false;
			goto tsm_out;
		}
	}

	/* Log selected primary and secondary indexes */
	LogDebug(
		COMPONENT_TSM,
		"TSM: Total cluster size %d, my_index %d, Primary index %d, Secondary index %d",
		cluster_size, my_index, prim_count, second_count);

	/* Mark peer flag to true */
	my_node->recovery.has_primary = true;
	my_node->recovery.has_secondary = true;
	tsm_msg1.has_primary = true;
	tsm_msg1.has_secondary = true;
	tsm_msg1.ganesha_id = g_nodeid;
	/* Notify cluster nodes about selected peers */
	tsm_msg1.msg_type = TSM_PEER_IP_NOTIFY;

	tsm_send_msg(&tsm_msg1);

tsm_out:

	return;
}

/* Request state information from peer nodes */
void tsm_request_state_from_peer(void)
{
	LogDebug(COMPONENT_TSM,
		 "Got back the primary and secondary peer node info");

	tsm_rpc_info tsm_msg = { 0 };
	memcpy(&tsm_msg.source_addr, &tsm_my_addr, sizeof(sockaddr_t));
	tsm_msg.msg_type = TSM_GET_STATE;

	tsm_send_msg_with_ack(&tsm_msg);
}

/* Build ordered node array and identify current node index */
int tsm_build_node_array(tsm_ceph_nodes_t **node_array,
			 tsm_ceph_nodes_t **my_node, int *my_index)
{
	struct glist_head *glist1;
	tsm_ceph_nodes_t *node = NULL;
	int count = 0;

	glist_for_each(glist1, &tsm_hosts) {
		node = glist_entry(glist1, tsm_ceph_nodes_t, node_list);

		if (!node)
			continue;

		/* Mark current node and initialize recovery state */
		if (node->is_my_ip == 1) {
			*my_index = count;
			*my_node = node;

			node->recovery.peer_recovery_state =
				TSM_PEER_RECORD_RECOVERY_INIT;
		}

		node_array[count++] = node;
	}

	return count;
}

/* Wait for peer recovery response using polling */
bool tsm_wait_for_recovery_poll(tsm_ceph_nodes_t *node)
{
	tsm_recovery_ctx_t *ctx;
	const int total_wait_ms = 100;
	const int interval_ms = 10;
	int waited = 0;

	ctx = &node->recovery;

	while (waited < total_wait_ms) {
		pthread_mutex_lock(&ctx->lock);

		/* Peer recovery response received */
		if (ctx->peer_responded) {
			pthread_mutex_unlock(&ctx->lock);
			return true;
		}

		pthread_mutex_unlock(&ctx->lock);

		usleep(interval_ms * 1000);
		waited += interval_ms;
	}

	/* Timed out waiting for peer response */
	return false;
}

/* Recover peer state information from available nodes */
bool tsm_recover_from_peers(tsm_ceph_nodes_t *my_node,
			    tsm_ceph_nodes_t **node_array, int cluster_size,
			    int my_index)
{
	tsm_recovery_ctx_t *ctx;
	tsm_rpc_info tsm_msg = { 0 };
	tsm_ceph_nodes_t *node;
	bool first_boot = false;
	int attempt;
	int i;

	ctx = &my_node->recovery;

	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cond, NULL);

	ctx->peer_responded = false;
	ctx->has_primary = false;
	ctx->has_secondary = false;
	ctx->timeout_sec = 3;

	memcpy(&tsm_msg.source_addr, &tsm_my_addr, sizeof(sockaddr_t));
	tsm_msg.msg_type = TSM_GET_PEER_NODE_INFO;

	LogDebug(COMPONENT_TSM, "TSM: Starting peer recovery");

	for (attempt = 0; attempt < TSM_MAX_RECOVERY_RETRIES; attempt++) {
		LogDebug(COMPONENT_TSM, "TSM: Starting recovery attempt %d",
			 attempt + 1);

		for (i = 0; i < cluster_size; i++) {
			if (i == my_index)
				continue;

			node = node_array[i];

			if (!node)
				continue;

			LogDebug(
				COMPONENT_TSM,
				"TSM: Trying recovery from node %d (attempt %d)",
				i, attempt + 1);

			/* Reset recovery state before each node attempt */
			pthread_mutex_lock(&ctx->lock);

			ctx->peer_responded = false;
			ctx->has_primary = false;
			ctx->has_secondary = false;
			ctx->peer_recovery_state =
				TSM_PEER_RECORD_RECOVERY_REQUEST_SENT;

			pthread_mutex_unlock(&ctx->lock);

			/* Send peer recovery request */
			if (!tsm_process_send_msg(&node->fd, &node->clnt,
						  node->node_addr, tsm_msg)) {
				LogDebug(COMPONENT_TSM,
					 "TSM: Send failed to node %d", i);

				continue;
			}

			/* Wait for peer response */
			if (tsm_wait_for_recovery_poll(my_node)) {
				pthread_mutex_lock(&ctx->lock);

				/* Peer responded but no cluster state present */
				if (!ctx->has_primary && !ctx->has_secondary) {
					first_boot = true;

					pthread_mutex_unlock(&ctx->lock);

					LogDebug(
						COMPONENT_TSM,
						"TSM: Peer node %d responded but no primary/secondary found",
						i);

					continue;
				}

				/* Recovery completed successfully */
				if (ctx->has_primary && ctx->has_secondary) {
					ctx->peer_recovery_state =
						TSM_PEER_RECORD_RECOVERY_DONE;

					pthread_mutex_unlock(&ctx->lock);

					LogDebug(
						COMPONENT_TSM,
						"TSM: Full recovery successful from node %d",
						i);

					return true;
				}

				pthread_mutex_unlock(&ctx->lock);
			}

			LogDebug(COMPONENT_TSM,
				 "TSM: Timeout from node %d (attempt %d)", i,
				 attempt + 1);
		}

		LogDebug(
			COMPONENT_TSM,
			"TSM: Attempt %d completed from all nodes. Retrying again",
			attempt + 1);

		/* Small backoff before retrying */
		usleep(1000 * 1000);
	}

	/* All recovery attempts failed */
	pthread_mutex_lock(&ctx->lock);

	if (first_boot) {
		ctx->peer_recovery_state = TSM_PEER_RECORD_FIRST_BOOT_DONE;

		pthread_mutex_unlock(&ctx->lock);

		return true;
	}

	ctx->peer_recovery_state = TSM_PEER_RECORD_RECOVERY_FAILED;

	pthread_mutex_unlock(&ctx->lock);

	LogDebug(COMPONENT_TSM, "TSM: Peer recovery failed after %d attempts",
		 TSM_MAX_RECOVERY_RETRIES);

	return false;
}

/* Initialize export entry for TSM tracking */
bool tsm_exp_list_init(struct gsh_export *exp, void *arg)
{
	state_export_ll_t *entry;

	if (exp->export_id == 0)
		return false;

	/* Allocate and initialize export entry */
	entry = gsh_calloc(1, sizeof(state_export_ll_t));

	if (!entry)
		return false;

	glist_init(&entry->export_list);

	entry->tsm_export_info.export_id = exp->export_id;
	entry->tsm_export_info.export_state = EXPORT_NO_IO;
	entry->tsm_export_info.export_ref_count = 0;
	entry->tsm_export_info.export_tsm_enabled = true;

	glist_add_tail(&export_ids, &entry->export_list);

	LogDebug(COMPONENT_TSM,
		 "TSM: Initialized export_id=%u with NO_IO state",
		 entry->tsm_export_info.export_id);

	return true;
}

/* Free RPC callback request context */
static void tsm_rpc_call_free(struct clnt_req *cc, size_t unused)
{
	gsh_free(cc);
}

/* Process RPC callback completion */
static void tsm_rpc_call_process(struct clnt_req *cc)
{
	LogFullDebug(COMPONENT_TSM, "TSM: callback cc=%p status=%d", cc,
		     cc->cc_error.re_status);

	if (cc->cc_error.re_status == RPC_SUCCESS) {
		LogDebug(COMPONENT_TSM, "TSM: RPC msg sent OK");
	} else {
		LogCrit(COMPONENT_TSM, "TSM: Sending RPC msg failed status=%d",
			cc->cc_error.re_status);
	}

	clnt_req_release(cc);
}

/* Ensure RPC event channel is registered */
static bool tsm_ensure_evchan(CLIENT *clnt)
{
	SVCXPRT *xprt;
	int code;

	if (clnt == NULL)
		return false;

	xprt = clnt_vc_get_client_xprt(clnt);

	if (xprt == NULL)
		return false;

	code = svc_rqst_evchan_reg(0, xprt, SVC_RQST_FLAG_NONE);

	if (code != 0) {
		LogCrit(COMPONENT_TSM, "TSM: evchan registration failed (%d)",
			code);
		return false;
	}

	LogFullDebug(COMPONENT_TSM, "TSM: evchan ok clnt=%p xprt=%p", clnt,
		     xprt);

	return true;
}

/* Common helper to send asynchronous RPC messages */
static bool tsm_send_rpc_common(CLIENT *clnt, rpcproc_t proc, xdrproc_t xdr_arg,
				void *arg, const char *tag)
{
	struct clnt_req *cc;

	if (clnt == NULL)
		return false;

	cc = gsh_malloc(sizeof(*cc));

	clnt_req_fill(cc, clnt, authnone_ncreate(), proc, xdr_arg, arg,
		      (xdrproc_t)xdr_void, NULL);

	cc->cc_error.re_status = clnt_req_setup(cc, msg_tout);

	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_TSM, "TSM: %s setup failed", tag);
		clnt_req_release(cc);
		return false;
	}

	if (!tsm_ensure_evchan(clnt)) {
		LogCrit(COMPONENT_TSM, "TSM: %s evchan missing", tag);
		clnt_req_release(cc);
		return false;
	}

	cc->cc_refreshes = 0;
	cc->cc_process_cb = tsm_rpc_call_process;
	cc->cc_free_cb = tsm_rpc_call_free;

	LogFullDebug(COMPONENT_TSM, "TSM: %s CLNT_CALL_BACK start cc=%p", tag,
		     cc);

	cc->cc_error.re_status = CLNT_CALL_BACK(cc);

	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_TSM, "TSM: %s send failed", tag);
		clnt_req_release(cc);
		return false;
	}

	LogFullDebug(COMPONENT_TSM, "TSM: %s CLNT_CALL_BACK queued cc=%p", tag,
		     cc);

	return true;
}

/* Send generic TSM RPC message */
static bool tsm_send_rpc_msg(CLIENT *clnt, struct tsm_rpc_info msg)
{
	return tsm_send_rpc_common(clnt, 1, (xdrproc_t)xdr_tsm_rpc_info, &msg,
				   "tsm_msg");
}

/* Send TSM state RPC message */
static bool tsm_send_rpc_state(CLIENT *clnt, struct tsm_rpc_states *state)
{
	return tsm_send_rpc_common(clnt, 2, (xdrproc_t)xdr_tsm_rpc_states,
				   state, "tsm_state");
}

/* Create TCP socket and connect to peer TSM node */
static int tsm_create_socket(sockaddr_t *sockaddr)
{
	int r = -1;
	int fd = -1;
	struct sockaddr_in *in1;
	struct sockaddr_in6 *in2;

	in1 = (struct sockaddr_in *)sockaddr;
	in2 = (struct sockaddr_in6 *)sockaddr;

	switch (sockaddr->ss_family) {
	case AF_INET:
		/* Create IPv4 socket */
		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			LogCrit(COMPONENT_TSM,
				"TSM: socket(AF_INET) create failed");
			break;
		}

		in1->sin_port = htons(TSM_PORT);

		/* Try connect */
		r = connect(fd, (struct sockaddr *)in1,
			    sizeof(struct sockaddr));

		if (!r)
			goto done;

		LogDebug(COMPONENT_TSM,
			 "TSM: connect(AF_INET) failed, closing fd=%d", fd);

		close(fd);
		break;

	case AF_INET6:
		/* Create IPv6 socket */
		fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			LogCrit(COMPONENT_TSM,
				"TSM: socket(AF_INET6) create failed");
			break;
		}

		in2->sin6_port = htons(TSM_PORT);

		/* Try connect */
		r = connect(fd, (struct sockaddr *)in2,
			    sizeof(struct sockaddr));

		if (!r)
			goto done;

		LogDebug(COMPONENT_TSM,
			 "TSM: connect(AF_INET6) failed, closing fd=%d", fd);

		close(fd);
		break;

	default:
		break;
	}

	return -1;

done:
	LogDebug(COMPONENT_TSM, "TSM: socket connected fd=%d", fd);
	return fd;
}

/* Create RPC client over existing connection */
static CLIENT *tsm_create_rpc_client(int fd)
{
	CLIENT *clnt;
	SVCXPRT *xprt;
	int code;
	struct sockaddr_storage ss;
	struct netbuf raddr;

	clnt = NULL;
	xprt = NULL;
	code = 0;

	if (fd < TSM_VALID_FD) {
		LogCrit(COMPONENT_TSM, "TSM: Invalid fd to create client");
		return NULL;
	}

	raddr.buf = &ss;
	raddr.len = sizeof(ss);

	/* Create RPC client on top of TCP fd */
	clnt = clnt_vc_ncreatef(fd, &raddr, TSMPROG, TSM_VERS, 0, 0,
				CLNT_CREATE_FLAG_CLOSE);

	if (CLNT_FAILURE(clnt)) {
		LogCrit(COMPONENT_TSM, "TSM: RPC client create failed");
		return NULL;
	}

	/* Get underlying transport */
	xprt = clnt_vc_get_client_xprt(clnt);

	if (xprt == NULL) {
		LogCrit(COMPONENT_TSM, "TSM: RPC client xprt is NULL");

		CLNT_DESTROY(clnt);
		return NULL;
	}

	/* Register on a (global/legacy) event channel so CLNT_CALL_BACK works */
	code = svc_rqst_evchan_reg(0, xprt, SVC_RQST_FLAG_NONE);

	if (code != 0) {
		LogCrit(COMPONENT_TSM, "TSM: evchan registration failed (%d)",
			code);

		CLNT_DESTROY(clnt);
		return NULL;
	}

	LogFullDebug(COMPONENT_TSM, "TSM: evchan ok clnt=%p xprt=%p", clnt,
		     xprt);

	return clnt;
}

const char *tsm_msg_type_to_str(int type)
{
	switch (type) {
	case TSM_PEER_IP_NOTIFY:
		return "TSM_PEER_IP_NOTIFY (1)";

	case TSM_GET_STATE:
		return "TSM_GET_STATE (2)";

	case TSM_RECLAIM_EXPORT_IDS:
		return "TSM_RECLAIM_EXPORT_IDS (3)";

	case TSM_SET_STATE:
		return "TSM_SET_STATE (4)";

	case TSM_REPLY_STATE:
		return "TSM_REPLY_STATE (5)";

	case TSM_REPLY_EXPORT_LIST:
		return "TSM_REPLY_EXPORT_LIST (6)";

	case TSM_DELETE_STATE:
		return "TSM_DELETE_STATE (7)";

	case TSM_SET_PEER_NODE_INFO:
		return "TSM_SET_PEER_NODE_INFO (8)";

	case TSM_GET_PEER_NODE_INFO:
		return "TSM_GET_PEER_NODE_INFO (9)";

	case TSM_PEER_PING:
		return "TSM_PEER_PING (10)";

	case TSM_DISABLE_NOTIFY:
		return "TSM_DISABLE_NOTIFY (11)";

	case TSM_ENABLE_NOTIFY:
		return "TSM_ENABLE_NOTIFY (12)";

	case TSM_ACK_STATE:
		return "TSM_ACK_STATE (13)";

	case TSM_EXPORT_ID_NOTIFY:
		return "TSM_EXPORT_ID_NOTIFY (14)";

	default:
		return "UNKNOWN";
	}
}

bool tsm_process_send_msg(int *fd, CLIENT **clnt, sockaddr_t sockaddr,
			  tsm_rpc_info tsm_msg)
{
	int retries = 0;

retry:
	if ((*fd >= TSM_VALID_FD) && (*clnt != NULL)) {
		if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
			LogCrit(COMPONENT_TSM,
				"TSM send failed resetting client fd clnt retry (fd=%d, clnt=%p, retry=%d)",
				*fd, *clnt, retries);

			CLNT_DESTROY(*clnt);
			close(*fd);
			*fd = -1;
			*clnt = NULL;

			retries++;

			if (retries > TSM_MAX_RETRIES)
				return false;

			goto retry;
		}

	} else if (*fd >= TSM_VALID_FD) {
		*clnt = tsm_create_rpc_client(*fd);

		if (*clnt != NULL) {
			if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
				LogCrit(COMPONENT_TSM,
					"TSM send failed after client create fd clnt retry (fd=%d, clnt=%p, retry=%d)",
					*fd, *clnt, retries);

				CLNT_DESTROY(*clnt);
				close(*fd);
				*fd = -1;
				*clnt = NULL;

				retries++;

				if (retries > TSM_MAX_RETRIES)
					return false;

				goto retry;
			}

		} else {
			LogCrit(COMPONENT_TSM,
				"TSM client create failed fd retry (fd=%d, retry=%d)",
				*fd, retries);
		}

	} else if (*fd < TSM_VALID_FD) {
		*fd = tsm_create_socket(&sockaddr);

		if (*fd >= TSM_VALID_FD) {
			*clnt = tsm_create_rpc_client(*fd);

			if (*clnt != NULL) {
				if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
					LogCrit(COMPONENT_TSM,
						"TSM send failed after connect fd clnt retry (fd=%d, clnt=%p, retry=%d)",
						*fd, *clnt, retries);

					CLNT_DESTROY(*clnt);
					close(*fd);
					*fd = -1;
					*clnt = NULL;

					retries++;

					if (retries > TSM_MAX_RETRIES)
						return false;

					goto retry;
				}

			} else {
				LogCrit(COMPONENT_TSM,
					"TSM client create failed after connect fd retry (fd=%d, retry=%d)",
					*fd, retries);

				close(*fd);
				*fd = -1;
			}

		} else {
			LogCrit(COMPONENT_TSM,
				"TSM connect failed fd invalid retry (retry=%d)",
				retries);
		}
	}

	return true;
}

static void tsm_process_send_state(int *fd, CLIENT **clnt, sockaddr_t sockaddr,
				   tsm_rpc_states *tsm_state)
{
	int retries = 0;

retry:
	if ((*fd >= TSM_VALID_FD) && (*clnt != NULL)) {
		if (!tsm_send_rpc_state(*clnt, tsm_state)) {
			LogCrit(COMPONENT_TSM,
				"TSM state send failed resetting client fd clnt retry (fd=%d, clnt=%p, retry=%d)",
				*fd, *clnt, retries);

			CLNT_DESTROY(*clnt);
			close(*fd);
			*fd = -1;
			*clnt = NULL;

			retries++;

			if (retries > TSM_MAX_RETRIES)
				return;

			goto retry;
		}

	} else if (*fd >= TSM_VALID_FD) {
		*clnt = tsm_create_rpc_client(*fd);

		if (*clnt != NULL) {
			if (!tsm_send_rpc_state(*clnt, tsm_state)) {
				LogCrit(COMPONENT_TSM,
					"TSM state send failed after client create fd clnt retry (fd=%d, clnt=%p, retry=%d)",
					*fd, *clnt, retries);

				CLNT_DESTROY(*clnt);
				close(*fd);
				*fd = -1;
				*clnt = NULL;

				retries++;

				if (retries > TSM_MAX_RETRIES)
					return;

				goto retry;
			}
		}

	} else if (*fd < TSM_VALID_FD) {
		*fd = tsm_create_socket(&sockaddr);

		if (*fd >= TSM_VALID_FD) {
			*clnt = tsm_create_rpc_client(*fd);

			if (*clnt != NULL) {
				if (!tsm_send_rpc_state(*clnt, tsm_state)) {
					LogCrit(COMPONENT_TSM,
						"TSM state send failed after connect fd clnt retry (fd=%d, clnt=%p, retry=%d)",
						*fd, *clnt, retries);

					CLNT_DESTROY(*clnt);
					close(*fd);
					*fd = -1;
					*clnt = NULL;

					retries++;

					if (retries > TSM_MAX_RETRIES)
						return;

					goto retry;
				}

			} else {
				LogCrit(COMPONENT_TSM,
					"TSM client create failed after connect fd retry (fd=%d, retry=%d)",
					*fd, retries);

				close(*fd);
				*fd = -1;
			}

		} else {
			LogCrit(COMPONENT_TSM,
				"TSM connect failed fd invalid retry (retry=%d)",
				retries);
		}
	}
}

static uint64_t tsm_next_msg_id(void)
{
	uint64_t ret = 0;
	ret = __sync_add_and_fetch(&tsm_msg_id_counter, 1);
	return ret;
}

tsm_ceph_nodes_t *tsm_find_node_by_addr(sockaddr_t *addr)
{
	struct glist_head *glist;
	tsm_ceph_nodes_t *node;

	if (glist_empty(&tsm_hosts))
		return NULL;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (node && !sockaddr_cmp(&node->node_addr, addr, true))
			return node;
	}

	return NULL;
}

static void tsm_add_pending_ack(tsm_ceph_nodes_t *node, const tsm_rpc_info *msg)
{
	struct tsm_pending_ack *p;

	p = gsh_calloc(1, sizeof(*p));
	if (!p)
		return;

	p->msg_id = msg->msg_id;
	memcpy(&p->msg, msg, sizeof(p->msg));

	if (clock_gettime(CLOCK_MONOTONIC, &p->last_send_ts) != 0)
		p->last_send_ts = (struct timespec){ 0, 0 };

	p->retries = 0;
	glist_init(&p->list);

	LogFullDebug(COMPONENT_TSM,
		     "Added pending ACK msg_id=%lu fsid_maj=%" PRIu64
		     " fsid_min=%" PRIu64 " fileid=%" PRIu64,
		     (unsigned long)p->msg_id, (uint64_t)p->msg.fsid_maj,
		     (uint64_t)p->msg.fsid_min, (uint64_t)p->msg.fileid);

	pthread_mutex_lock(&tsm_pending_mutex);
	glist_add_tail(&node->pending_acks, &p->list);
	pthread_mutex_unlock(&tsm_pending_mutex);
}

/* Check whether ACK is still pending from primary or secondary node */
bool tsm_is_ack_pending_on_primay_secondary(uint64_t msg_id)
{
	struct glist_head *g, *g2;
	tsm_ceph_nodes_t *node;
	struct tsm_pending_ack *p;
	tsm_ceph_nodes_t *my_node;

	my_node = tsm_find_node_by_addr(&tsm_my_addr);
	if (!my_node)
		return false;

	glist_for_each(g, &tsm_hosts) {
		node = glist_entry(g, tsm_ceph_nodes_t, node_list);

		if ((!sockaddr_cmp(&node->node_addr,
				   &my_node->primary_node_addr, true)) ||
		    (!sockaddr_cmp(&node->node_addr,
				   &my_node->secondary_node_addr, true))) {
			glist_for_each(g2, &node->pending_acks) {
				p = glist_entry(g2, struct tsm_pending_ack,
						list);

				if (p->msg_id == msg_id) {
					LogDebug(
						COMPONENT_TSM,
						"Still waiting for ACK primary or secondary");
					return true;
				}
			}
		}
	}

	LogDebug(COMPONENT_TSM, "ACK received from primary and secondary");
	return false;
}

static void tsm_complete_pending_ack(tsm_ceph_nodes_t *node, uint64_t msg_id)
{
	struct glist_head *glist, *tmp;
	struct tsm_pending_ack *p;

	uint16_t export_id = 0;
	tsm_export_event_t ev = 0;
	bool clear = false;
	bool trigger_broadcast = false;
	bool is_reclaim = false;

	pthread_mutex_lock(&tsm_pending_mutex);

	glist_for_each_safe(glist, tmp, &node->pending_acks) {
		p = glist_entry(glist, struct tsm_pending_ack, list);

		if (p->msg_id == msg_id) {
			export_id = p->msg.export_info.export_id;
			ev = p->msg.export_info.export_event;
			is_reclaim = p->msg.reclaim;
			clear = true;

			glist_del(glist);
			gsh_free(p);

			node->tsm_suspect = false;

			LogFullDebug(COMPONENT_TSM,
				     "Pending ACK cleared msg_id=%lu",
				     (unsigned long)msg_id);
			break;
		}
	}

	if (clear) {
		/* Check if ack is pending. If all acks are processed, trigger export list broadcast */
		if (!tsm_is_ack_pending_on_primay_secondary(msg_id) &&
		    !is_reclaim) {
			trigger_broadcast = true;

		} else if (is_reclaim) {
			LogDebug(
				COMPONENT_TSM,
				"Reclaiming previous state. Not broadcasting export list");
		}
	}

	pthread_mutex_unlock(&tsm_pending_mutex);

	if (trigger_broadcast) {
		LogFullDebug(COMPONENT_TSM,
			     "All ACKs done. Broadcasting export_id=%u",
			     export_id);

		tsm_broadcast_export_list(export_id, ev, true);
	}
}

static void tsm_drain_ack_events(void)
{
	struct tsm_ack_event *ev;
	struct glist_head *g;

	pthread_mutex_lock(&tsm_ack_mutex);

	while (!glist_empty(&tsm_ack_events)) {
		/* Pop one event from the queue */
		g = tsm_ack_events.next;
		ev = glist_entry(g, struct tsm_ack_event, list);

		glist_del(g);

		/* Drop the lock while we complete the ACK to avoid holding it too long */
		pthread_mutex_unlock(&tsm_ack_mutex);

		if (ev->node != NULL)
			tsm_complete_pending_ack(ev->node, ev->msg_id);

		gsh_free(ev);

		/* Reacquire lock for the next iteration */
		pthread_mutex_lock(&tsm_ack_mutex);
	}

	pthread_mutex_unlock(&tsm_ack_mutex);
}

static void tsm_send_ack_to_peer(tsm_ceph_nodes_t *node,
				 const tsm_rpc_info *orig)
{
	const char *skip_env;
	tsm_rpc_info ack;

	/* Test hook: when TSM_SKIP_ACK_FOR_TEST=1 is set in the environment
	 * (on the ACK-sending node), we process the TSM message locally but
	 * intentionally do NOT send the ACK to the peer.
	 *
	 * - RPC SET_STATE/DELETE_STATE from Node A to this node still succeeds,
	 *   because the RPC handler runs and returns normally.
	 * - Node A adds a pending ACK entry and then never sees the ACK.
	 * - After TSM_ACK_TIMEOUT_MS * (1 initial + retries), Node A will retry
	 *   and eventually log "Peer ACK timeout ... marking peer suspect".
	 */
	skip_env = getenv("TSM_SKIP_ACK_FOR_TEST");
	if (skip_env != NULL && atoi(skip_env) == 1) {
		LogFullDebug(COMPONENT_TSM,
			     "Skipping ACK send msg_id=%lu test mode enabled",
			     (unsigned long)orig->msg_id);
		return;
	}

	memset(&ack, 0, sizeof(ack));
	ack.msg_type = TSM_ACK_STATE;
	ack.rec_type = orig->rec_type;
	ack.msg_id = orig->msg_id;
	ack.fsid_maj = orig->fsid_maj;
	ack.fsid_min = orig->fsid_min;
	ack.fileid = orig->fileid;
	memcpy(&ack.source_addr, &tsm_my_addr, sizeof(sockaddr_t));

	LogFullDebug(COMPONENT_TSM,
		     "Sending ACK msg_id=%lu msg_type=%d rec_type=%d to peer",
		     (unsigned long)orig->msg_id, orig->msg_type,
		     orig->rec_type);

	tsm_process_send_msg(&node->fd, &node->clnt, node->node_addr, ack);
}

static void tsm_retry_or_fail_pending_acks_for_node(tsm_ceph_nodes_t *node)
{
	struct glist_head *glist, *tmp;
	struct tsm_pending_ack *p;
	struct timespec now;
	long elapsed_ms;

	if (glist_empty(&node->pending_acks))
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;

	pthread_mutex_lock(&tsm_pending_mutex);
	glist_for_each_safe(glist, tmp, &node->pending_acks) {
		p = glist_entry(glist, struct tsm_pending_ack, list);
		elapsed_ms = (now.tv_sec - p->last_send_ts.tv_sec) * 1000 +
			     (now.tv_nsec - p->last_send_ts.tv_nsec) / 1000000;

		LogFullDebug(
			COMPONENT_TSM,
			"Check pending ACK msg_id=%lu retries=%d elapsed=%ldms "
			"fsid_maj=%" PRIu64 " fsid_min=%" PRIu64
			" fileid=%" PRIu64,
			(unsigned long)p->msg_id, p->retries, elapsed_ms,
			(uint64_t)p->msg.fsid_maj, (uint64_t)p->msg.fsid_min,
			(uint64_t)p->msg.fileid);

		if (elapsed_ms < TSM_ACK_TIMEOUT_MS)
			continue;

		if (p->retries < TSM_ACK_MAX_RETRIES) {
			p->last_send_ts = now;
			p->retries++;

			LogFullDebug(COMPONENT_TSM,
				     "Retry ACK msg_id=%lu attempt=%d",
				     (unsigned long)p->msg_id, p->retries);

			tsm_process_send_msg(&node->fd, &node->clnt,
					     node->node_addr, p->msg);
		} else {
			LogWarn(COMPONENT_TSM,
				"ACK timeout msg_id=%lu after %d retries "
				"fsid_maj=%" PRIu64 " fsid_min=%" PRIu64
				" fileid=%" PRIu64,
				(unsigned long)p->msg_id, TSM_ACK_MAX_RETRIES,
				(uint64_t)p->msg.fsid_maj,
				(uint64_t)p->msg.fsid_min,
				(uint64_t)p->msg.fileid);

			/* After timeout, if the ack is still pending from primary or secondary, disable tsm for the export */
			if (tsm_is_ack_pending_on_primay_secondary(p->msg_id)) {
				if (p->msg.msg_type == TSM_GET_STATE) {
					tsm_initialized = 0;
					tsm_rpc_info tsm_msg3 = { 0 };
					memcpy(&tsm_msg3.source_addr,
					       &tsm_my_addr,
					       sizeof(sockaddr_t));
					tsm_msg3.msg_type = TSM_DISABLE_NOTIFY;
					tsm_disabled_source = true;
					tsm_send_msg(&tsm_msg3);

					/* Delete all records from self node as well */
					tsm_delete_all_records_all_nodes();
				} else {
					uint16_t exp_id =
						p->msg.export_info.export_id;

					LogWarn(COMPONENT_TSM,
						"ACK timeout for export_id=%u",
						exp_id);

					state_export_ll_t *exp =
						tsm_find_export_node(exp_id);

					if (exp) {
						if (exp->tsm_export_info
							    .export_tsm_enabled) {
							LogWarn(COMPONENT_TSM,
								"Disabling export_id=%u due to ACK timeout",
								exp_id);

							exp->tsm_export_info
								.export_tsm_enabled =
								false;

							tsm_broadcast_export_list(
								exp->tsm_export_info
									.export_id,
								EXPORT_REF_DEC,
								false);
						} else {
							LogDebug(
								COMPONENT_TSM,
								"export_id=%u already disabled, skipping",
								exp_id);
						}
					} else {
						LogWarn(COMPONENT_TSM,
							"export_id=%u not found in export list",
							exp_id);
					}
				}

				glist_del(glist);
				gsh_free(p);
				node->tsm_suspect = true;
			}
		}
	}
	pthread_mutex_unlock(&tsm_pending_mutex);
}

void tsm_send_msg_with_ack(tsm_rpc_info *tsm_rpc_msg)
{
	tsm_rpc_info *arg = tsm_rpc_msg;

	if (arg->msg_type == TSM_SET_STATE && arg->rec_type == TSM_OPEN_REC) {
		/* SET OPEN state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type OPEN fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_OPEN_REC) {
		/* DELETE CLOSE state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type CLOSE fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		/* SET LOCK state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type LOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		/* DELETE UNLOCK state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type UNLOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		/* SET DELEGATION state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEG fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		/* DELETE DELEGATION state logging */
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEGRETURN fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);
	}

	/* target nodes for primary and secondary replication */
	tsm_ceph_nodes_t *targets[2] = { 0 };
	int target_count = 0;

	tsm_rpc_msg->msg_id = tsm_next_msg_id();

	target_count =
		tsm_process_send_msg_peer(tsm_rpc_msg, targets, &target_count);

	/* store pending ack only for primary and secondary targets */
	for (int i = 0; i < target_count; i++) {
		tsm_add_pending_ack(targets[i], tsm_rpc_msg);
	}
}

void tsm_send_msg(tsm_rpc_info *tsm_rpc_msg)
{
	tsm_ceph_nodes_t *node;
	tsm_rpc_info *arg = tsm_rpc_msg;

	if (arg->msg_type == TSM_SET_STATE && arg->rec_type == TSM_OPEN_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type OPEN fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_OPEN_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type CLOSE fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type LOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type UNLOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEG fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEGRETURN fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);
	}

	if (!glist_empty(&tsm_hosts)) {
		struct glist_head *glist, *glist1;

		glist_for_each(glist1, &tsm_hosts) {
			node = glist_entry(glist1, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return;

			if (node->is_my_ip == 1) {
				memcpy(&tsm_rpc_msg->primary_node_addr,
				       &node->primary_node_addr,
				       sizeof(sockaddr_t));

				memcpy(&tsm_rpc_msg->secondary_node_addr,
				       &node->secondary_node_addr,
				       sizeof(sockaddr_t));

				tsm_rpc_msg->has_primary =
					node->recovery.has_primary;
				tsm_rpc_msg->has_secondary =
					node->recovery.has_secondary;
				break;
			}
		}

		glist_for_each(glist, &tsm_hosts) {
			node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return;

			if (node->is_my_ip != 1 &&
			    (!sockaddr_cmp(&node->node_addr,
					   &tsm_rpc_msg->primary_node_addr,
					   true) ||
			     !sockaddr_cmp(&node->node_addr,
					   &tsm_rpc_msg->secondary_node_addr,
					   true))) {
				tsm_process_send_msg(&node->fd, &node->clnt,
						     node->node_addr,
						     *tsm_rpc_msg);
			}
		}
	}
}

bool tsm_send_msg_peer(tsm_rpc_info *tsm_rpc_msg)
{
	tsm_rpc_info *arg = tsm_rpc_msg;

	if (arg->msg_type == TSM_SET_STATE && arg->rec_type == TSM_OPEN_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type OPEN fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_OPEN_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type CLOSE fsid_maj %lu fsid_min %lu fileid %lu share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->open_info.share_access, arg->open_info.share_deny,
			arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type LOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type UNLOCK fsid_maj %lu fsid_min %lu fileid %lu lock_start %lu lock_length %lu lock_type %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEG fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		LogDebug(
			COMPONENT_TSM,
			"rec_type DELEGRETURN fsid_maj %lu fsid_min %lu fileid %lu deleg_type %d share_access %d share_deny %d owner_str %s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);
	}

	tsm_ceph_nodes_t *node;
	bool ret = false, got_record = false;

	if (!glist_empty(&tsm_hosts)) {
		struct glist_head *glist, *glist1;

		glist_for_each(glist1, &tsm_hosts) {
			node = glist_entry(glist1, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return false;

			if (node->is_my_ip == 1) {
				memcpy(&tsm_rpc_msg->primary_node_addr,
				       &node->primary_node_addr,
				       sizeof(sockaddr_t));

				memcpy(&tsm_rpc_msg->secondary_node_addr,
				       &node->secondary_node_addr,
				       sizeof(sockaddr_t));

				tsm_rpc_msg->has_primary =
					node->recovery.has_primary;
				tsm_rpc_msg->has_secondary =
					node->recovery.has_secondary;
				break;
			}
		}

		glist_for_each(glist, &tsm_hosts) {
			node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return false;

			if (node->is_my_ip != 1 &&
			    (!sockaddr_cmp(&node->node_addr,
					   &tsm_rpc_msg->primary_node_addr,
					   true) ||
			     !sockaddr_cmp(&node->node_addr,
					   &tsm_rpc_msg->secondary_node_addr,
					   true))) {
				ret = tsm_process_send_msg(&node->fd,
							   &node->clnt,
							   node->node_addr,
							   *tsm_rpc_msg);

				if (!ret) {
					if (got_record)
						return true;
				} else {
					got_record = true;
				}
			}
		}
	}
	return ret;
}

int tsm_process_send_msg_peer(tsm_rpc_info *tsm_rpc_msg,
			      tsm_ceph_nodes_t **targets, int *target_count)
{
	struct glist_head *glist = NULL;
	tsm_ceph_nodes_t *node = NULL;

	*target_count = 0;

	/* If no cluster nodes exist, nothing to do */
	if (glist_empty(&tsm_hosts))
		return 0;

	/* Identify self node and copy primary and secondary addresses */
	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (node && node->is_my_ip == 1) {
			memcpy(&tsm_rpc_msg->primary_node_addr,
			       &node->primary_node_addr, sizeof(sockaddr_t));
			memcpy(&tsm_rpc_msg->secondary_node_addr,
			       &node->secondary_node_addr, sizeof(sockaddr_t));

			tsm_rpc_msg->has_primary = node->recovery.has_primary;
			tsm_rpc_msg->has_secondary =
				node->recovery.has_secondary;
			break;
		}
	}

	/* Send RPC message to primary and secondary peers only */
	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (!node || node->is_my_ip == 1)
			continue;

		if ((!sockaddr_cmp(&node->node_addr,
				   &tsm_rpc_msg->primary_node_addr, true)) ||
		    (!sockaddr_cmp(&node->node_addr,
				   &tsm_rpc_msg->secondary_node_addr, true))) {
			LogFullDebug(COMPONENT_TSM,
				     "Sending msg_id %lu to peer node",
				     (unsigned long)tsm_rpc_msg->msg_id);

			tsm_process_send_msg(&node->fd, &node->clnt,
					     node->node_addr, *tsm_rpc_msg);

			/* Store two target nodes i.e, primary and secondary
			 * for ACK tracking.
			 */
			if (*target_count < 2)
				targets[(*target_count)++] = node;
		}
	}

	return *target_count;
}

#if 1
/* API to delete a record from this node */
void tsm_delete_node_state(tsm_rpc_info *msg, struct glist_head *node_state)
{
	state_info_t *state = NULL;

	struct glist_head *iter = NULL;
	struct glist_head *tmp = NULL;

	struct glist_head *open_iter = NULL;
	struct glist_head *open_tmp = NULL;
	struct state_open_ll *open_ptr = NULL;

	struct glist_head *lock_iter = NULL;
	struct glist_head *lock_tmp = NULL;
	struct state_lock_ll *lock_ptr = NULL;

	struct glist_head *deleg_iter = NULL;
	struct glist_head *deleg_tmp = NULL;
	struct state_deleg_ll *deleg_ptr = NULL;

	if (glist_empty(node_state))
		return;

	/* Traverse all state entries for this node */
	glist_for_each_safe(iter, tmp, node_state) {
		state = glist_entry(iter, state_info_t, states_list);

		if (state->fsid_maj == msg->fsid_maj &&
		    state->fsid_min == msg->fsid_min &&
		    state->fileid == msg->fileid) {
			switch (msg->rec_type) {
			case TSM_OPEN_REC:

				if (!glist_empty(&state->open_info)) {
					glist_for_each_safe(open_iter, open_tmp,
							    &state->open_info) {
						open_ptr = glist_entry(
							open_iter,
							state_open_ll_t,
							open_list);

						if (open_ptr->tsm_state_open
								    .share_access ==
							    msg->open_info
								    .share_access &&
						    open_ptr->tsm_state_open
								    .share_deny ==
							    msg->open_info
								    .share_deny &&
						    (strcmp(open_ptr->tsm_state_open
								    .owner_str,
							    msg->open_info
								    .owner_str) ==
						     0)) {
							LogDebug(
								COMPONENT_TSM,
								"delete record fsid maj %lu fsid min %lu fileid %lu share_access %d share_deny %d rec_owner=%s req_owner= %s",
								state->fsid_maj,
								state->fsid_min,
								state->fileid,
								open_ptr->tsm_state_open
									.share_access,
								open_ptr->tsm_state_open
									.share_deny,
								open_ptr->tsm_state_open
									.owner_str,
								msg->open_info
									.owner_str);

							glist_del(
								&open_ptr->open_list);
							gsh_free(open_ptr);
						}
					}
				}
				break;

			case TSM_LOCK_REC:

				if (!glist_empty(&state->lock_info)) {
					glist_for_each_safe(lock_iter, lock_tmp,
							    &state->lock_info) {
						lock_ptr = glist_entry(
							lock_iter,
							state_lock_ll_t,
							lock_list);

						if (lock_ptr->tsm_state_lock
								    .start ==
							    msg->lock_info
								    .start &&
						    lock_ptr->tsm_state_lock
								    .length ==
							    msg->lock_info
								    .length &&
						    (strcmp(lock_ptr->tsm_state_lock
								    .owner_str,
							    msg->lock_info
								    .owner_str) ==
						     0)) {
							LogDebug(
								COMPONENT_TSM,
								"delete lock fsid maj %lu fsid min %lu fileid %lu start %lu length %lu",
								state->fsid_maj,
								state->fsid_min,
								state->fileid,
								lock_ptr->tsm_state_lock
									.start,
								lock_ptr->tsm_state_lock
									.length);

							glist_del(
								&lock_ptr->lock_list);
							gsh_free(lock_ptr);
						}
					}
				}
				break;

			case TSM_DELEG_REC:

				if (!glist_empty(&state->deleg_info)) {
					glist_for_each_safe(
						deleg_iter, deleg_tmp,
						&state->deleg_info) {
						deleg_ptr = glist_entry(
							deleg_iter,
							state_deleg_ll_t,
							deleg_list);

						if (deleg_ptr->tsm_state_deleg
								    .sd_type ==
							    msg->deleg_info
								    .sd_type &&
						    deleg_ptr->tsm_state_deleg
								    .share_access ==
							    msg->deleg_info
								    .share_access &&
						    (strcmp(deleg_ptr
								    ->tsm_state_deleg
								    .owner_str,
							    msg->deleg_info
								    .owner_str) ==
						     0)) {
							LogDebug(
								COMPONENT_TSM,
								"delete deleg fsid maj %lu fsid min %lu fileid %lu type %d share_access %d share_deny %d owner %s",
								state->fsid_maj,
								state->fsid_min,
								state->fileid,
								deleg_ptr
									->tsm_state_deleg
									.sd_type,
								deleg_ptr
									->tsm_state_deleg
									.share_access,
								deleg_ptr
									->tsm_state_deleg
									.share_deny,
								deleg_ptr
									->tsm_state_deleg
									.owner_str);

							glist_del(deleg_iter);
							gsh_free(deleg_ptr);

							LogFullDebug(
								COMPONENT_TSM,
								"removed deleg record fileid %lu type %d",
								state->fileid,
								msg->deleg_info
									.sd_type);
						}
					}
				}
				break;

			default:
				break;
			}

			if (glist_empty(&state->open_info) &&
			    glist_empty(&state->lock_info) &&
			    glist_empty(&state->deleg_info)) {
				LogDebug(
					COMPONENT_TSM,
					"delete file state fsid maj %lu fsid min %lu fileid %lu",
					state->fsid_maj, state->fsid_min,
					state->fileid);

				glist_del(&state->states_list);
				gsh_free(state);
			}
		}
	}
}
#endif

/* Delete ALL state records from ALL nodes */
void tsm_delete_all_records_all_nodes(void)
{
	struct glist_head *glist = NULL, *glist2 = NULL;
	struct glist_head *pos = NULL, *tmp = NULL;
	struct glist_head *p = NULL, *t = NULL;

	tsm_ceph_nodes_t *node = NULL;
	state_info_t *state = NULL;

	state_open_ll_t *open_ptr = NULL;
	state_lock_ll_t *lock_ptr = NULL;
	state_deleg_ll_t *deleg_ptr = NULL;

	if (glist_empty(&tsm_hosts))
		return;

	/* Iterate all nodes */
	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
		if (node == NULL)
			continue;

		/* Iterate all states in this node */
		glist_for_each_safe(pos, tmp, &node->state_info) {
			state = glist_entry(pos, state_info_t, states_list);

			/* delete OPEN list */
			glist_for_each_safe(p, t, &state->open_info) {
				open_ptr = glist_entry(p, state_open_ll_t,
						       open_list);
				glist_del(&open_ptr->open_list);
				gsh_free(open_ptr);
			}

			/* delete LOCK list */
			glist_for_each_safe(p, t, &state->lock_info) {
				lock_ptr = glist_entry(p, state_lock_ll_t,
						       lock_list);
				glist_del(&lock_ptr->lock_list);
				gsh_free(lock_ptr);
			}

			/* delete DELEG list */
			glist_for_each_safe(p, t, &state->deleg_info) {
				deleg_ptr = glist_entry(p, state_deleg_ll_t,
							deleg_list);
				glist_del(&deleg_ptr->deleg_list);
				gsh_free(deleg_ptr);
			}

			/* delete STATE */
			if (glist_empty(&state->open_info) &&
			    glist_empty(&state->lock_info) &&
			    glist_empty(&state->deleg_info)) {
				glist_del(&state->states_list);
				gsh_free(state);
			}
		}
	}

	/* Debug dump after cleanup */
	glist_for_each(glist2, &tsm_hosts) {
		node = glist_entry(glist2, tsm_ceph_nodes_t, node_list);
		if (node == NULL)
			continue;
		sleep(2);
		tsm_print_node_state(&node->state_info);
	}

	LogDebug(COMPONENT_TSM, "Cleaned all node state records");
}

/* -------------------- */
/* Common Helpers       */
/* -------------------- */

static inline bool tsm_same_owner(const char *a, const char *b)
{
	return (a && b && strcmp(a, b) == 0);
}

static inline bool tsm_is_reclaim_same_owner(bool_t reclaim,
					     const char *req_owner,
					     const char *rec_owner)
{
	if (reclaim && tsm_same_owner(req_owner, rec_owner)) {
		return true;
	}

	return false;
}

/* OPEN vs OPEN (using share_access only) */
static inline bool tsm_open_vs_open_conflict(uint32_t sa_existing,
					     uint32_t sa_new)
{
	if ((sa_existing == 1 && sa_new == 2) ||
	    (sa_existing == 2 && sa_new == 1))
		return true;

	if (sa_existing == 2 && sa_new == 2)
		return true;

	return false;
}

/* OPEN vs DELEG */
static inline bool tsm_open_vs_deleg_conflict(uint32_t sa_new, int deleg_type)
{
	if (sa_new == 2) /* WRITE request */
		return true;

	if (deleg_type == OPEN_DELEGATE_WRITE)
		return true;

	if (deleg_type == OPEN_DELEGATE_READ && sa_new == 2)
		return true;

	return false;
}

/* DELEG vs OPEN */
static inline bool tsm_deleg_vs_open_conflict(int deleg_type, uint32_t open_sa)
{
	if (deleg_type == OPEN_DELEGATE_WRITE)
		return true;

	if (deleg_type == OPEN_DELEGATE_READ && open_sa == 2)
		return true;

	return false;
}

/* DELEG vs DELEG */
static inline bool tsm_deleg_vs_deleg_conflict(int new_type, int existing_type)
{
	if (new_type == OPEN_DELEGATE_WRITE)
		return true;

	if (new_type == OPEN_DELEGATE_READ &&
	    existing_type == OPEN_DELEGATE_WRITE)
		return true;

	return false;
}

bool tsm_is_conflicting_open(tsm_rpc_info *msg, state_info_t *state,
			     bool_t reclaim)
{
	bool open_confl_found = false;
	struct glist_head *pos = NULL, *tmp = NULL;
	uint32_t sa_existing = 0;
	uint32_t sa_new = 0;
	state_open_ll_t *open_ptr = NULL;

	if (glist_empty(&state->open_info))
		return false;

	/* iterate open records safely */
	glist_for_each_safe(pos, tmp, &state->open_info) {
		open_ptr = glist_entry(pos, state_open_ll_t, open_list);

		sa_existing = open_ptr->tsm_state_open.share_access;
		sa_new = msg->open_info.share_access;

		if (tsm_is_reclaim_same_owner(
			    reclaim, msg->open_info.owner_str,
			    open_ptr->tsm_state_open.owner_str)) {
			LogDebug(COMPONENT_TSM, "Reclaim open. No conflict");
			continue;
		}

		if (tsm_same_owner(msg->open_info.owner_str,
				   open_ptr->tsm_state_open.owner_str)) {
			LogDebug(COMPONENT_TSM, "Same owner. No open conflict");
			continue;
		}

		if ((sa_existing == 1 && sa_new == 2) ||
		    (sa_existing == 2 && sa_new == 1)) {
			LogDebug(
				COMPONENT_TSM,
				"Open conflict (READ/WRITE) existing_owner=%s new_owner=%s",
				open_ptr->tsm_state_open.owner_str,
				msg->open_info.owner_str
					? msg->open_info.owner_str
					: "NULL");

			open_confl_found = true;
		} else if (sa_existing == 2 && sa_new == 2) {
			LogDebug(
				COMPONENT_TSM,
				"Open conflict (WRITE/WRITE) existing_owner=%s new_owner=%s",
				open_ptr->tsm_state_open.owner_str,
				msg->open_info.owner_str
					? msg->open_info.owner_str
					: "NULL");

			open_confl_found = true;
		} else {
			LogDebug(COMPONENT_TSM,
				 "Open allowed existing_sa=%u new_sa=%u",
				 sa_existing, sa_new);
		}
	}

	return open_confl_found;
}

bool tsm_is_conflicting_lock(tsm_rpc_info *msg, state_info_t *state,
			     bool_t reclaim)
{
	bool lock_confl_found = false;
	struct glist_head *pos = NULL, *tmp = NULL;
	state_lock_ll_t *lock_ptr = NULL;

	uint64_t req_start = 0;
	uint64_t req_end = 0;
	uint64_t existing_start = 0;
	uint64_t existing_end = 0;

	if (glist_empty(&state->lock_info))
		return false;

	/* iterate lock records safely */
	glist_for_each_safe(pos, tmp, &state->lock_info) {
		lock_ptr = glist_entry(pos, state_lock_ll_t, lock_list);

		req_start = msg->lock_info.start;
		req_end = req_start + msg->lock_info.length - 1;

		existing_start = lock_ptr->tsm_state_lock.start;
		existing_end =
			existing_start + lock_ptr->tsm_state_lock.length - 1;

		if (tsm_is_reclaim_same_owner(
			    reclaim, msg->lock_info.owner_str,
			    lock_ptr->tsm_state_lock.owner_str)) {
			LogDebug(COMPONENT_TSM, "Reclaim lock no conflict");
			continue;
		}

		if (tsm_same_owner(msg->lock_info.owner_str,
				   lock_ptr->tsm_state_lock.owner_str)) {
			LogDebug(COMPONENT_TSM, "Same owner no lock conflict");
			continue;
		}

		if (existing_end < req_start || existing_start > req_end)
			continue;

		if ((lock_ptr->tsm_state_lock.type == FSAL_LOCK_W ||
		     msg->lock_info.type == FSAL_LOCK_W)) {
			LogDebug(COMPONENT_TSM,
				 "Lock conflict existing_owner=%s new_owner=%s",
				 lock_ptr->tsm_state_lock.owner_str,
				 msg->lock_info.owner_str
					 ? msg->lock_info.owner_str
					 : "NULL");

			lock_confl_found = true;
		}
	}

	return lock_confl_found;
}

bool tsm_is_conflicting_deleg(struct fsal_obj_handle *obj, clientid4 *clientid,
			      open_delegation_type4 type,
			      open_claim_type4 claim, char *owner)
{
	struct glist_head *glist = NULL;
	struct glist_head *state_list_head = NULL;
	struct glist_head *open_list_head = NULL;
	struct glist_head *deleg_list_head = NULL;

	tsm_ceph_nodes_t *node = NULL;
	state_info_t *state = NULL;
	state_open_ll_t *open_rec = NULL;
	state_deleg_ll_t *deleg_rec = NULL;

	bool same_owner_claim_prev = false;

	if (glist_empty(&tsm_hosts))
		return false;

	/* iterate all nodes */
	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
		if (!node)
			continue;

		if (node->tsm_suspect)
			continue;

		state_list_head = &node->state_info;

		if (glist_empty(state_list_head))
			continue;

		glist_for_each(state_list_head, &node->state_info) {
			state = glist_entry(state_list_head, state_info_t,
					    states_list);
			if (!state)
				continue;

			if (state->fsid_maj != obj->fsid.major ||
			    state->fsid_min != obj->fsid.minor ||
			    state->fileid != obj->fileid)
				continue;

			/* check open records */
			if (!glist_empty(&state->open_info)) {
				glist_for_each(open_list_head,
					       &state->open_info) {
					open_rec = glist_entry(open_list_head,
							       state_open_ll_t,
							       open_list);

					same_owner_claim_prev =
						(claim == CLAIM_PREVIOUS &&
						 owner &&
						 open_rec->tsm_state_open
							 .owner_str &&
						 !strcmp(open_rec->tsm_state_open
								 .owner_str,
							 owner));

					if (same_owner_claim_prev) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: ALLOW DELEG vs OPEN (CLAIM_PREVIOUS owner match) Obj=%lu ReqOwner=%s RecOwner=%s",
							obj->fileid,
							owner ? owner : "NULL",
							(open_rec->tsm_state_open
								 .owner_str[0] !=
							 '\0')
								? open_rec->tsm_state_open
									  .owner_str
								: "EMPTY");
						continue;
					}

					if (claim == CLAIM_PREVIOUS) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG vs OPEN (CLAIM_PREVIOUS different owner) Obj=%lu ReqOwner=%s RecOwner=%s",
							obj->fileid,
							owner ? owner : "NULL",
							(open_rec->tsm_state_open
								 .owner_str[0] !=
							 '\0')
								? open_rec->tsm_state_open
									  .owner_str
								: "EMPTY");
						return true;
					}

					/* Requesting WRITE delegation conflicts with any OPEN */
					if (type == OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG (Write) vs OPEN Obj=%lu ReqOwner=%s RecOwner=%s RecAccess=%d RecDeny=%d",
							obj->fileid,
							owner ? owner : "NULL",
							(open_rec->tsm_state_open
								 .owner_str[0] !=
							 '\0')
								? open_rec->tsm_state_open
									  .owner_str
								: "EMPTY",
							open_rec->tsm_state_open
								.share_access,
							open_rec->tsm_state_open
								.share_deny);

						return true;
					}

					if (type == OPEN_DELEGATE_READ &&
					    ((open_rec->tsm_state_open
						      .share_access &
					      OPEN4_SHARE_ACCESS_WRITE) ||
					     (open_rec->tsm_state_open
						      .share_deny &
					      OPEN4_SHARE_ACCESS_READ))) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG (Read) vs OPEN Obj=%lu ReqOwner=%s RecOwner=%s RecAccess=%d RecDeny=%d",
							obj->fileid,
							owner ? owner : "NULL",
							(open_rec->tsm_state_open
								 .owner_str[0] !=
							 '\0')
								? open_rec->tsm_state_open
									  .owner_str
								: "EMPTY",
							open_rec->tsm_state_open
								.share_access,
							open_rec->tsm_state_open
								.share_deny);
						return true;
					}

					/* Different owner: allow READ deleg request */
					if (type == OPEN_DELEGATE_READ) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: ALLOW DELEG (Read) vs OPEN Obj=%lu ReqOwner=%s RecOwner=%s",
							obj->fileid,
							owner ? owner : "NULL",
							(open_rec->tsm_state_open
								 .owner_str[0] !=
							 '\0')
								? open_rec->tsm_state_open
									  .owner_str
								: "EMPTY");
						continue;
					}
				}
			}

			/* check deleg records */
			if (!glist_empty(&state->deleg_info)) {
				glist_for_each(deleg_list_head,
					       &state->deleg_info) {
					deleg_rec =
						glist_entry(deleg_list_head,
							    state_deleg_ll_t,
							    deleg_list);

					same_owner_claim_prev =
						(claim == CLAIM_PREVIOUS &&
						 owner &&
						 deleg_rec->tsm_state_deleg
							 .owner_str &&
						 !strcmp(deleg_rec
								 ->tsm_state_deleg
								 .owner_str,
							 owner));

					/* CLAIM_PREVIOUS with same owner → no conflict */
					if (same_owner_claim_prev) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: ALLOW DELEG vs DELEG (CLAIM_PREVIOUS owner match) Obj=%lu ReqOwner=%s RecOwner=%s",
							obj->fileid,
							owner ? owner : "NULL",
							(deleg_rec
								 ->tsm_state_deleg
								 .owner_str[0] !=
							 '\0')
								? deleg_rec
									  ->tsm_state_deleg
									  .owner_str
								: "EMPTY");
						continue;
					}

					if (claim == CLAIM_PREVIOUS) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG vs DELEG (CLAIM_PREVIOUS different owner) Obj=%lu ReqOwner=%s RecOwner=%s RecType=%d",
							obj->fileid,
							owner ? owner : "NULL",
							(deleg_rec
								 ->tsm_state_deleg
								 .owner_str[0] !=
							 '\0')
								? deleg_rec
									  ->tsm_state_deleg
									  .owner_str
								: "EMPTY",
							deleg_rec
								->tsm_state_deleg
								.sd_type);
						return true;
					}

					if (type == OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG (Write) vs DELEG Obj=%lu ReqOwner=%s RecOwner=%s RecType=%d",
							obj->fileid,
							owner ? owner : "NULL",
							(deleg_rec
								 ->tsm_state_deleg
								 .owner_str[0] !=
							 '\0')
								? deleg_rec
									  ->tsm_state_deleg
									  .owner_str
								: "EMPTY",
							deleg_rec
								->tsm_state_deleg
								.sd_type);
						return true;
					}

					if (type == OPEN_DELEGATE_READ &&
					    deleg_rec->tsm_state_deleg.sd_type ==
						    OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: DENY DELEG (Read) vs DELEG (Rec Write) Obj=%lu ReqOwner=%s RecOwner=%s",
							obj->fileid,
							owner ? owner : "NULL",
							(deleg_rec
								 ->tsm_state_deleg
								 .owner_str[0] !=
							 '\0')
								? deleg_rec
									  ->tsm_state_deleg
									  .owner_str
								: "EMPTY");
						return true;
					}

					if (type == OPEN_DELEGATE_READ) {
						LogFullDebug(
							COMPONENT_TSM,
							"TSM: ALLOW DELEG (Read) vs DELEG Obj=%lu ReqOwner=%s RecOwner=%s RecType=%d",
							obj->fileid,
							owner ? owner : "NULL",
							(deleg_rec
								 ->tsm_state_deleg
								 .owner_str[0] !=
							 '\0')
								? deleg_rec
									  ->tsm_state_deleg
									  .owner_str
								: "EMPTY",
							deleg_rec
								->tsm_state_deleg
								.sd_type);
						continue;
					}
				}
			}
		}
	}

	return false;
}

bool tsm_is_conflicting_node_state(tsm_rpc_info *msg,
				   struct glist_head *node_state,
				   bool_t reclaim)
{
	state_info_t *state = NULL;
	struct glist_head *glist = NULL;

	if (glist_empty(node_state))
		return false;

	glist_for_each(glist, node_state) {
		state = glist_entry(glist, state_info_t, states_list);

		if (state->fsid_maj != msg->fsid_maj ||
		    state->fsid_min != msg->fsid_min ||
		    state->fileid != msg->fileid)
			continue;

		switch (msg->rec_type) {
		case TSM_OPEN_REC:
			if (tsm_is_conflicting_open(msg, state, reclaim))
				return true;
			break;

		case TSM_LOCK_REC:
			if (tsm_is_conflicting_lock(msg, state, reclaim))
				return true;
			break;

		default:
			break;
		}
	}

	return false;
}

void tsm_print_node_state(struct glist_head *node_state)
{
	state_info_t *state = NULL;

	int open_count = 0, lock_count = 0, deleg_count = 0;

	struct glist_head *glist = NULL;
	struct glist_head *pos = NULL;
	struct glist_head *tmp = NULL;

	state_open_ll_t *open_state = NULL;
	state_lock_ll_t *lock_state = NULL;
	state_deleg_ll_t *deleg_state = NULL;

	if (glist_empty(node_state)) {
		LogDebug(COMPONENT_TSM, "node_state list empty");
		return;
	}

	glist_for_each(glist, node_state) {
		state = glist_entry(glist, state_info_t, states_list);

		if (!glist_empty(&state->open_info)) {
			glist_for_each_safe(pos, tmp, &state->open_info) {
				open_state = glist_entry(pos, state_open_ll_t,
							 open_list);
				open_count++;

				LogDebug(
					COMPONENT_TSM,
					"Open record fsid_maj=%lu fsid_min=%lu fileid=%lu sa=%d sd=%d",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					open_state->tsm_state_open.share_access,
					open_state->tsm_state_open.share_deny);
			}
		}

		if (!glist_empty(&state->lock_info)) {
			glist_for_each_safe(pos, tmp, &state->lock_info) {
				lock_state = glist_entry(pos, state_lock_ll_t,
							 lock_list);
				lock_count++;

				LogDebug(
					COMPONENT_TSM,
					"Lock record fsid_maj=%lu fsid_min=%lu fileid=%lu start=%lu length=%lu type=%d",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					lock_state->tsm_state_lock.start,
					lock_state->tsm_state_lock.length,
					lock_state->tsm_state_lock.type);
			}
		}

		if (!glist_empty(&state->deleg_info)) {
			glist_for_each_safe(pos, tmp, &state->deleg_info) {
				deleg_state = glist_entry(pos, state_deleg_ll_t,
							  deleg_list);
				deleg_count++;

				LogDebug(
					COMPONENT_TSM,
					"Deleg record fsid_maj=%lu fsid_min=%lu fileid=%lu type=%d sa=%d sd=%d owner=%s",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					deleg_state->tsm_state_deleg.sd_type,
					deleg_state->tsm_state_deleg
						.share_access,
					deleg_state->tsm_state_deleg.share_deny,
					deleg_state->tsm_state_deleg.owner_str);
			}
		}
	}

	LogDebug(COMPONENT_TSM,
		 "Tsm record summary total=%d open=%d lock=%d deleg=%d",
		 (open_count + lock_count + deleg_count), open_count,
		 lock_count, deleg_count);
}

/* API to check access validity */
bool tsm_is_access_valid(tsm_rpc_info *tsm_rpc_msg, bool_t reclaim)
{
	bool can_open = true;
	tsm_ceph_nodes_t *node = NULL;
	struct glist_head *glist = NULL;
	state_export_ll_t *exp_node = NULL;

	/* -------- export level check -------- */
	if (!glist_empty(&export_ids)) {
		glist_for_each(glist, &export_ids) {
			exp_node = glist_entry(glist, state_export_ll_t,
					       export_list);

			if (exp_node->tsm_export_info.export_id ==
			    tsm_rpc_msg->export_info.export_id) {
				if (exp_node->tsm_export_info
					    .export_tsm_enabled == false) {
					LogDebug(
						COMPONENT_TSM,
						"Access denied export_id=%u tsm disabled",
						exp_node->tsm_export_info
							.export_id);
					return false;
				}

				if (exp_node->tsm_export_info.export_state ==
				    EXPORT_NO_IO) {
					LogDebug(
						COMPONENT_TSM,
						"Access allowed export_id=%u no io state",
						exp_node->tsm_export_info
							.export_id);

					return true;
				}

				LogDebug(
					COMPONENT_TSM,
					"Export io in progress export_id=%u fallback to state check",
					exp_node->tsm_export_info.export_id);

				break;
			}
		}
	}

	/* -------- node level check -------- */
	if (!glist_empty(&tsm_hosts)) {
		glist_for_each(glist, &tsm_hosts) {
			node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

			if (!node)
				break;

			if (glist_empty(&node->state_info))
				continue;

			if (node->is_my_ip == true) {
				can_open = !tsm_is_conflicting_node_state(
					tsm_rpc_msg, &node->state_info,
					reclaim);

				break;
			}
		}
	}

	return can_open;
}

static void tsm_store_node_peer_record(tsm_rpc_info *msg,
				       tsm_ceph_nodes_t *node)
{
	/* store peer state received from remote node */
	node->recovery.has_primary = msg->has_primary;
	node->recovery.has_secondary = msg->has_secondary;

	node->ganesha_id = msg->ganesha_id;

	memcpy(&node->primary_node_addr, &msg->primary_node_addr,
	       sizeof(sockaddr_t));
	memcpy(&node->secondary_node_addr, &msg->secondary_node_addr,
	       sizeof(sockaddr_t));
}

static void tsm_store_prev_node_peer_record(tsm_rpc_info *msg,
					    tsm_ceph_nodes_t *node)
{
	tsm_recovery_ctx_t *ctx = &node->recovery;

	pthread_mutex_lock(&ctx->lock);

	/* update peer recovery state atomically */
	ctx->peer_responded = true;
	ctx->has_primary = msg->has_primary;
	ctx->has_secondary = msg->has_secondary;

	memcpy(&node->primary_node_addr, &msg->primary_node_addr,
	       sizeof(sockaddr_t));
	memcpy(&node->secondary_node_addr, &msg->secondary_node_addr,
	       sizeof(sockaddr_t));

	pthread_mutex_unlock(&ctx->lock);
}

/* API to store the export list for the given export*/
static void tsm_store_export_entry(tsm_rpc_info *msg)
{
	state_export_ll_t *node = NULL;
	struct glist_head *glist = NULL;

	if (!msg)
		return;

	if (glist_empty(&export_ids))
		return;

	glist_for_each(glist, &export_ids) {
		node = glist_entry(glist, state_export_ll_t, export_list);

		if (node->tsm_export_info.export_id ==
		    msg->export_info.export_id) {
			node->tsm_export_info.export_state =
				msg->export_info.export_state;

			node->tsm_export_info.export_ref_count =
				msg->export_info.export_ref_count;

			node->tsm_export_info.export_tsm_enabled =
				msg->export_info.export_tsm_enabled;

			LogDebug(
				COMPONENT_TSM,
				"Updated export_id=%u state=%u refcount=%u export_tsm_enabled=%d",
				node->tsm_export_info.export_id,
				node->tsm_export_info.export_state,
				node->tsm_export_info.export_ref_count,
				node->tsm_export_info.export_tsm_enabled);

			return;
		}
	}
}

static void tsm_store_node_state(tsm_rpc_info *msg,
				 struct glist_head *node_state)
{
	state_info_t *state = NULL;
	state_info_t *state1 = NULL;

	struct glist_head *glist = NULL;
	struct glist_head *oplist = NULL;
	struct glist_head *locklist = NULL;
	struct glist_head *deleglist = NULL;

	struct state_open_ll *open_state = NULL;
	struct state_lock_ll *lock_state = NULL;
	struct state_deleg_ll *deleg_state = NULL;

	bool file_rec_found = 0;
	bool open_rec_found = 0;
	bool lock_rec_found = 0;
	bool deleg_rec_found = 0;

	if (!glist_empty(node_state)) {
		glist_for_each(glist, node_state) {
			state = glist_entry(glist, state_info_t, states_list);

			if (state->fsid_maj == msg->fsid_maj &&
			    state->fsid_min == msg->fsid_min &&
			    state->fileid == msg->fileid) {
				file_rec_found = 1;
				break;
			}
		}
	}

	if (glist_empty(node_state) == true || file_rec_found == 0) {
		state1 = gsh_calloc(1, sizeof(state_info_t));
		glist_init(&state1->states_list);
		glist_init(&state1->open_info);
		glist_init(&state1->lock_info);
		glist_init(&state1->deleg_info);
		glist_init(&state1->layout_info);

		state1->fsid_maj = msg->fsid_maj;
		state1->fsid_min = msg->fsid_min;
		state1->fileid = msg->fileid;

		glist_add_tail(node_state, &state1->states_list);
		state = state1;
	}

	switch (msg->rec_type) {
	case TSM_OPEN_REC:

		if (!glist_empty(&state->open_info)) {
			glist_for_each(oplist, &state->open_info) {
				open_state = glist_entry(oplist,
							 state_open_ll_t,
							 open_list);

				if (open_state->tsm_state_open.share_access ==
					    msg->open_info.share_access &&
				    open_state->tsm_state_open.share_deny ==
					    msg->open_info.share_deny &&
				    (strcmp(open_state->tsm_state_open.owner_str,
					    msg->open_info.owner_str) == 0)) {
					open_rec_found = 1;
				}
			}
		}

		if (glist_empty(&state->open_info) == true ||
		    open_rec_found == 0) {
			open_state =
				gsh_calloc(1, sizeof(struct state_open_ll));
			glist_init(&open_state->open_list);

			open_state->tsm_state_open.share_access =
				msg->open_info.share_access;

			open_state->tsm_state_open.share_deny =
				msg->open_info.share_deny;

			strncpy(open_state->tsm_state_open.owner_str,
				msg->open_info.owner_str,
				sizeof(open_state->tsm_state_open.owner_str));

			glist_add_tail(&state->open_info,
				       &open_state->open_list);

			LogDebug(
				COMPONENT_TSM,
				"Added TSM open record fsid maj:%lu fsid min:%lu fileid:%lu sa:%d sd:%d",
				state->fsid_maj, state->fsid_min, state->fileid,
				open_state->tsm_state_open.share_access,
				open_state->tsm_state_open.share_deny);
		}
		break;

	case TSM_LOCK_REC:

		if (!glist_empty(&state->lock_info)) {
			glist_for_each(locklist, &state->lock_info) {
				lock_state = glist_entry(locklist,
							 state_lock_ll_t,
							 lock_list);

				if (lock_state->tsm_state_lock.type ==
					    msg->lock_info.type &&
				    lock_state->tsm_state_lock.start ==
					    msg->lock_info.start &&
				    lock_state->tsm_state_lock.length ==
					    msg->lock_info.length &&
				    (strcmp(lock_state->tsm_state_lock.owner_str,
					    msg->lock_info.owner_str) == 0)) {
					lock_rec_found = 1;
				}
			}
		}

		if (glist_empty(&state->lock_info) == true ||
		    lock_rec_found == 0) {
			lock_state =
				gsh_calloc(1, sizeof(struct state_lock_ll));
			glist_init(&lock_state->lock_list);

			lock_state->tsm_state_lock.type = msg->lock_info.type;
			lock_state->tsm_state_lock.start = msg->lock_info.start;
			lock_state->tsm_state_lock.length =
				msg->lock_info.length;

			strncpy(lock_state->tsm_state_lock.owner_str,
				msg->lock_info.owner_str,
				sizeof(lock_state->tsm_state_lock.owner_str));

			glist_add_tail(&state->lock_info,
				       &lock_state->lock_list);

			LogDebug(
				COMPONENT_TSM,
				"Added TSM lock record fsid maj:%lu fsid min:%lu fileid:%lu start:%lu length:%lu",
				state->fsid_maj, state->fsid_min, state->fileid,
				lock_state->tsm_state_lock.start,
				lock_state->tsm_state_lock.length);
		}
		break;

	case TSM_DELEG_REC:

		if (!glist_empty(&state->deleg_info)) {
			glist_for_each(deleglist, &state->deleg_info) {
				deleg_state = glist_entry(deleglist,
							  state_deleg_ll_t,
							  deleg_list);

				if (deleg_state->tsm_state_deleg.sd_type ==
					    msg->deleg_info.sd_type &&
				    deleg_state->tsm_state_deleg.share_access ==
					    msg->deleg_info.share_access &&
				    deleg_state->tsm_state_deleg.share_deny ==
					    msg->deleg_info.share_deny &&
				    strcmp(deleg_state->tsm_state_deleg
						   .owner_str,
					   msg->deleg_info.owner_str) == 0) {
					deleg_rec_found = 1;
				}
			}
		}

		if (glist_empty(&state->deleg_info) == true ||
		    deleg_rec_found == 0) {
			deleg_state =
				gsh_calloc(1, sizeof(struct state_deleg_ll));
			glist_init(&deleg_state->deleg_list);

			deleg_state->tsm_state_deleg.sd_type =
				msg->deleg_info.sd_type;

			deleg_state->tsm_state_deleg.sd_state =
				msg->deleg_info.sd_state;

			deleg_state->tsm_state_deleg.share_access =
				msg->deleg_info.share_access;

			deleg_state->tsm_state_deleg.share_deny =
				msg->deleg_info.share_deny;

			strncpy(deleg_state->tsm_state_deleg.owner_str,
				msg->deleg_info.owner_str,
				sizeof(deleg_state->tsm_state_deleg.owner_str));

			glist_add_tail(&state->deleg_info,
				       &deleg_state->deleg_list);

			LogDebug(
				COMPONENT_TSM,
				"Added TSM deleg record fsid maj:%lu fsid min:%lu fileid:%lu type:%d sa:%d sd:%d owner:%s",
				state->fsid_maj, state->fsid_min, state->fileid,
				deleg_state->tsm_state_deleg.sd_type,
				deleg_state->tsm_state_deleg.share_access,
				deleg_state->tsm_state_deleg.share_deny,
				deleg_state->tsm_state_deleg.owner_str);
		}
		break;

	default:
		break;
	}

	/* print the record */
	tsm_print_node_state(node_state);
}

/* API to store the previous export list of this node
 * before its reboot. This is called when the export list
 * of this node, arrives from the peer node.
 */
static void tsm_store_prev_export_list(tsm_rpc_states *msg)
{
	int i = 0;
	tsm_rpc_info *exp;

	if (!msg || !msg->tsmarray_val)
		return;

	exp = msg->tsmarray_val;

	for (i = 0; i < msg->tsmrec_len; i++) {
		tsm_store_export_entry(exp);
		exp++;
	}
}

/* API to store the previous state records of this node
 * before its reboot. This is called when the state records
 * of this node, arrive from the peer node.
 */
static void tsm_store_prev_state(tsm_rpc_states *msg,
				 struct glist_head *node_state)
{
	int i = 0;
	tsm_rpc_info *state = msg->tsmarray_val;

	if (!msg || !msg->tsmarray_val)
		return;

	state = msg->tsmarray_val;
	for (i = 0; i < msg->tsmrec_len; i++) {
		tsm_store_node_state(state, node_state);
		state++;
	}
}

/* Reply to any state record request of any node*/
void tsm_reply_node_state(tsm_ceph_nodes_t *node)
{
	tsm_rpc_states rpc_state;
	tsm_rpc_info *rpc_msg = NULL;
	state_info_t *state = NULL;

	struct glist_head *glist = NULL;
	struct glist_head *oplist = NULL;

	struct state_open_ll *open_ptr = NULL;
	struct state_lock_ll *lock_ptr = NULL;
	struct state_deleg_ll *deleg_ptr = NULL;

	size_t len = 0;
	int total_records = 0;

	memset(&rpc_state, 0, sizeof(rpc_state));

	/* early exit: no state records */
	if (glist_empty(&node->state_info))
		goto send_state_records;

	glist_for_each(glist, &node->state_info) {
		state = glist_entry(glist, state_info_t, states_list);

		if (state == NULL)
			goto send_state_records;

		/* OPEN */
		if (!glist_empty(&state->open_info)) {
			glist_for_each(oplist, &state->open_info) {
				total_records++;
				rpc_state.tsmrec_len++;

				if (total_records == 1)
					rpc_state.tsmarray_val = gsh_calloc(
						1, sizeof(tsm_rpc_info));
				else
					rpc_state.tsmarray_val = gsh_realloc(
						rpc_state.tsmarray_val,
						total_records *
							sizeof(tsm_rpc_info));

				rpc_msg = rpc_state.tsmarray_val +
					  (total_records - 1);

				rpc_msg->fsid_maj = state->fsid_maj;
				rpc_msg->fsid_min = state->fsid_min;
				rpc_msg->fileid = state->fileid;

				rpc_msg->msg_type = TSM_REPLY_STATE;
				rpc_msg->rec_type = TSM_OPEN_REC;

				open_ptr = glist_entry(oplist, state_open_ll_t,
						       open_list);

				rpc_msg->open_info.share_access =
					open_ptr->tsm_state_open.share_access;
				rpc_msg->open_info.share_deny =
					open_ptr->tsm_state_open.share_deny;

				len = sizeof(rpc_msg->open_info.owner_str) - 1;
				memcpy(rpc_msg->open_info.owner_str,
				       open_ptr->tsm_state_open.owner_str, len);
				rpc_msg->open_info.owner_str[len] = '\0';

				LogDebug(
					COMPONENT_TSM,
					"Reply OPEN fsid_maj=%lu fsid_min=%lu fileid=%lu sa=%d sd=%d",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					rpc_msg->open_info.share_access,
					rpc_msg->open_info.share_deny);
			}
		}

		/* LOCK */
		if (!glist_empty(&state->lock_info)) {
			glist_for_each(oplist, &state->lock_info) {
				total_records++;
				rpc_state.tsmrec_len++;

				if (total_records == 1)
					rpc_state.tsmarray_val = gsh_calloc(
						1, sizeof(tsm_rpc_info));
				else
					rpc_state.tsmarray_val = gsh_realloc(
						rpc_state.tsmarray_val,
						total_records *
							sizeof(tsm_rpc_info));

				rpc_msg = rpc_state.tsmarray_val +
					  (total_records - 1);

				rpc_msg->fsid_maj = state->fsid_maj;
				rpc_msg->fsid_min = state->fsid_min;
				rpc_msg->fileid = state->fileid;

				rpc_msg->msg_type = TSM_REPLY_STATE;
				rpc_msg->rec_type = TSM_LOCK_REC;

				lock_ptr = glist_entry(oplist, state_lock_ll_t,
						       lock_list);

				rpc_msg->lock_info.type =
					lock_ptr->tsm_state_lock.type;
				rpc_msg->lock_info.start =
					lock_ptr->tsm_state_lock.start;
				rpc_msg->lock_info.length =
					lock_ptr->tsm_state_lock.length;

				len = sizeof(rpc_msg->lock_info.owner_str) - 1;
				memcpy(rpc_msg->lock_info.owner_str,
				       lock_ptr->tsm_state_lock.owner_str, len);
				rpc_msg->lock_info.owner_str[len] = '\0';

				LogDebug(
					COMPONENT_TSM,
					"Reply LOCK fsid_maj=%lu fsid_min=%lu fileid=%lu start=%lu len=%lu type=%d",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					lock_ptr->tsm_state_lock.start,
					lock_ptr->tsm_state_lock.length,
					lock_ptr->tsm_state_lock.type);
			}
		}

		/* DELEG */
		if (!glist_empty(&state->deleg_info)) {
			glist_for_each(oplist, &state->deleg_info) {
				total_records++;
				rpc_state.tsmrec_len++;

				if (total_records == 1)
					rpc_state.tsmarray_val = gsh_calloc(
						1, sizeof(tsm_rpc_info));
				else
					rpc_state.tsmarray_val = gsh_realloc(
						rpc_state.tsmarray_val,
						total_records *
							sizeof(tsm_rpc_info));

				rpc_msg = rpc_state.tsmarray_val +
					  (total_records - 1);

				rpc_msg->fsid_maj = state->fsid_maj;
				rpc_msg->fsid_min = state->fsid_min;
				rpc_msg->fileid = state->fileid;

				rpc_msg->msg_type = TSM_REPLY_STATE;
				rpc_msg->rec_type = TSM_DELEG_REC;

				deleg_ptr = glist_entry(oplist,
							state_deleg_ll_t,
							deleg_list);

				rpc_msg->deleg_info.sd_type =
					deleg_ptr->tsm_state_deleg.sd_type;
				rpc_msg->deleg_info.sd_state =
					deleg_ptr->tsm_state_deleg.sd_state;
				rpc_msg->deleg_info.share_access =
					deleg_ptr->tsm_state_deleg.share_access;
				rpc_msg->deleg_info.share_deny =
					deleg_ptr->tsm_state_deleg.share_deny;

				len = sizeof(rpc_msg->deleg_info.owner_str) - 1;
				memcpy(rpc_msg->deleg_info.owner_str,
				       deleg_ptr->tsm_state_deleg.owner_str,
				       len);
				rpc_msg->deleg_info.owner_str[len] = '\0';

				LogDebug(
					COMPONENT_TSM,
					"Reply DELEG fsid_maj=%lu fsid_min=%lu fileid=%lu type=%d sa=%d sd=%d",
					state->fsid_maj, state->fsid_min,
					state->fileid,
					deleg_ptr->tsm_state_deleg.sd_type,
					deleg_ptr->tsm_state_deleg.share_access,
					deleg_ptr->tsm_state_deleg.share_deny);
			}
		}
	}

send_state_records:

	if (total_records != 0) {
		tsm_process_send_state(&node->fd, &node->clnt, node->node_addr,
				       &rpc_state);

		tsm_print_node_state(&node->state_info);

		gsh_free(rpc_state.tsmarray_val);
	}
}

/* REPLY to any export list request of any given node */
void tsm_reply_node_exports(tsm_ceph_nodes_t *node)
{
	tsm_rpc_states rpc_state;
	tsm_rpc_info *rpc_msg;
	state_export_ll_t *exp_entry;

	struct glist_head *glist;
	int total_records = 0;

	memset(&rpc_state, 0, sizeof(rpc_state));

	/* early exit: no exports */
	if (glist_empty(&export_ids))
		goto send_export_records;

	glist_for_each(glist, &export_ids) {
		exp_entry = glist_entry(glist, state_export_ll_t, export_list);

		if (!exp_entry)
			continue;

		total_records++;
		rpc_state.tsmrec_len++;

		if (total_records == 1)
			rpc_state.tsmarray_val =
				gsh_calloc(1, sizeof(tsm_rpc_info));
		else
			rpc_state.tsmarray_val = gsh_realloc(
				rpc_state.tsmarray_val,
				total_records * sizeof(tsm_rpc_info));

		rpc_msg = rpc_state.tsmarray_val + (total_records - 1);

		rpc_msg->msg_type = TSM_REPLY_EXPORT_LIST;

		rpc_msg->export_info.export_id =
			exp_entry->tsm_export_info.export_id;

		rpc_msg->export_info.export_state =
			exp_entry->tsm_export_info.export_state;

		rpc_msg->export_info.export_ref_count =
			exp_entry->tsm_export_info.export_ref_count;

		rpc_msg->export_info.export_tsm_enabled =
			exp_entry->tsm_export_info.export_tsm_enabled;

		LogDebug(COMPONENT_TSM,
			 "Reply EXPORT export_id=%u state=%u ref=%u enabled=%d",
			 exp_entry->tsm_export_info.export_id,
			 exp_entry->tsm_export_info.export_state,
			 exp_entry->tsm_export_info.export_ref_count,
			 exp_entry->tsm_export_info.export_tsm_enabled);
	}

send_export_records:

	if (total_records != 0) {
		tsm_process_send_state(&node->fd, &node->clnt, node->node_addr,
				       &rpc_state);

		gsh_free(rpc_state.tsmarray_val);
	}
}

void tsm_reply_node_peer_rec(tsm_ceph_nodes_t *node)
{
	tsm_rpc_info rpc_msg;

	memcpy(&rpc_msg.source_addr, &node->node_addr, sizeof(sockaddr_t));
	rpc_msg.msg_type = TSM_SET_PEER_NODE_INFO;

	rpc_msg.has_primary = node->recovery.has_primary;
	rpc_msg.has_secondary = node->recovery.has_secondary;
	memcpy(&rpc_msg.primary_node_addr, &node->primary_node_addr,
	       sizeof(sockaddr_t));
	memcpy(&rpc_msg.secondary_node_addr, &node->secondary_node_addr,
	       sizeof(sockaddr_t));

	tsm_process_send_msg(&node->fd, &node->clnt, node->node_addr, rpc_msg);
}

void tsm_process_recd_msg(tsm_rpc_info *msg)
{
	tsm_ceph_nodes_t *node = NULL;
	tsm_ceph_nodes_t *ack_node = NULL;
	struct tsm_ack_event *ev = NULL;
	struct glist_head *glist = NULL;

	LogFullDebug(COMPONENT_TSM, "TSM msg received: msg_type=%d",
		     msg->msg_type);

	if (msg->msg_type == TSM_ENABLE_NOTIFY) {
		LogFullDebug(COMPONENT_TSM, "Processing TSM_ENABLE_NOTIFY");

		tsm_initialized = 1;
		return;
	}

	if (msg->msg_type == TSM_DISABLE_NOTIFY) {
		LogFullDebug(COMPONENT_TSM, "Processing TSM_DISABLE_NOTIFY");

		tsm_initialized = 0;
		tsm_delete_all_records_all_nodes();
		return;
	}

	if (msg->msg_type == TSM_ACK_STATE) {
		ack_node = tsm_find_node_by_addr(&msg->source_addr);
		if (!ack_node)
			return;

		LogFullDebug(COMPONENT_TSM, "TSM ACK received msg_id=%lu",
			     (unsigned long)msg->msg_id);

		ev = gsh_malloc(sizeof(*ev));
		if (!ev) {
			LogWarn(COMPONENT_TSM, "TSM ack allocation failed");
			return;
		}

		ev->node = ack_node;
		ev->msg_id = msg->msg_id;
		glist_init(&ev->list);

		pthread_mutex_lock(&tsm_ack_mutex);
		glist_add_tail(&tsm_ack_events, &ev->list);
		pthread_cond_signal(&tsm_ack_cond);
		pthread_mutex_unlock(&tsm_ack_mutex);

		return;
	}

	if (msg->msg_type == TSM_EXPORT_ID_NOTIFY) {
		tsm_store_export_state(msg->export_info.export_id,
				       msg->export_info.export_event,
				       msg->export_info.export_tsm_enabled);
		return;
	}

	if (glist_empty(&tsm_hosts)) {
		LogFullDebug(COMPONENT_TSM, "TSM host list empty");
		return;
	}

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (!node) {
			LogFullDebug(COMPONENT_TSM, "NULL node in host list");
			return;
		}

		if (sockaddr_cmp(&node->node_addr, &msg->source_addr, true))
			continue;

		LogFullDebug(COMPONENT_TSM, "Matched node for message type=%d",
			     msg->msg_type);

		switch (msg->msg_type) {
		case TSM_SET_STATE:
			LogFullDebug(COMPONENT_TSM, "Processing TSM_SET_STATE");

			tsm_store_node_state(msg, &node->state_info);
			tsm_send_ack_to_peer(node, msg);
			return;

		case TSM_GET_STATE:
			LogFullDebug(COMPONENT_TSM, "Processing TSM_GET_STATE");

			node->is_state_requested = true;
			tsm_send_ack_to_peer(node, msg);
			return;

		case TSM_DELETE_STATE:
			LogFullDebug(COMPONENT_TSM,
				     "Processing TSM_DELETE_STATE");

			tsm_delete_node_state(msg, &node->state_info);
			tsm_print_node_state(&node->state_info);
			tsm_send_ack_to_peer(node, msg);
			return;

		case TSM_PEER_IP_NOTIFY:
			LogFullDebug(COMPONENT_TSM,
				     "Processing TSM_PEER_IP_NOTIFY");

			tsm_store_node_peer_record(msg, node);
			return;

		case TSM_SET_PEER_NODE_INFO:
			LogFullDebug(COMPONENT_TSM,
				     "Processing TSM_SET_PEER_NODE_INFO");

			tsm_store_prev_node_peer_record(msg, node);
			return;

		case TSM_GET_PEER_NODE_INFO:
			LogFullDebug(COMPONENT_TSM,
				     "Processing TSM_GET_PEER_NODE_INFO");

			tsm_reply_node_peer_rec(node);
			tsm_reply_node_exports(node);
			return;

		case TSM_RECLAIM_EXPORT_IDS:
			LogFullDebug(COMPONENT_TSM,
				     "Processing TSM_RECLAIM_EXPORT_IDS");
			return;

		default:
			LogFullDebug(COMPONENT_TSM, "Unknown msg_type=%d",
				     msg->msg_type);
			return;
		}
	}

	LogFullDebug(COMPONENT_TSM, "No matching node found");
}

void tsm_process_recd_states(tsm_rpc_states *msg)
{
	tsm_ceph_nodes_t *node = NULL;
	struct glist_head *glist = NULL;

	LogDebug(COMPONENT_TSM, "Export list populated");
	tsm_store_prev_export_list(msg);

	if (glist_empty(&tsm_hosts))
		return;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (!node)
			return;

		if (!node->is_my_ip)
			continue;

		tsm_store_prev_state(msg, &node->state_info);
		return;
	}
}

/* Function to store the export list on this node */
void tsm_store_export_state(uint16_t export_id, uint16_t export_event,
			    bool tsm_enabled)
{
	state_export_ll_t *node = NULL;
	struct glist_head *glist = NULL;

	if (glist_empty(&export_ids))
		return;

	glist_for_each(glist, &export_ids) {
		node = glist_entry(glist, state_export_ll_t, export_list);

		if (node->tsm_export_info.export_id != export_id)
			continue;

		node->tsm_export_info.export_event = export_event;
		node->tsm_export_info.export_tsm_enabled = tsm_enabled;

		/* EXPORT_REF_INC will be sent for OPEN, LOCK, DELEG
		 * EXPORT_REF_DEC will be sent for CLOSE, UNLOCK, DELEGRETURN 
		 */
		if (export_event == EXPORT_REF_INC)
			node->tsm_export_info.export_ref_count++;
		else if (export_event == EXPORT_REF_DEC &&
			 node->tsm_export_info.export_ref_count > 0)
			node->tsm_export_info.export_ref_count--;

		if (node->tsm_export_info.export_ref_count == 0)
			node->tsm_export_info.export_state = EXPORT_NO_IO;
		else
			node->tsm_export_info.export_state =
				EXPORT_IO_IN_PROGRESS;

		LogDebug(COMPONENT_TSM,
			 "Saving export export_id=%u event=%u state=%u ref=%u",
			 export_id, export_event,
			 node->tsm_export_info.export_state,
			 node->tsm_export_info.export_ref_count);

		return;
	}
}
static void *tsm_ack_thread_func(void *arg)
{
	tsm_ceph_nodes_t *node = NULL;
	tsm_ceph_nodes_t *my_node = NULL;
	struct glist_head *glist = NULL;

	/* Resolve my_node once */
	while (!my_node) {
		my_node = tsm_find_node_by_addr(&tsm_my_addr);
		if (!my_node) {
			LogFullDebug(COMPONENT_TSM,
				     "TSM: Waiting for my_node resolution...");
			usleep(200000);
		}
	}

	while (true) {
		/* Wait until primary + secondary are ready */
		if (!my_node->recovery.has_primary ||
		    !my_node->recovery.has_secondary) {
			usleep(200000);
			continue;
		}

		/* Step 1: process incoming ACKs */
		tsm_drain_ack_events();

		/* Step 2: retry or fail pending ACKs */
		if (!glist_empty(&tsm_hosts)) {
			glist_for_each(glist, &tsm_hosts) {
				node = glist_entry(glist, tsm_ceph_nodes_t,
						   node_list);

				if (!node)
					continue;

				if ((!sockaddr_cmp(&node->node_addr,
						   &my_node->primary_node_addr,
						   true)) ||
				    (!sockaddr_cmp(&node->node_addr,
						   &my_node->secondary_node_addr,
						   true))) {
					tsm_retry_or_fail_pending_acks_for_node(
						node);
				}
			}
		}

		usleep(200000);
	}

	return NULL;
}

static void *tsm_thread_func(void *arg)
{
	tsm_ceph_nodes_t *node = NULL;
	struct glist_head *glist = NULL;

	while (true) {
		if (!glist_empty(&tsm_hosts)) {
			glist_for_each(glist, &tsm_hosts) {
				node = glist_entry(glist, tsm_ceph_nodes_t,
						   node_list);

				if (!node)
					continue;

				if (node->is_state_requested == true) {
					tsm_reply_node_state(node);
					node->is_state_requested = false;
				}
			}
		}

		usleep(200000);
	}

	return NULL;
}

static void tsm_thread_init(void)
{
	int ret = 0;

	if (tsm_initialized == 0) {
		ret = pthread_create(&tsm_thread, NULL, tsm_thread_func, NULL);

		if (ret != 0) {
			LogFatal(COMPONENT_TSM,
				 "TSM: Thread creation failed error %d (%s)",
				 errno, strerror(errno));
		}

		pthread_detach(tsm_thread);
	}

	ret = pthread_create(&tsm_thread2, NULL, tsm_ack_thread_func, NULL);

	if (ret != 0) {
		LogFatal(COMPONENT_TSM,
			 "TSM: Thread creation failed error %d (%s)", errno,
			 strerror(errno));
	}

	pthread_detach(tsm_thread2);

	tsm_initialized = 1;
	tsm_disabled_source = false;

	LogDebug(COMPONENT_TSM, "TSM: Cluster QOS thread is initialized");
}

void tsm_init(void)
{
	if (tsm_initialized == 0) {
		LogDebug(COMPONENT_TSM, "TSM: Cluster QOS thread_init");

		tsm_thread_init();
	}
}
