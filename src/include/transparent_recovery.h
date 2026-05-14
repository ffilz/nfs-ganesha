/*
 * =====================================================================================
 *
 *       Filename:  tsm.h
 *
 *    Description:
 *
 *        Version:  1.0
 *        Created:  12/10/2025 01:45:02 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (),
 *   Organization:
 *
 * =====================================================================================
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/if_link.h>
#include <time.h>
#include "tsm.h"
#include "nfs_core.h"

#define TSM_MAX_RETRIES 2
#define TSM_VALID_FD 0
#define TSM_ACK_TIMEOUT_MS 500
#define TSM_ACK_MAX_RETRIES 3
#define TSM_MAX_RECOVERY_RETRIES 5

extern struct glist_head export_ids;
extern struct glist_head tsm_hosts;
extern unsigned int tsm_initialized;
extern struct config_block tsm_core;
extern sockaddr_t tsm_my_addr;
extern bool tsm_disabled_source;

typedef struct state_export_ll {
	struct tsm_export_info tsm_export_info;
	struct glist_head export_list;
} state_export_ll_t;

typedef struct state_open_ll {
	struct tsm_state_open tsm_state_open;
	struct glist_head open_list;
} state_open_ll_t;

typedef struct state_lock_ll {
	struct tsm_state_lock tsm_state_lock;
	struct glist_head lock_list;
} state_lock_ll_t;

typedef struct state_deleg_ll {
	struct tsm_state_deleg tsm_state_deleg;
	struct glist_head deleg_list;
} state_deleg_ll_t;

struct tsm_state_layout {
	struct glist_head state_segments; /*< List of segments */
	layouttype4 state_layout_type; /*< The type of layout this state
					 represents */
	uint32_t granting; /*< Number of LAYOUTGETs in progress */
	bool state_return_on_close; /*< Whether this layout should be
				      returned on last close. */
};

typedef struct state_layout_ll {
	struct tsm_state_layout tsm_state_layout;
	struct glist_head layout_list;
} state_layout_ll_t;

typedef struct state_info {
	struct glist_head states_list;
	uint64_t fsid_maj;
	uint64_t fsid_min;
	uint64_t fileid;
	uint64_t client_id;
	uint16_t open_count;
	struct glist_head open_info;
	uint16_t lock_count;
	struct glist_head lock_info;
	uint16_t deleg_count;
	struct glist_head deleg_info;
	uint16_t layout_count;
	struct glist_head layout_info;
} state_info_t;

typedef struct tsm_recovery_ctx {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool peer_responded;
	bool_t has_primary;
	bool_t has_secondary;
	struct timespec start_time;
	int timeout_sec;
	uint32_t peer_recovery_state;
} tsm_recovery_ctx_t;

typedef struct tsm_ceph_nodes {
	struct glist_head node_list;
	sockaddr_t node_addr;
	sockaddr_t primary_node_addr;
	sockaddr_t secondary_node_addr;
	tsm_recovery_ctx_t recovery;
	int32_t fd;
	CLIENT *clnt;
	bool is_my_ip;
	bool is_state_requested;
	bool is_peer_rec_requested;
	uint16_t ganesha_id;
	struct glist_head state_info;
	struct glist_head pending_acks;
	bool tsm_suspect;
} tsm_ceph_nodes_t;

/* Pending ACK entry: one per (sender node, msg_id) */
struct tsm_pending_ack {
	uint64_t msg_id;
	tsm_rpc_info msg;
	struct timespec last_send_ts;
	int retries;
	struct glist_head list;
};

extern pthread_mutex_t tsm_pending_mutex;
extern uint64_t tsm_msg_id_counter;

enum TSM_PEER_RECORD_RECOVERY_STATE {
	TSM_PEER_RECORD_RECOVERY_INIT,
	TSM_PEER_RECORD_RECOVERY_REQUEST_SENT,
	TSM_PEER_RECORD_RECOVERY_DONE,
	TSM_PEER_RECORD_FIRST_BOOT_DONE,
	TSM_PEER_RECORD_RECOVERY_FAILED
};

enum TSM_MSG_TYPE {
	/*  Once peer node for this node selected, notify peer IP to all nodes. */
	TSM_PEER_IP_NOTIFY = 1,
	/*  New node fetches state info of  old node state info using this msg. */
	TSM_GET_STATE,
	/*  Inform all nodes list of export ids of which reclaim is in progress. */
	TSM_RECLAIM_EXPORT_IDS,
	/*  Set a particular record message */
	TSM_SET_STATE,
	/* Message to send all state info of a node */
	TSM_REPLY_STATE,
	/* Message to send export list  of a node */
	TSM_REPLY_EXPORT_LIST,
	/* Delete State from peer node */
	TSM_DELETE_STATE,
	/* Store Peer nodes' info on a node */
	TSM_SET_PEER_NODE_INFO,
	/* Request Peer nodes' info */
	TSM_GET_PEER_NODE_INFO,
	/* Heartbeat check */
	TSM_PEER_PING,
	/* Disable tsm */
	TSM_DISABLE_NOTIFY,
	/* Enable tsm */
	TSM_ENABLE_NOTIFY,
	/* ACK that state update was applied on receiver */
	TSM_ACK_STATE,
	/*Broadcast export id of an Operation*/
	TSM_EXPORT_ID_NOTIFY,
};

enum TSM_REC_TYPE {
	/* Send open state info of a file */
	TSM_OPEN_REC = 1,
	/* Send lock state info of user request */
	TSM_LOCK_REC,
	/* Send read/write delegation info */
	TSM_DELEG_REC,
	/* Send Layout info of file */
	TSM_LAYOUT_REC,
};

bool is_self_ip(const char *in_addr);
void tsm_disable_export(uint16_t curr_exp);
void tsm_broadcast_export_list(uint16_t curr_exp, tsm_export_event_t exp_ev,
			       bool tsm_enabled);
void tsm_handle_peer_node_selection(tsm_ceph_nodes_t *my_node,
				    tsm_ceph_nodes_t **node_array,
				    int cluster_size, int my_index);
void tsm_request_state_from_peer(void);
int tsm_build_node_array(tsm_ceph_nodes_t **node_array,
			 tsm_ceph_nodes_t **my_node, int *my_index);
bool tsm_recover_from_peers(tsm_ceph_nodes_t *my_node,
			    tsm_ceph_nodes_t **node_array, int cluster_size,
			    int my_index);
bool tsm_wait_for_recovery(tsm_ceph_nodes_t *node);
void tsm_delete_node_state(tsm_rpc_info *msg, struct glist_head *node_state);
tsm_ceph_nodes_t *tsm_find_node_by_addr(sockaddr_t *addr);
void tsm_store_export_state(uint16_t export_id, uint16_t export_state,
			    bool tsm_enabled);
bool tsm_exp_list_init(struct gsh_export *exp, void *arg);
void tsm_send_msg_with_ack(tsm_rpc_info *tsm_rpc_msg);
bool tsm_is_conflicting_deleg(struct fsal_obj_handle *obj, clientid4 *clientid,
			      open_delegation_type4 type,
			      open_claim_type4 claim, char *owner);
void tsm_delete_all_records_all_nodes(void);
const char *tsm_msg_type_to_str(int type);
bool tsm_process_send_msg(int *fd, CLIENT **clnt, sockaddr_t sockaddr,
			  tsm_rpc_info tsm_msg);
bool tsm_send_msg_peer(tsm_rpc_info *tsm_rpc_msg);
int tsm_process_send_msg_peer(tsm_rpc_info *msg, tsm_ceph_nodes_t **targets,
			      int *target_count);
void tsm_reply_node_peer_rec(tsm_ceph_nodes_t *node);
uint64_t tsm_generate_node_id(sockaddr_t *tsm_my_addr, uint64_t cluster_size);
uint32_t tsm_hash_fnv1a_32(const uint8_t *data, size_t len);
uint64_t tsm_hash_fnv1a_64(const uint8_t *data, size_t len);
uint64_t tsm_generate_node_ip_hash(sockaddr_t *input_addr);
bool tsm_is_access_valid(tsm_rpc_info *tsm_rpc_msg, bool_t reclaim);
void tsm_print_node_state(struct glist_head *node_state);
bool tsm_is_conflicting_node_state(tsm_rpc_info *msg,
				   struct glist_head *node_state,
				   bool_t reclaim);
void tsm_send_msg(tsm_rpc_info *tsm_rpc_msg);
void tsm_process_recd_msg(tsm_rpc_info *msg);
void tsm_process_recd_states(tsm_rpc_states *msg);
void tsm_init(void);
