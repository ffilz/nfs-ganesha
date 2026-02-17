#include <stdlib.h>
#include "nfs_core.h"
#include "transparent_recovery.h"
#include "tsm.h"
#include <rpc/svc_rqst.h>

unsigned int tsm_initialized;
struct glist_head tsm_hosts = GLIST_HEAD_INIT(tsm_hosts);
static const struct timespec msg_tout = { 0, 200000000 };
sockaddr_t tsm_my_addr;

static void tsm_rpc_call_free(struct clnt_req *cc, size_t unused)
{
	gsh_free(cc);
}

static void tsm_rpc_call_process(struct clnt_req *cc)
{
	LogFullDebug(COMPONENT_QOS, "CQOS: callback cc=%p status=%d", cc,
		     cc->cc_error.re_status);
	if (cc->cc_error.re_status == RPC_SUCCESS) {
		LogDebug(COMPONENT_QOS, "CQOS: RPC msg sent OK");
	} else {
		LogCrit(COMPONENT_QOS, "CQOS: Sending RPC msg failed status=%d",
			cc->cc_error.re_status);
	}

	/* Free only after RPC layer signals completion */
	clnt_req_release(cc);
}

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
		LogCrit(COMPONENT_QOS, "CQOS: evchan registration failed (%d)",
			code);
		return false;
	}

	LogFullDebug(COMPONENT_QOS, "CQOS: evchan ok clnt=%p xprt=%p", clnt,
		     xprt);
	return true;
}

static bool tsm_send_rpc_msg(CLIENT *clnt, struct tsm_rpc_info msg)
{
	struct clnt_req *cc;

	if (clnt == NULL)
		return false;

	cc = gsh_malloc(sizeof(*cc));

	clnt_req_fill(cc, clnt, authnone_ncreate(), 1,
		      (xdrproc_t)xdr_tsm_rpc_info, &msg, (xdrproc_t)xdr_void,
		      NULL);

	cc->cc_error.re_status = clnt_req_setup(cc, msg_tout);
	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_QOS, "CQOS: Sending RPC msg failed");
		clnt_req_release(cc);
		return false;
	}

	if (!tsm_ensure_evchan(clnt)) {
		LogCrit(COMPONENT_QOS,
			"CQOS: evchan missing for CLNT_CALL_BACK");
		clnt_req_release(cc);
		return false;
	}

	cc->cc_refreshes = 0;
	cc->cc_process_cb = tsm_rpc_call_process;
	cc->cc_free_cb = tsm_rpc_call_free;

	LogFullDebug(COMPONENT_QOS, "CQOS: CLNT_CALL_BACK start cc=%p", cc);
	cc->cc_error.re_status = CLNT_CALL_BACK(cc);
	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_QOS, "CQOS: Sending RPC msg failed");
		clnt_req_release(cc);
		return false;
	}

	LogFullDebug(COMPONENT_QOS, "CQOS: CLNT_CALL_BACK queued cc=%p", cc);
	return true;
}

static bool tsm_send_rpc_state(CLIENT *clnt, struct tsm_rpc_states *state)
{
	struct clnt_req *cc;

	if (clnt == NULL)
		return false;

	cc = gsh_malloc(sizeof(*cc));

	clnt_req_fill(cc, clnt, authnone_ncreate(), 2,
		      (xdrproc_t)xdr_tsm_rpc_states, state, (xdrproc_t)xdr_void,
		      NULL);

	cc->cc_error.re_status = clnt_req_setup(cc, msg_tout);
	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_QOS, "CQOS: Sending RPC msg failed");
		clnt_req_release(cc);
		return false;
	}

	if (!tsm_ensure_evchan(clnt)) {
		LogCrit(COMPONENT_QOS,
			"CQOS: evchan missing for CLNT_CALL_BACK");
		clnt_req_release(cc);
		return false;
	}

	cc->cc_refreshes = 0;
	cc->cc_process_cb = tsm_rpc_call_process;
	cc->cc_free_cb = tsm_rpc_call_free;

	LogFullDebug(COMPONENT_QOS, "CQOS: CLNT_CALL_BACK start cc=%p", cc);
	cc->cc_error.re_status = CLNT_CALL_BACK(cc);
	if (cc->cc_error.re_status != RPC_SUCCESS) {
		LogCrit(COMPONENT_QOS, "CQOS: Sending RPC msg failed");
		clnt_req_release(cc);
		return false;
	}

	LogFullDebug(COMPONENT_QOS, "CQOS: CLNT_CALL_BACK queued cc=%p", cc);
	return true;
}

static int tsm_create_socket(sockaddr_t *sockaddr)
{
	int r = -1;
	int fd = -1;
	struct sockaddr_in *in1 = (struct sockaddr_in *)sockaddr;
	struct sockaddr_in6 *in2 = (struct sockaddr_in6 *)sockaddr;

	switch (sockaddr->ss_family) {
	case AF_INET: {
		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM socket(AF_INET) create failed");
			break;
		}
		in1->sin_port = htons(TSM_PORT);
		r = connect(fd, (struct sockaddr *)in1,
			    sizeof(struct sockaddr));
		if (!r)
			goto done;
		LogDebug(COMPONENT_QOS,
			 "CQOS: TSM connect(AF_INET) failed, closing fd=%d",
			 fd);
		close(fd);
		break;
	}
	case AF_INET6: {
		fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM socket(AF_INET6) create failed");
			break;
		}
		in2->sin6_port = htons(TSM_PORT);
		r = connect(fd, (struct sockaddr *)in2,
			    sizeof(struct sockaddr));
		if (!r)
			goto done;
		LogDebug(COMPONENT_QOS,
			 "CQOS: TSM connect(AF_INET6) failed, closing fd=%d",
			 fd);
		close(fd);
		break;
	}
	default:
		break;
	}
	return -1;
done:
	LogDebug(COMPONENT_QOS, "CQOS: TSM socket connected fd=%d", fd);
	return fd;
}

/**
 * This function creates rpc client handle for given fd.
 *
 * @param [in]  fd which is already connected to remote node.
 * @return      rpc client handle.
 *              NULL if rpc client handle can't be created.
 */
static CLIENT *tsm_create_rpc_client(int fd)
{
	CLIENT *clnt = NULL;
	SVCXPRT *xprt = NULL;
	int code;

	if (fd < TSM_VALID_FD) {
		LogCrit(COMPONENT_QOS, "CQOS: Invalid Fd to create client");
		return NULL;
	}

	struct sockaddr_storage ss;

	struct netbuf raddr = { .buf = &ss, .len = sizeof(ss) };

	clnt = clnt_vc_ncreatef(fd, &raddr, TSMPROG, TSM_VERS, 0, 0,
				CLNT_CREATE_FLAG_CLOSE);
	if (CLNT_FAILURE(clnt)) {
		LogCrit(COMPONENT_QOS, "CQOS: RPC client create failed");
		return NULL;
	}

	xprt = clnt_vc_get_client_xprt(clnt);
	if (xprt == NULL) {
		LogCrit(COMPONENT_QOS, "CQOS: RPC client xprt is NULL");
		CLNT_DESTROY(clnt);
		return NULL;
	}

	/* Register on a (global/legacy) event channel so CLNT_CALL_BACK works */
	code = svc_rqst_evchan_reg(0, xprt, SVC_RQST_FLAG_NONE);
	if (code != 0) {
		LogCrit(COMPONENT_QOS, "CQOS: evchan registration failed (%d)",
			code);
		CLNT_DESTROY(clnt);
		return NULL;
	}

	LogFullDebug(COMPONENT_QOS, "CQOS: evchan ok clnt=%p xprt=%p", clnt,
		     xprt);
	return clnt;
}

static void tsm_process_send_msg(int *fd, CLIENT **clnt, sockaddr_t sockaddr,
				 tsm_rpc_info tsm_msg)
{
	int retries = 0;

retry:
	if ((*fd >= TSM_VALID_FD) && (*clnt != NULL)) {
		if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM send failed, resetting client (fd=%d, clnt=%p, retry=%d)",
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
			if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
				LogCrit(COMPONENT_QOS,
					"CQOS: TSM send failed after client create (fd=%d, clnt=%p, retry=%d)",
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
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM client create failed (fd=%d, retry=%d)",
				*fd, retries);
		}
	} else if (*fd < TSM_VALID_FD) {
		*fd = tsm_create_socket(&sockaddr);
		if (*fd >= TSM_VALID_FD) {
			*clnt = tsm_create_rpc_client(*fd);
			if (*clnt != NULL) {
				if (!tsm_send_rpc_msg(*clnt, tsm_msg)) {
					LogCrit(COMPONENT_QOS,
						"CQOS: TSM send failed after connect (fd=%d, clnt=%p, retry=%d)",
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
				LogCrit(COMPONENT_QOS,
					"CQOS: TSM client create failed after connect (fd=%d, retry=%d)",
					*fd, retries);
				close(*fd);
				*fd = -1;
			}
		} else {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM connect failed, fd invalid (retry=%d)",
				retries);
		}
	}
}

static void tsm_process_send_state(int *fd, CLIENT **clnt, sockaddr_t sockaddr,
				   tsm_rpc_states *tsm_state)
{
	int retries = 0;

retry:
	if ((*fd >= TSM_VALID_FD) && (*clnt != NULL)) {
		if (!tsm_send_rpc_state(*clnt, tsm_state)) {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM state send failed, resetting client (fd=%d, clnt=%p, retry=%d)",
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
				LogCrit(COMPONENT_QOS,
					"CQOS: TSM state send failed after client create (fd=%d, clnt=%p, retry=%d)",
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
					LogCrit(COMPONENT_QOS,
						"CQOS: TSM state send failed after connect (fd=%d, clnt=%p, retry=%d)",
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
				LogCrit(COMPONENT_QOS,
					"CQOS: TSM client create failed after connect (fd=%d, retry=%d)",
					*fd, retries);
				close(*fd);
				*fd = -1;
			}
		} else {
			LogCrit(COMPONENT_QOS,
				"CQOS: TSM connect failed, fd invalid (retry=%d)",
				retries);
		}
	}
}

void tsm_send_msg(tsm_rpc_info *tsm_rpc_msg)
{
	tsm_ceph_nodes_t *node;
	tsm_rpc_info *arg = tsm_rpc_msg;

	LogFullDebug(COMPONENT_STATE,
		     "TSM: Sending Msg Type=%d (Rec=%d), FileID=%lu",
		     arg->msg_type, arg->rec_type, arg->fileid);

	if (arg->rec_type == 1) {
		printf("\n\nSending TSM open packet\n"
		       " fsid maj:%lu\n"
		       " fsid min:%lu\n"
		       " fileid:%lu\n"
		       " open sa:%d\n"
		       " open sd:%d\n\n",
		       arg->fsid_maj, arg->fsid_min, arg->fileid,
		       arg->open_info.share_access, arg->open_info.share_deny);

	} else if (arg->rec_type == TSM_DELEG_REC) {
		/* Delegation record logging */
		LogFullDebug(
			COMPONENT_STATE,
			"Sending TSM DELEG packet | fsid maj:%lu fsid min:%lu "
			"fileid:%lu type:%d access:%d deny:%d",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny);

	} else {
		LogFullDebug(
			COMPONENT_STATE,
			"Sending TSM LOCK packet | fsid maj:%lu fsid min:%lu "
			"fileid:%lu lock start:%lu lock length:%lu lock type:%d",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type);
	}

	if (!glist_empty(&tsm_hosts)) {
		struct glist_head *glist;

		glist_for_each(glist, &tsm_hosts) {
			node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return;

			if (node->is_my_ip != 1) {
				tsm_process_send_msg(&node->fd, &node->clnt,
						     node->node_addr,
						     *tsm_rpc_msg);
			}
		}
	}
}

static void tsm_store_node_state(tsm_rpc_info *msg,
				 struct glist_head *node_state)
{
	state_info_t *state;
	state_info_t *state1;
	bool file_rec_found = 0;
	bool open_rec_found = 0;
	bool lock_rec_found = 0;
	bool deleg_rec_found = 0;

	if (!glist_empty(node_state)) {
		struct glist_head *glist;

		glist_for_each(glist, node_state) {
			state = glist_entry(glist, state_info_t, states_list);
			if (state->fsid_maj == msg->fsid_maj &&
			    state->fsid_min == msg->fsid_min &&
			    state->fileid == msg->fileid &&
			    state->client_id == msg->client_id) {
				file_rec_found = 1;
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
		state1->client_id = msg->client_id;
		glist_add_tail(node_state, &state1->states_list);
		state = state1;
	}

	switch (msg->rec_type) {
	case TSM_OPEN_REC:
		if (!glist_empty(&state->open_info)) {
			struct glist_head *oplist;

			glist_for_each(oplist, &state->open_info) {
				struct state_open_ll *open_ptr;
				open_ptr = glist_entry(oplist, state_open_ll_t,
						       open_list);
				if (open_ptr->tsm_state_open.share_access ==
					    msg->open_info.share_access &&
				    open_ptr->tsm_state_open.share_deny ==
					    msg->open_info.share_deny)
					open_rec_found = 1;
			}
		}
		if (glist_empty(&state->open_info) == true ||
		    open_rec_found == 0) {
			struct state_open_ll *open_state;
			open_state =
				gsh_calloc(1, sizeof(struct state_open_ll));
			glist_init(&open_state->open_list);
			open_state->tsm_state_open.share_access =
				msg->open_info.share_access;
			open_state->tsm_state_open.share_deny =
				msg->open_info.share_deny;
			if (msg->open_info.owner[0] != '\0')
				strncpy(open_state->owner, msg->open_info.owner,
					sizeof(open_state->owner) - 1);
			glist_add_tail(&state->open_info,
				       &open_state->open_list);
			LogFullDebug(
				COMPONENT_STATE,
				"TSM Store: Added OPEN Record: FileID=%lu, Access=%d, Deny=%d, Owner=%s",
				state->fileid,
				open_state->tsm_state_open.share_access,
				open_state->tsm_state_open.share_deny,
				open_state->owner);
		}
		break;
	case TSM_DELEG_REC:
		if (!glist_empty(&state->deleg_info)) {
			struct glist_head *deleglist;

			glist_for_each(deleglist, &state->deleg_info) {
				struct state_deleg_ll *deleg_ptr;
				deleg_ptr = glist_entry(deleglist,
							state_deleg_ll_t,
							deleg_list);
				if (deleg_ptr->tsm_state_deleg.sd_type ==
					    msg->deleg_info.sd_type &&
				    deleg_ptr->tsm_state_deleg.share_access ==
					    msg->deleg_info.share_access)
					deleg_rec_found = 1;
			}
		}
		if (glist_empty(&state->deleg_info) == true ||
		    deleg_rec_found == 0) {
			struct state_deleg_ll *deleg_state;
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
			if (msg->deleg_info.owner[0] != '\0')
				strncpy(deleg_state->owner,
					msg->deleg_info.owner,
					sizeof(deleg_state->owner) - 1);
			glist_add_tail(&state->deleg_info,
				       &deleg_state->deleg_list);
			LogFullDebug(
				COMPONENT_STATE,
				"TSM Store: Added DELEG Record: FileID=%lu, Type=%d, Access=%d, Deny=%d, Owner=%s",
				state->fileid,
				deleg_state->tsm_state_deleg.sd_type,
				deleg_state->tsm_state_deleg.share_access,
				deleg_state->tsm_state_deleg.share_deny,
				deleg_state->owner);
		}
		break;
	case TSM_LOCK_REC:
		if (!glist_empty(&state->lock_info)) {
			struct glist_head *locklist;

			glist_for_each(locklist, &state->lock_info) {
				struct state_lock_ll *lock_ptr;
				lock_ptr = glist_entry(locklist,
						       state_lock_ll_t,
						       lock_list);
				if (lock_ptr->tsm_state_lock.type ==
					    msg->lock_info.type &&
				    lock_ptr->tsm_state_lock.start ==
					    msg->lock_info.start &&
				    lock_ptr->tsm_state_lock.length ==
					    msg->lock_info.length)
					lock_rec_found = 1;
			}
		}
		if (glist_empty(&state->lock_info) == true ||
		    lock_rec_found == 0) {
			struct state_lock_ll *lock_state;
			lock_state =
				gsh_calloc(1, sizeof(struct state_lock_ll));
			glist_init(&lock_state->lock_list);
			lock_state->tsm_state_lock.type = msg->lock_info.type;
			lock_state->tsm_state_lock.start = msg->lock_info.start;
			lock_state->tsm_state_lock.length =
				msg->lock_info.length;
			if (msg->lock_info.owner[0] != '\0')
				strncpy(lock_state->owner, msg->lock_info.owner,
					sizeof(lock_state->owner) - 1);
			glist_add_tail(&state->lock_info,
				       &lock_state->lock_list);
			LogFullDebug(
				COMPONENT_STATE,
				"TSM Store: Added LOCK Record: FileID=%lu, Type=%d, Start=%lu, Len=%lu, Owner=%s",
				state->fileid, lock_state->tsm_state_lock.type,
				lock_state->tsm_state_lock.start,
				lock_state->tsm_state_lock.length,
				lock_state->owner);
		}
		break;
	default:
		break;
	}
}

/*
 * Deletes the particular state info based on msg record type
 */
void tsm_delete_node_state(tsm_rpc_info *msg, struct glist_head *node_state)
{
	state_info_t *state = NULL;
	bool found = false;

	if (!glist_empty(node_state)) {
		struct glist_head *glist;

		glist_for_each(glist, node_state) {
			state = glist_entry(glist, state_info_t, states_list);
			if (state->fsid_maj == msg->fsid_maj &&
			    state->fsid_min == msg->fsid_min &&
			    state->fileid == msg->fileid &&
			    state->client_id == msg->client_id) {
				found = true;
				break;
			}
		}
	}

	if (!found || !state)
		return;

	switch (msg->rec_type) {
	case TSM_OPEN_REC:
		if (!glist_empty(&state->open_info)) {
			struct glist_head *oplist, *tmp;

			glist_for_each_safe(oplist, tmp, &state->open_info) {
				struct state_open_ll *open_ptr;
				open_ptr = glist_entry(oplist, state_open_ll_t,
						       open_list);
				if (open_ptr->tsm_state_open.share_access ==
					    msg->open_info.share_access &&
				    open_ptr->tsm_state_open.share_deny ==
					    msg->open_info.share_deny) {
					glist_del(oplist);
					gsh_free(open_ptr);
					LogFullDebug(
						COMPONENT_STATE,
						"TSM Delete: Removed OPEN Record: FileID=%lu, Access=%d, Deny=%d",
						state->fileid,
						msg->open_info.share_access,
						msg->open_info.share_deny);
				}
			}
		}
		break;

	case TSM_LOCK_REC:
		if (!glist_empty(&state->lock_info)) {
			struct glist_head *locklist, *tmp;

			glist_for_each_safe(locklist, tmp, &state->lock_info) {
				struct state_lock_ll *lock_ptr;
				lock_ptr = glist_entry(locklist,
						       state_lock_ll_t,
						       lock_list);
				if (lock_ptr->tsm_state_lock.type ==
					    msg->lock_info.type &&
				    lock_ptr->tsm_state_lock.start ==
					    msg->lock_info.start &&
				    lock_ptr->tsm_state_lock.length ==
					    msg->lock_info.length) {
					glist_del(locklist);
					gsh_free(lock_ptr);
					LogFullDebug(
						COMPONENT_STATE,
						"TSM Delete: Removed LOCK Record: FileID=%lu, Type=%d, Start=%lu, Len=%lu",
						state->fileid,
						msg->lock_info.type,
						msg->lock_info.start,
						msg->lock_info.length);
				}
			}
		}
		break;

	case TSM_DELEG_REC:
		if (!glist_empty(&state->deleg_info)) {
			struct glist_head *deleglist, *tmp;

			glist_for_each_safe(deleglist, tmp,
					    &state->deleg_info) {
				struct state_deleg_ll *deleg_ptr;
				deleg_ptr = glist_entry(deleglist,
							state_deleg_ll_t,
							deleg_list);
				if (deleg_ptr->tsm_state_deleg.sd_type ==
					    msg->deleg_info.sd_type &&
				    deleg_ptr->tsm_state_deleg.share_access ==
					    msg->deleg_info.share_access) {
					glist_del(deleglist);
					gsh_free(deleg_ptr);
					LogFullDebug(
						COMPONENT_STATE,
						"TSM Delete: Removed DELEG Record: FileID=%lu, Type=%d",
						state->fileid,
						msg->deleg_info.sd_type);
				}
			}
		}
		break;
	case TSM_LAYOUT_REC:
		break;
	}
}

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

void tsm_reply_node_state(tsm_ceph_nodes_t *node)
{
	tsm_rpc_states rpc_state;
	tsm_rpc_info *rpc_msg;
	state_info_t *state;
	int total_records = 0;

	memset(&rpc_state, 0, sizeof(rpc_state));

	if (!glist_empty(&node->state_info)) {
		struct glist_head *glist;

		glist_for_each(glist, &node->state_info) {
			state = glist_entry(glist, state_info_t, states_list);
			if (state == NULL)
				goto send_state_records;

			/* OPEN records */
			if (!glist_empty(&state->open_info)) {
				struct glist_head *oplist;

				glist_for_each(oplist, &state->open_info) {
					total_records++;
					rpc_state.tsmrec_len++;

					if (total_records == 1) {
						rpc_state
							.tsmarray_val = gsh_calloc(
							1,
							sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val;
					} else {
						rpc_state
							.tsmarray_val = gsh_realloc(
							rpc_state.tsmarray_val,
							total_records *
								sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val +
							(total_records - 1);
					}

					rpc_msg->fsid_maj = state->fsid_maj;
					rpc_msg->fsid_min = state->fsid_min;
					rpc_msg->fileid = state->fileid;
					rpc_msg->client_id = state->client_id;

					rpc_msg->msg_type = TSM_REPLY_STATE;
					rpc_msg->rec_type = TSM_OPEN_REC;

					struct state_open_ll *open_ptr;
					open_ptr = glist_entry(oplist,
							       state_open_ll_t,
							       open_list);

					rpc_msg->open_info.share_access =
						open_ptr->tsm_state_open
							.share_access;
					rpc_msg->open_info.share_deny =
						open_ptr->tsm_state_open
							.share_deny;

					if (open_ptr->owner[0] != '\0') {
						strncpy(rpc_msg->open_info.owner,
							open_ptr->owner,
							sizeof(rpc_msg->open_info
								       .owner) -
								1);
					}
				}
			}

			/* DELEG records */
			if (!glist_empty(&state->deleg_info)) {
				struct glist_head *oplist;

				glist_for_each(oplist, &state->deleg_info) {
					total_records++;
					rpc_state.tsmrec_len++;

					if (total_records == 1) {
						rpc_state
							.tsmarray_val = gsh_calloc(
							1,
							sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val;
					} else {
						rpc_state
							.tsmarray_val = gsh_realloc(
							rpc_state.tsmarray_val,
							total_records *
								sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val +
							(total_records - 1);
					}

					rpc_msg->fsid_maj = state->fsid_maj;
					rpc_msg->fsid_min = state->fsid_min;
					rpc_msg->fileid = state->fileid;
					rpc_msg->client_id = state->client_id;

					rpc_msg->msg_type = TSM_REPLY_STATE;
					rpc_msg->rec_type = TSM_DELEG_REC;

					struct state_deleg_ll *deleg_ptr;
					deleg_ptr =
						glist_entry(oplist,
							    state_deleg_ll_t,
							    deleg_list);

					rpc_msg->deleg_info.sd_type =
						deleg_ptr->tsm_state_deleg
							.sd_type;
					rpc_msg->deleg_info.sd_state =
						deleg_ptr->tsm_state_deleg
							.sd_state;
					rpc_msg->deleg_info.share_access =
						deleg_ptr->tsm_state_deleg
							.share_access;
					rpc_msg->deleg_info.share_deny =
						deleg_ptr->tsm_state_deleg
							.share_deny;

					if (deleg_ptr->owner[0] != '\0') {
						strncpy(rpc_msg->deleg_info
								.owner,
							deleg_ptr->owner,
							sizeof(rpc_msg->deleg_info
								       .owner) -
								1);
					}
				}
			}

			/* LOCK records */
			if (!glist_empty(&state->lock_info)) {
				struct glist_head *oplist;

				glist_for_each(oplist, &state->lock_info) {
					total_records++;
					rpc_state.tsmrec_len++;

					if (total_records == 1) {
						rpc_state
							.tsmarray_val = gsh_calloc(
							1,
							sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val;
					} else {
						rpc_state
							.tsmarray_val = gsh_realloc(
							rpc_state.tsmarray_val,
							total_records *
								sizeof(tsm_rpc_info));
						rpc_msg =
							rpc_state.tsmarray_val +
							(total_records - 1);
					}

					rpc_msg->fsid_maj = state->fsid_maj;
					rpc_msg->fsid_min = state->fsid_min;
					rpc_msg->fileid = state->fileid;
					rpc_msg->client_id = state->client_id;

					rpc_msg->msg_type = TSM_REPLY_STATE;
					rpc_msg->rec_type = TSM_LOCK_REC;

					struct state_lock_ll *lock_ptr;
					lock_ptr = glist_entry(oplist,
							       state_lock_ll_t,
							       lock_list);

					rpc_msg->lock_info.type =
						lock_ptr->tsm_state_lock.type;
					rpc_msg->lock_info.start =
						lock_ptr->tsm_state_lock.start;
					rpc_msg->lock_info.length =
						lock_ptr->tsm_state_lock.length;

					if (lock_ptr->owner[0] != '\0') {
						strncpy(rpc_msg->lock_info.owner,
							lock_ptr->owner,
							sizeof(rpc_msg->lock_info
								       .owner) -
								1);
					}
				}
			}
		}
	}

send_state_records:
	if (total_records != 0) {
		LogFullDebug(
			COMPONENT_STATE,
			"TSM: Replying with Bulk State: TotalRecords=%d to Peer",
			total_records);

		tsm_process_send_state(&node->fd, &node->clnt, node->node_addr,
				       &rpc_state);

		gsh_free(rpc_state.tsmarray_val);
	} else {
		LogFullDebug(
			COMPONENT_STATE,
			"TSM: Replying with Empty State (No records found)");
	}
}

pthread_t tsm_thread;

void tsm_process_recd_msg(tsm_rpc_info *msg)
{
	tsm_ceph_nodes_t *node;

	LogFullDebug(COMPONENT_STATE, "TSM msg received: msg_type=%d",
		     msg->msg_type);

	if (glist_empty(&tsm_hosts)) {
		LogFullDebug(COMPONENT_STATE,
			     "TSM host list is empty, ignoring message");
		return;
	}

	struct glist_head *glist;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);

		if (node == NULL) {
			LogFullDebug(COMPONENT_STATE,
				     "Encountered NULL node in tsm_hosts list");
			return;
		}

		LogFullDebug(COMPONENT_STATE,
			     "Checking TSM node against source address");

		if (!sockaddr_cmpf(&node->node_addr, &msg->source_addr, true)) {
			LogFullDebug(COMPONENT_STATE,
				     "Matched TSM node for source address");

			switch (msg->msg_type) {
			case TSM_SET_STATE:
				LogFullDebug(COMPONENT_STATE,
					     "Processing TSM_SET_STATE");
				tsm_store_node_state(msg, &node->state_info);
				return;

			case TSM_GET_STATE:
				LogFullDebug(
					COMPONENT_STATE,
					"Processing TSM_GET_STATE: marking state requested");
				node->is_state_requested = true;
				return;

			case TSM_DELETE_STATE:
				LogFullDebug(COMPONENT_STATE,
					     "Processing TSM_DELETE_STATE");
				tsm_delete_node_state(msg, &node->state_info);
				return;

			case TSM_PEER_IP_NOTIFY:
				LogFullDebug(COMPONENT_STATE,
					     "Processing TSM_PEER_IP_NOTIFY");
				return;

			case TSM_RECLAIM_EXPORT_IDS:
				LogFullDebug(
					COMPONENT_STATE,
					"Processing TSM_RECLAIM_EXPORT_IDS");
				return;

			default:
				LogFullDebug(COMPONENT_STATE,
					     "Unknown TSM msg_type=%d",
					     msg->msg_type);
				return;
			}
		}
	}

	LogFullDebug(COMPONENT_STATE,
		     "No matching TSM node found for source address");
}

void tsm_process_recd_states(tsm_rpc_states *msg)
{
	tsm_ceph_nodes_t *node;

	if (!glist_empty(&tsm_hosts)) {
		struct glist_head *glist;

		glist_for_each(glist, &tsm_hosts) {
			node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
			if (node == NULL)
				return;
			if (node->is_my_ip == true) {
				tsm_store_prev_state(msg, &node->state_info);
				return;
			}
		}
	}
}

static void *tsm_thread_func(void *arg)
{
	tsm_ceph_nodes_t *node;
	while (true) {
		if (!glist_empty(&tsm_hosts)) {
			struct glist_head *glist;

			glist_for_each(glist, &tsm_hosts) {
				node = glist_entry(glist, tsm_ceph_nodes_t,
						   node_list);
				if (node == NULL)
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

pthread_t tsm_thread;

static void tsm_thread_init(void)
{
	int ret = 0;

	if (tsm_initialized == 0) {
		ret = pthread_create(&tsm_thread, NULL, tsm_thread_func, NULL);
		if (ret != 0) {
			LogFatal(COMPONENT_QOS,
				 "CQOS: Thread creation failed error %d (%s)",
				 errno, strerror(errno));
		}

		pthread_detach(tsm_thread);
	}
	tsm_initialized = 1;
	LogDebug(COMPONENT_QOS, "CQOS: Cluster QOS thread is initialized");
}

void tsm_init(void)
{
	if (tsm_initialized == 0) {
		LogDebug(COMPONENT_QOS, "Cluster QOS thread_init");
		tsm_thread_init();
	}
}

bool tsm_is_conflicting_open(struct fsal_obj_handle *obj, clientid4 *clientid,
			     uint32_t share_access, uint32_t share_deny,
			     open_claim_type4 claim, char *owner)
{
	struct glist_head *glist;
	struct glist_head *state_list_head;
	struct glist_head *open_list_head;

	tsm_ceph_nodes_t *node;
	state_info_t *state;
	state_open_ll_t *open_rec;

	if (!obj)
		return false;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
		if (!node)
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

			/* Check OPEN vs OPEN conflicts */
			glist_for_each(open_list_head, &state->open_info) {
				open_rec = glist_entry(open_list_head,
						       state_open_ll_t,
						       open_list);

				/* Same owner → no conflict */
				if (owner && open_rec->owner &&
				    !strcmp(open_rec->owner, owner)) {
					LogFullDebug(
						COMPONENT_STATE,
						"TSM: Allowing OPEN (Owner Match) for Owner=%s",
						owner);
					continue;
				}

				if ((share_access &
				     open_rec->tsm_state_open.share_deny) ||
				    (share_deny &
				     open_rec->tsm_state_open.share_access)) {
					LogFullDebug(
						COMPONENT_STATE,
						"TSM Conflict OPEN Detected! Obj=%lu, "
						"Access=%d, Deny=%d vs RecAccess=%d, RecDeny=%d",
						obj->fileid, share_access,
						share_deny,
						open_rec->tsm_state_open
							.share_access,
						open_rec->tsm_state_open
							.share_deny);
					return true;
				}
			}

			/* Check OPEN vs DELEG conflicts */
			if (!glist_empty(&state->deleg_info)) {
				struct glist_head *deleg_list_head;
				state_deleg_ll_t *deleg_rec;

				glist_for_each(deleg_list_head,
					       &state->deleg_info) {
					deleg_rec =
						glist_entry(deleg_list_head,
							    state_deleg_ll_t,
							    deleg_list);

					/* Same owner → no conflict */
					if (owner && deleg_rec->owner &&
					    !strcmp(deleg_rec->owner, owner)) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM: Allowing OPEN vs DELEG (Owner Match) for Owner=%s",
							owner);
						continue;
					}

					/* Write delegation conflicts with any OPEN */
					if (deleg_rec->tsm_state_deleg.sd_type ==
					    OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict OPEN vs DELEG (Write) Detected! Obj=%lu",
							obj->fileid);
						return true;
					}

					/* Read delegation conflicts with write access or deny-read */
					if (deleg_rec->tsm_state_deleg.sd_type ==
					    OPEN_DELEGATE_READ) {
						if ((share_access &
						     OPEN4_SHARE_ACCESS_WRITE) ||
						    (share_deny &
						     OPEN4_SHARE_ACCESS_READ)) {
							LogFullDebug(
								COMPONENT_STATE,
								"TSM Conflict OPEN vs DELEG (Read) Detected! Obj=%lu",
								obj->fileid);
							return true;
						}
					}
				}
			}
		}
	}

	return false;
}

bool tsm_is_conflicting_lock(struct fsal_obj_handle *obj, clientid4 *clientid,
			     uint64_t start, uint64_t length,
			     nfs_lock_type4 type, char *owner)
{
	struct glist_head *glist;
	struct glist_head *state_list_head;
	struct glist_head *lock_list_head;

	tsm_ceph_nodes_t *node;
	state_info_t *state;
	state_lock_ll_t *lock_rec;

	uint64_t end = start + length - 1;

	if (!obj)
		return false;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
		if (!node)
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

			glist_for_each(lock_list_head, &state->lock_info) {
				lock_rec = glist_entry(lock_list_head,
						       state_lock_ll_t,
						       lock_list);

				uint64_t rec_start =
					lock_rec->tsm_state_lock.start;
				uint64_t rec_len =
					lock_rec->tsm_state_lock.length;
				uint64_t rec_end = rec_start + rec_len - 1;
				uint32_t rec_type =
					lock_rec->tsm_state_lock.type;

				/* -------- Owner matching -------- */
				bool same_owner = false;

				/*
				 * ClientID match alone is not sufficient in NFSv4
				 * (different lock owners from same client can conflict),
				 * so we primarily rely on owner string match.
				 */
				if (owner && lock_rec->owner &&
				    !strcmp(lock_rec->owner, owner))
					same_owner = true;

				/* -------- Range overlap check -------- */
				if (rec_end < start || rec_start > end)
					continue;

				if (same_owner) {
					LogFullDebug(
						COMPONENT_STATE,
						"TSM: Allowing LOCK (Owner Match) for Owner=%s",
						owner ? owner : "NULL");
					continue;
				}

				bool is_write_req =
					(type == WRITE_LT || type == WRITEW_LT);
				bool is_write_rec = (rec_type == WRITE_LT ||
						     rec_type == WRITEW_LT);

				/*
				 * Any overlapping region where either side
				 * is a WRITE lock is a conflict.
				 */
				if (is_write_req || is_write_rec) {
					LogFullDebug(
						COMPONENT_STATE,
						"TSM Conflict LOCK Detected! Obj=%lu, "
						"Req=[%lu, %lu) Type=%d vs "
						"Rec=[%lu, %lu) Type=%d",
						obj->fileid, start, end + 1,
						type, rec_start, rec_end + 1,
						rec_type);
					return true;
				}
			}
		}
	}

	return false;
}

bool tsm_is_conflicting_deleg(struct fsal_obj_handle *obj, clientid4 *clientid,
			      open_delegation_type4 type,
			      open_claim_type4 claim, char *owner)
{
	struct glist_head *glist;
	struct glist_head *state_list_head;

	tsm_ceph_nodes_t *node;
	state_info_t *state;
	state_deleg_ll_t *deleg_rec;

	/* Iterate over TSM hosts list and check for conflicting state */
	if (glist_empty(&tsm_hosts))
		return false;

	glist_for_each(glist, &tsm_hosts) {
		node = glist_entry(glist, tsm_ceph_nodes_t, node_list);
		if (!node)
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

			/* ---------- Check vs Existing OPEN records ---------- */
			if (!glist_empty(&state->open_info)) {
				struct glist_head *open_list_head;
				state_open_ll_t *open_rec;

				glist_for_each(open_list_head,
					       &state->open_info) {
					open_rec = glist_entry(open_list_head,
							       state_open_ll_t,
							       open_list);

					bool same_owner = false;

					if (clientid && owner &&
					    open_rec->owner) {
						if (claim == CLAIM_PREVIOUS) {
							same_owner = (!strcmp(
								open_rec->owner,
								owner));
						} else {
							same_owner =
								(state->client_id ==
									 *clientid &&
								 !strcmp(open_rec->owner,
									 owner));
						}
					}

					/* Same owner + same clientid → no conflict */
					if (same_owner)
						continue;

					if (claim == CLAIM_PREVIOUS) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict (CLAIM_PREVIOUS) vs Existing Open! Obj=%lu ReqOwner=%s ReqClientID=%llu OpenOwner=%s OpenClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(open_rec->owner[0] !=
							 '\0')
								? open_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}

					/* Different owner: allow READ deleg request */
					if (type == OPEN_DELEGATE_READ) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM: Allowing READ Deleg (Different Owner) Obj=%lu ReqOwner=%s ReqClientID=%llu OpenOwner=%s OpenClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(open_rec->owner[0] !=
							 '\0')
								? open_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						continue;
					}

					/* Requesting WRITE delegation conflicts with any OPEN */
					if (type == OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict (Req Write Deleg) vs Existing Open! Obj=%lu ReqOwner=%s ReqClientID=%llu OpenOwner=%s OpenClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(open_rec->owner[0] !=
							 '\0')
								? open_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}

					/* Requesting READ delegation conflicts with WRITE OPEN */
					if (type == OPEN_DELEGATE_READ &&
					    (open_rec->tsm_state_open
						     .share_access &
					     OPEN4_SHARE_ACCESS_WRITE)) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict (Req Read Deleg) vs Existing Write Open! Obj=%lu ReqOwner=%s ReqClientID=%llu OpenOwner=%s OpenClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(open_rec->owner[0] !=
							 '\0')
								? open_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}
				}
			}

			/* ---------- Check vs Existing DELEG records ---------- */
			if (!glist_empty(&state->deleg_info)) {
				struct glist_head *deleg_list_head;

				glist_for_each(deleg_list_head,
					       &state->deleg_info) {
					deleg_rec =
						glist_entry(deleg_list_head,
							    state_deleg_ll_t,
							    deleg_list);

					bool same_owner = false;

					if (clientid && owner &&
					    deleg_rec->owner) {
						if (claim == CLAIM_PREVIOUS) {
							same_owner = (!strcmp(
								deleg_rec->owner,
								owner));
						} else {
							same_owner =
								(state->client_id ==
									 *clientid &&
								 !strcmp(deleg_rec
										 ->owner,
									 owner));
						}
					}

					/* Same owner + same clientid → no conflict */
					if (same_owner) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM: Allowing DELEG (Owner Match) ReqOwner=%s RecOwner=%s",
							owner ? owner : "NULL",
							(deleg_rec->owner[0] !=
							 '\0')
								? deleg_rec->owner
								: "EMPTY");

						continue;
					}

					/* Different owner cases */

					if (claim == CLAIM_PREVIOUS) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict (CLAIM_PREVIOUS) vs Existing Deleg! Obj=%lu ReqOwner=%s ReqClientID=%llu DelegOwner=%s DelegClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(deleg_rec->owner[0] !=
							 '\0')
								? deleg_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}

					/* Different owner: allow READ deleg request */
					if (type == OPEN_DELEGATE_READ) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM: Allowing READ Deleg (Different Owner) Obj=%lu ReqOwner=%s ReqClientID=%llu DelegOwner=%s DelegClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(deleg_rec->owner[0] !=
							 '\0')
								? deleg_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						continue;
					}

					/* WRITE deleg request conflicts with any existing deleg */
					if (type == OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict DELEG (Req Write) vs Existing Deleg! Obj=%lu ReqOwner=%s ReqClientID=%llu DelegOwner=%s DelegClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(deleg_rec->owner[0] !=
							 '\0')
								? deleg_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}

					/* READ deleg request conflicts with WRITE deleg */
					if (type == OPEN_DELEGATE_READ &&
					    deleg_rec->tsm_state_deleg.sd_type ==
						    OPEN_DELEGATE_WRITE) {
						LogFullDebug(
							COMPONENT_STATE,
							"TSM Conflict DELEG (Req Read) vs Write Deleg! Obj=%lu ReqOwner=%s ReqClientID=%llu DelegOwner=%s DelegClientID=%llu",
							obj->fileid,
							owner ? owner : "NULL",
							(unsigned long long)*clientid,
							(deleg_rec->owner[0] !=
							 '\0')
								? deleg_rec->owner
								: "EMPTY",
							(unsigned long long)state
								->client_id);
						return true;
					}
				}
			}
		}
	}

	return false;
}
