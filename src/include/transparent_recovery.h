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
#define TSM_PORT 36369
#define TSM_MAX_RETRIES 2
#define TSM_VALID_FD 0

extern struct glist_head tsm_hosts;
extern unsigned int tsm_initialized;
extern struct config_block tsm_core;
extern sockaddr_t tsm_my_addr;

typedef struct state_open_ll {
	struct tsm_state_open tsm_state_open;
	char owner[1024];
        struct glist_head open_list;
}state_open_ll_t;


typedef struct state_lock_ll {
	struct tsm_state_lock tsm_state_lock;
	char owner[1024];
        struct glist_head lock_list;
}state_lock_ll_t;

typedef struct state_deleg_ll {
	struct tsm_state_deleg tsm_state_deleg;
	char owner[1024];
	struct glist_head deleg_list;
}state_deleg_ll_t;

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
}state_layout_ll_t;


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
}state_info_t;

typedef struct tsm_ceph_nodes {
        struct glist_head node_list;
        sockaddr_t node_addr;
	int32_t fd;
        CLIENT *clnt;
	bool is_my_ip;
	bool is_state_requested;
	uint16_t ganesha_id;
	char export_ids[1024];
	struct glist_head state_info;
}tsm_ceph_nodes_t;

enum TSM_MSG_TYPE  {
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
	/* Delete a particular record message */
	TSM_DELETE_STATE,
};

enum TSM_REC_TYPE  {
	/* Send open state info of a file */
	TSM_OPEN_REC = 1,
	/* Send lock state info of user request */
	TSM_LOCK_REC,
	/* Send read/write delegation info */
	TSM_DELEG_REC,
	/* Send Layout info of file */
	TSM_LAYOUT_REC,
};
void tsm_send_msg (tsm_rpc_info *tsm_rpc_msg);
void tsm_process_recd_msg (tsm_rpc_info *msg);
void tsm_process_recd_states (tsm_rpc_states *msg);
void tsm_init(void );
void tsm_delete_node_state(tsm_rpc_info *msg, struct glist_head *node_state);
                             
bool tsm_is_conflicting_open(struct fsal_obj_handle *obj,
                            clientid4 *clientid,
                            uint32_t share_access,
                            uint32_t share_deny,
                            open_claim_type4 claim,
			    char *owner);

bool tsm_is_conflicting_deleg(struct fsal_obj_handle *obj,
                             clientid4 *clientid,
                             open_delegation_type4 type,
                             open_claim_type4 claim,
			     char *owner);
			     
bool tsm_is_conflicting_lock(struct fsal_obj_handle *obj, clientid4 *clientid,
                             uint64_t start, uint64_t length,
                             nfs_lock_type4 type,
			     char *owner);
