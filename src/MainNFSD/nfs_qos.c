#include "config.h"
#include "log.h"
#include "fsal.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "nfs_convert.h"
#include "fsal_pnfs.h"
#include "server_stats.h"
#include "export_mgr.h"
#include "nfs_qos.h"
unsigned int qos_initalized = 0;
typedef void (*qos_svc_rcb)(void *);
static void qos_token_exausted_deffer_task(void *ptr, void *caller_data , compound_data_t *data, unsigned int class_type, unsigned int op_type);
static void qos_thread_init(void);
static void *qos_thread_func(void *arg);
static struct qos_client_entry *get_and_insert_client_details(struct qos_client_entry **head, compound_data_t *data);
static timer_entry_t *create_timer_entry(uint64_t expiry, void (*callback)(void *), void *args);
static void insert_timer_entry(timer_entry_t **head, timer_entry_t *new_entry);
static void remove_timer_entry(timer_entry_t **head, timer_entry_t *entry_to_remove);
void list_timer_entries(timer_entry_t *current_share_list);
static inline bool check_bandwidth_and_delay(struct qos_token_bucket *bucket, uint64_t bytes, void *caller_data, unsigned int op_type);
static inline bool check_bandwidth_and_reschedule(struct qos_token_bucket *bucket, uint64_t bytes, void *caller_data, unsigned int op_type);
static inline uint64_t get_time_in_usec(void);
static inline uint64_t get_time_future_useconds(uint64_t current, uint64_t seconds, uint64_t mseconds, uint64_t useconds);
static inline struct qos_token_bucket *qos_get_bucket(void *entry, unsigned int class_type, unsigned int  op_type);
static inline struct qos_token_bucket *qos_get_token_bucket(void *entry, unsigned int class_type, unsigned int  op_type);
static inline struct qos_token_bucket *qos_get_bw_bucket(void *entry, unsigned int class_type, unsigned int  op_type);
static inline void qos_bw_bucket_deffer_task(struct qos_token_bucket *bucket, void *caller_data, uint64_t timeout, uint64_t size, unsigned int op_type);
static inline void release_wait_ios(timer_entry_t **head, unsigned int *counter1 ,unsigned int *counter2 );
void pspc_free_client_list(struct QoS_perClient_Class **head);
struct QoS_perClient_Class *pspc_remove_client_from_list(struct QoS_perClient_Class **head, sockaddr_t *client_addr);

extern void nfs4_qos_write_cb(void *args);
extern void nfs4_qos_read_cb(void *args);

struct QoS_perShare_Class  *get_share_qos(struct gsh_export *export);
struct QoS_perClient_Class *get_client_qos(struct gsh_client *client);

#define  THREAD_DELAY_NFS_ERR_DELAY_DEFAULT	15
#define  THREAD_DELAY_NFS_ERR_DELAY_IMMED	1

#define  BW_SYNC_ENABLE		0
#define  BW_ASYNC_ENABLE	!BW_SYNC_ENABLE

#define  QOS_NOT_ENABLED       0
#define  QOS_PS_ENABLED        1
#define  QOS_PC_ENABLED        2
#define  QOS_PS_PC_ENABLED     3
extern nfs_parameter_t nfs_param;
struct qos_block_config *g_qos_config = (struct qos_block_config *)&nfs_param.core_param.qos_global_config;
#define  DELAY_MSEC	1000
/*  do not change this value below 3 msec,
 *  will reduce this value to 1 millisecond once the testing is done */
#define  BW_DELAY_MSEC 	5
#define  BW_DELAY_USEC	BW_DELAY_MSEC * 1000
#define  BW_CLIENT_FW_IO_SCHEDULE BW_DELAY_USEC*20
#define  BW_SHARE_FW_IO_SCHEDULE BW_DELAY_USEC*5
#define  TOKEN_DELAY_IN_MSEC	1000
#define  TOKEN_REFRESH_DELAY	TOKEN_DELAY_IN_MSEC/BW_DELAY_MSEC

static inline void qos_drain_token_ios(void *qos_class, unsigned int qos_class_type)
{
	struct qos_client_entry *token_client = NULL;
	struct qos_client_entry *temp = NULL;
	uint32_t *num_ios_waiting = NULL;
	bool token_enabled = false;
	if(qos_class_type == QOS_SHARE) {
		token_enabled = ((struct QoS_perShare_Class *)qos_class)->token_enabled;
		token_client = ((struct QoS_perShare_Class *)qos_class)->client_entries;
		num_ios_waiting = &(((struct QoS_perShare_Class *)qos_class)->num_ios_waiting);

	} else {
		token_enabled = ((struct QoS_perClient_Class *)qos_class)->token_enabled;
		token_client = ((struct QoS_perClient_Class *)qos_class)->client_entries;
		num_ios_waiting = &(((struct QoS_perClient_Class *)qos_class)->num_ios_waiting);
	}
	if (token_enabled && token_client != NULL) {
		while (token_client != NULL) {
			temp = token_client->next;
			LogFullDebug(COMPONENT_QOS,"token clients present:%d:%p:%d",g_qos_config->qos_type, token_client, token_client->num_ios_waiting);
			release_wait_ios(&(token_client->io_waitlist_qos), num_ios_waiting, &(token_client->num_ios_waiting));
			token_client = temp;
		}
	}
}
static inline void qos_drain_bw_ios(void *qos_class, unsigned int qos_class_type)
{
	bool bw_enabled = false;

	if(qos_class_type == QOS_SHARE) {
		bw_enabled = ((struct QoS_perShare_Class *)qos_class)->bw_enabled;
	} else {
		bw_enabled = ((struct QoS_perClient_Class *)qos_class)->bw_enabled;
	}
	if (bw_enabled) {
		struct qos_token_bucket *bucket = NULL;
		bucket = qos_get_bw_bucket(qos_class, qos_class_type, QOS_READ);
		release_wait_ios(&(bucket->io_waitlist_qos_bc), &(bucket->num_ios_waiting), NULL);
		bucket = qos_get_bw_bucket(qos_class, qos_class_type, QOS_WRITE);
		release_wait_ios(&(bucket->io_waitlist_qos_bc), &(bucket->num_ios_waiting), NULL);
	}
}

bool pspc_per_export_free_mem_cb(struct gsh_export *export, void *state)
{
	if(export == NULL)
		return true;
	struct QoS_perShare_Class *s_qos_class = export->qos_class;
	/*  list is not populated */
	if (s_qos_class == NULL || s_qos_class->clients == NULL) {
		return true;
	}

	struct QoS_perClient_Class *c_qos_class = pspc_remove_client_from_list(&(s_qos_class->clients), (sockaddr_t *)state);
	if (c_qos_class != NULL) {
		LogFullDebug(COMPONENT_QOS,"Tried freeing client:%d from export mem :%p",export->qos_class->share_id, (sockaddr_t *)state);
		qos_drain_token_ios(c_qos_class, QOS_CLIENT);
		qos_drain_bw_ios(c_qos_class, QOS_CLIENT);
		free(c_qos_class);
	} else  {
		LogFullDebug(COMPONENT_QOS,"Tried freeing client:%d from export mem :%p",export->qos_class->share_id, (sockaddr_t *)state);
	}
	return true; //Continue th eiteration for next share
}

void qos_free_mem(void *gsh_ptr, unsigned int qos_class_type)
{
	struct gsh_export *export = gsh_ptr;
	struct gsh_client *client = gsh_ptr;

	if (qos_class_type == QOS_SHARE) {
		if(export == NULL || export->qos_class == NULL)
			return;
	} else { //if(qos_class_type ==  QOS_CLIENT) {
		if(client == NULL)
			return;
	}

	switch (g_qos_config->qos_type) {
		case QOS_PS_ENABLED:
			if (qos_class_type == QOS_SHARE) {
				qos_drain_token_ios(export->qos_class, QOS_SHARE);
				qos_drain_bw_ios(export->qos_class, QOS_SHARE);
				LogFullDebug(COMPONENT_QOS,"freeing export mem :%d",export->qos_class->share_id);
				free(export->qos_class);
			}
			break;
		case QOS_PC_ENABLED:
			if (qos_class_type == QOS_CLIENT && client->qos_class != NULL) {
				LogFullDebug(COMPONENT_QOS,"freeing client mem :%p", client->qos_class->client_addr);
				qos_drain_token_ios(client->qos_class, QOS_CLIENT);
				qos_drain_bw_ios(client->qos_class, QOS_CLIENT);
				free(client->qos_class);
			}
			break;
		case QOS_PS_PC_ENABLED:
			if(qos_class_type ==  QOS_SHARE) {
				struct  QoS_perShare_Class *s_qos_class = export->qos_class;
				struct QoS_perClient_Class *c_qos_class = s_qos_class->clients;
				LogFullDebug(COMPONENT_QOS,"freeing export mem :%d",export->qos_class->share_id);
				/* releasing BW waiting io's */
				while (c_qos_class != NULL) {
					qos_drain_token_ios(c_qos_class, QOS_CLIENT);
					qos_drain_bw_ios(c_qos_class, QOS_CLIENT);
					c_qos_class = c_qos_class->next;
				}
				pspc_free_client_list(&s_qos_class->clients);
				qos_drain_token_ios(s_qos_class, QOS_SHARE);
				qos_drain_bw_ios(s_qos_class, QOS_SHARE);
				free(export->qos_class);
			} else  {
				foreach_gsh_export(pspc_per_export_free_mem_cb, false, &(client->cl_addrbuf));
			}
			break;
		default :
			LogFullDebug(COMPONENT_QOS," Something really wrong:%d",g_qos_config->qos_type);
	}
}


void init_bucket_value(struct qos_token_bucket *bucket)
{
	bucket->num_ios_waiting = 0;
	bucket->max_bw_allowed = 0;
	bucket->max_available_tokens = 0;
	bucket->tokens_consumed = 0;
	bucket->last_tokens_consumed_time = 0;
	bucket->bw_ldct = 0;
	bucket->io_waitlist_qos_bc = NULL;
	pthread_mutex_init(&(bucket->lock), NULL);

}

void set_bucket_value(struct qos_token_bucket *bucket, unsigned int max_bw, unsigned int max_tokens, unsigned int tokens_renew_time)
{
	bucket->max_bw_allowed = max_bw;
	bucket->max_available_tokens = max_tokens;
	bucket->tokens_renew_time = (tokens_renew_time*1000000);
}
void print_bucket_values(struct qos_token_bucket *bucket)
{
	LogFullDebug(COMPONENT_QOS,"bucket_value: wio:%d, bw:%ld, bw_ldct:%ld, mat:%ld, tc:%ld trt:%ld, ltct:%ld",
			bucket->num_ios_waiting,
			bucket->max_bw_allowed,
			bucket->bw_ldct,
			bucket->max_available_tokens,
			bucket->tokens_consumed,
			bucket->tokens_renew_time,
			bucket->last_tokens_consumed_time);

}
static inline void print_class_values(void *qos_class, unsigned int qos_class_type, const char *str)
{
	if (qos_class_type == QOS_SHARE) {
		struct QoS_perShare_Class *share = qos_class;
		LogFullDebug(COMPONENT_QOS,"%s QOS_TYPE:PER_SHARE SI:%d s_wio:%d bw_enabled:%d, token_enabled:%d c_rw_bw:%d, c_rw_token:%d",
				str, share->share_id,
				share->num_ios_waiting,
				share->bw_enabled, share->token_enabled, share->combined_rw_bw_control, share->combined_rw_token_control);
		print_bucket_values(&(share->read_bucket));
		print_bucket_values(&(share->write_bucket));

	} else if (qos_class_type == QOS_CLIENT) {
		struct QoS_perClient_Class *client = qos_class;
		LogFullDebug(COMPONENT_QOS,"%s QOS_TYPE:PER_CLIENT SI:%p s_wio:%d bw_enabled:%d, token_enabled:%d c_rw_bw:%d, c_rw_token:%d",
				str, client->client_addr,
				client->num_ios_waiting,
				client->bw_enabled, client->token_enabled, client->combined_rw_bw_control, client->combined_rw_token_control);
		print_bucket_values(&(client->read_bucket));
		print_bucket_values(&(client->write_bucket));
	}

}
void set_bucket_values(void* entry, unsigned int class_type, struct qos_block_config *in)
{
	if(in->enable_qos == false){
		return;
	}
	if (class_type == QOS_SHARE) {
		struct QoS_perShare_Class *share_entry = entry;
		if ((g_qos_config->enable_tokens && in->enable_tokens) && (g_qos_config->enable_bw_control && in->enable_bw_control)) {
			share_entry->bw_enabled = 1;
			share_entry->token_enabled = 1;
			set_bucket_value(&(share_entry->read_bucket), in->max_export_read_bw, in->max_export_read_tokens, in->export_read_tokens_renew_time);
			set_bucket_value(&(share_entry->write_bucket),in->max_export_write_bw, in->max_export_write_tokens, in->export_write_tokens_renew_time);
		} else if (g_qos_config->enable_bw_control && in->enable_bw_control) {
			share_entry->bw_enabled = 1;
			set_bucket_value(&(share_entry->read_bucket), in->max_export_read_bw, 0, 0);
			set_bucket_value(&(share_entry->write_bucket),in->max_export_write_bw, 0, 0);
		} else if (g_qos_config->enable_tokens && in->enable_tokens) {
			share_entry->token_enabled = 1;
			set_bucket_value(&(share_entry->read_bucket),  0, in->max_export_read_tokens, in->export_read_tokens_renew_time);
			set_bucket_value(&(share_entry->write_bucket), 0, in->max_export_write_tokens, in->export_write_tokens_renew_time);
		}
		print_class_values(share_entry, QOS_SHARE, "debugdp");
	} else {
		struct QoS_perClient_Class *client_entry = entry;
		if ((g_qos_config->enable_tokens && in->enable_tokens) && (g_qos_config->enable_bw_control && in->enable_bw_control)) {
			client_entry->bw_enabled = 1;
			client_entry->token_enabled = 1;
			set_bucket_value(&(client_entry->read_bucket), in->max_client_read_bw, in->max_client_read_tokens, in->client_read_tokens_renew_time);
			set_bucket_value(&(client_entry->write_bucket),in->max_client_write_bw, in->max_client_write_tokens, in->client_write_tokens_renew_time);
		} else if (g_qos_config->enable_bw_control && in->enable_bw_control) {
			client_entry->bw_enabled = 1;
			set_bucket_value(&(client_entry->read_bucket), in->max_client_read_bw, 0, 0 );
			set_bucket_value(&(client_entry->write_bucket),in->max_client_write_bw,0, 0 );
		} else if (g_qos_config->enable_tokens && in->enable_tokens) {
			client_entry->token_enabled = 1;
			set_bucket_value(&(client_entry->read_bucket), 0, in->client_read_tokens_renew_time, in->max_client_read_tokens);
			set_bucket_value(&(client_entry->write_bucket),0, in->client_write_tokens_renew_time, in->max_client_write_tokens);
		}
		print_class_values(client_entry, QOS_CLIENT, "debugdp");
	}
}

void setNode_ps(struct QoS_perShare_Class* node, uint16_t export_id, struct qos_block_config *qos_block)
{
	init_bucket_value(&(node->read_bucket));
	init_bucket_value(&(node->write_bucket));
	node->share_id = export_id;
	node->clients = NULL;
	node->client_entries = NULL;
	node->num_ios_waiting = 0;
	node->combined_rw_bw_control = qos_block->combined_rw_bw_control;
	node->combined_rw_token_control = 1;
	set_bucket_values(node, QOS_SHARE, qos_block);
	pthread_mutex_init(&(node->lock), NULL);
	return;
}

void setNode_pc(struct QoS_perClient_Class* node, sockaddr_t *client_addr, struct qos_block_config *qos_block)
{
	node->client_addr = client_addr;
	init_bucket_value(&(node->read_bucket));
	init_bucket_value(&(node->write_bucket));
	node->client_entries = NULL;
	node->next = NULL;
	node->num_ios_waiting = 0;
	node->combined_rw_bw_control = qos_block->combined_rw_bw_control;
	node->combined_rw_token_control = qos_block->combined_rw_token_control;
	set_bucket_values(node, QOS_CLIENT, qos_block);
	pthread_mutex_init(&(node->lock), NULL);
	return;
}

void QoS_perShareInsert(struct gsh_export *export, struct qos_block_config *qos_block)
{
	/*  Condition indicates this is new export or run time enabled of QOS due to global config*/
	if (export->qos_class == NULL) {
		struct QoS_perShare_Class* newNode = (struct QoS_perShare_Class*)malloc(sizeof(struct QoS_perShare_Class));
		memset(newNode, 0, sizeof(struct QoS_perShare_Class));
		/* NULL Indicates QOS block is not popultaed i.e run time enabledment of QOS */
		if (qos_block != NULL) {
			setNode_ps(newNode, export->export_id, qos_block);
			export->qos_class = newNode;
		} else {
			setNode_ps(newNode, export->export_id, g_qos_config);
			export->qos_class = newNode;
		}
	} else {
		LogFullDebug(COMPONENT_QOS,"NOT NULL EXPORT NOT EXPECTED");

	}
	return;
}

struct QoS_perClient_Class *pspc_allocate_client(void) {
	struct QoS_perClient_Class* newNode = (struct QoS_perClient_Class*)malloc(sizeof(struct QoS_perClient_Class));
	if (!newNode) {
		LogFullDebug(COMPONENT_QOS,"memallocation failed for client_class");
		return NULL; // Allocation failed
	}
	memset(newNode, 0, sizeof(struct QoS_perClient_Class));
	return newNode;
}
struct QoS_perClient_Class *pspc_allocate_and_init_client(sockaddr_t *client_addr, struct qos_block_config *qos_block)
{
	struct QoS_perClient_Class *new_node = pspc_allocate_client();
	if (new_node) {
		setNode_pc(new_node, client_addr, qos_block);
	}
	return new_node;

}

void pspc_add_client_to_list(struct QoS_perClient_Class **head, struct QoS_perClient_Class *client) {
	if (!client) {
		return;
	}
	client->next = *head;
	*head = client;
}

struct QoS_perClient_Class *pspc_allocate_init_add_client(struct QoS_perClient_Class **head, sockaddr_t *client_addr, struct qos_block_config *qos_block)
{
	struct QoS_perClient_Class *new_node = pspc_allocate_and_init_client(client_addr, qos_block);
	if (new_node)
		pspc_add_client_to_list(head, new_node);
	return new_node;

}

struct QoS_perClient_Class *pspc_get_client_from_list(struct QoS_perClient_Class *head, sockaddr_t *client_addr) {
	struct QoS_perClient_Class *current = head;

	while (current != NULL) {
		if (current->client_addr == client_addr) {
			return current; // Client found
		}
		current = current->next;
	}
	return NULL; // Client not found
}

struct QoS_perClient_Class *pspc_remove_client_from_list(struct QoS_perClient_Class **head, sockaddr_t *client_addr) {
	struct QoS_perClient_Class *current = *head;
	struct QoS_perClient_Class *prev = NULL;

	while (current != NULL) {
		if (current->client_addr == client_addr) {
			if (prev == NULL) {
				*head = current->next; // Removing the head
			} else {
				prev->next = current->next; // Bypass the current node
			}
			return current;
		}
		prev = current;
		current = current->next;
	}
	return NULL;
}
/*  Free all the clients related to a share */
void pspc_free_client_list(struct QoS_perClient_Class **head)
{
	struct QoS_perClient_Class *current = *head;
	struct QoS_perClient_Class *next;
	while (current != NULL) {
		next = current->next;
		free(current);
		current = next;
	}
	head = NULL;
}

void QoS_perClientInsert(struct qos_block_config *qos_block, struct gsh_client *client)
{
	struct QoS_perClient_Class* newNode = (struct QoS_perClient_Class*)malloc(sizeof(struct QoS_perClient_Class));
	memset(newNode, 0, sizeof(struct QoS_perClient_Class));
	if(qos_block == NULL) {
		setNode_pc(newNode, &client->cl_addrbuf, g_qos_config);
		client->qos_class = newNode;
	} else {
		setNode_pc(newNode, &client->cl_addrbuf, qos_block);
		client->qos_class = newNode;
	}
	return;
}

static inline uint64_t get_time_in_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000); // Convert to microseconds
}

/*  Manupulte this function to make single token bucket or independant read/write bucket */
static inline struct qos_token_bucket *qos_get_bucket(void *entry, unsigned int class_type, unsigned int op_type)
{
	if (class_type == QOS_SHARE || class_type == QOS_PSPC)
		return (op_type == QOS_READ) ? &((struct QoS_perShare_Class *)entry)->read_bucket : &((struct QoS_perShare_Class *)entry)->write_bucket;
	else
		return (op_type == QOS_READ) ? &((struct QoS_perClient_Class *)entry)->read_bucket : &((struct QoS_perClient_Class *)entry)->write_bucket;

}
static inline struct qos_token_bucket *qos_get_token_bucket(void *qos_class, unsigned int class_type, unsigned int op_type)
{
	if (class_type == QOS_SHARE || class_type == QOS_PSPC) {
		struct QoS_perShare_Class *share = qos_class;
		if(share->token_enabled == 0)
			return NULL;
		if(share->combined_rw_token_control)
			op_type = QOS_WRITE;
		return qos_get_bucket(share, class_type, op_type);
	} else {
		struct QoS_perClient_Class *client = qos_class;
		if(client->token_enabled == 0)
			return NULL;
		if (client->combined_rw_token_control)
			op_type = QOS_WRITE;
		return qos_get_bucket(client, class_type, op_type);
	}
}
static inline struct qos_token_bucket *qos_get_bw_bucket(void *qos_class, unsigned int class_type, unsigned int op_type)
{
	if (class_type == QOS_SHARE || class_type == QOS_PSPC) {
		struct QoS_perShare_Class *share = qos_class;
		if(share->bw_enabled == 0)
			return NULL;
		if(share->combined_rw_bw_control)
			op_type = QOS_WRITE;
		return qos_get_bucket(share, class_type, op_type);
	} else {
		struct QoS_perClient_Class *client = qos_class;
		if(client->bw_enabled == 0)
			return NULL;
		if (client->combined_rw_bw_control)
			op_type = QOS_WRITE;
		return qos_get_bucket(client, class_type, op_type);
	}
}

/*  True indicates : consumed the token for the current io
 *  False indicates : Not able to consume token i.,e tokens alreday exuhasted
 **/
static bool qos_check_bucket_token_availablity(struct qos_token_bucket *bucket, uint64_t request_size)
{
	if (bucket->tokens_consumed <= bucket->max_available_tokens) {
		return true;
	} else {
		return false;
	}
}

static void qos_consume_bucket_token(struct qos_token_bucket *bucket, uint64_t request_size)
{
	bucket->last_tokens_consumed_time =get_time_in_usec();
	bucket->tokens_consumed += request_size;
}

/*  True indicates : consumed the token for the current io or was not suppose to consume token
 *  False indicates : Not able to consume token i.,e tokens alreday exuhasted
 **/
static bool qos_check_token_availablity(void *qos_class, uint64_t request_size, unsigned int op_type, unsigned int class_type)
{
	struct qos_token_bucket *bucket = qos_get_token_bucket(qos_class, class_type, op_type);
	if (bucket == NULL)
		return true;
	return qos_check_bucket_token_availablity(bucket, request_size);
}

static void qos_consume_token(void *qos_class, uint64_t request_size, unsigned int op_type, unsigned int class_type)
{
	struct qos_token_bucket *bucket = qos_get_token_bucket(qos_class, class_type, op_type);
	if (bucket == NULL)
		return;
	return qos_consume_bucket_token(bucket, request_size);
}
/*
return true i.e ASYNC scheduled
return false on SYNC delay or BW is not enabled
*/
static bool qos_control_bucket_bw(struct qos_token_bucket *bucket, uint64_t request_size, unsigned int op_type, void *caller_data)
{
	LogFullDebug(COMPONENT_QOS,"Max_bw_io :%ld BW_control_type:%d ", bucket->max_bw_allowed, BW_SYNC_ENABLE);
	if (BW_SYNC_ENABLE && !check_bandwidth_and_delay(bucket, request_size, caller_data, op_type)) {
		return true;
	} else if  (BW_ASYNC_ENABLE && !check_bandwidth_and_reschedule(bucket, request_size, caller_data, op_type) ){
		return false;
	} else  {
		return true;
	}
}
static bool qos_control_bw(void *qos_class, uint64_t request_size, unsigned int op_type, void *caller_data, unsigned int class_type)
{
	struct qos_token_bucket *bucket = qos_get_bw_bucket(qos_class, class_type, op_type);
	if (bucket == NULL)
		return true;
	return qos_control_bucket_bw(bucket, request_size, op_type, caller_data);
}

static inline void qos_bw_deffer_task(void *qos_class, void *caller_data, uint64_t size, uint64_t timeout, unsigned int op_type, unsigned int class_type)
{
	struct qos_token_bucket *bucket = qos_get_bw_bucket(qos_class, class_type, op_type);
	if (bucket == NULL)
		return;
	return qos_bw_bucket_deffer_task(bucket, caller_data, size, timeout, op_type);
}
static bool qos_check(void *class_ptr, uint64_t request_size, unsigned int op_type, void *caller_data, compound_data_t *data, unsigned int class_type)
{
	if (class_ptr) {

		/*  this is temporary place, need to remove it based on the QOS config */
		if (qos_initalized == 0) {
			LogFullDebug(COMPONENT_QOS,"QOS thread_init");
			qos_thread_init();
		}

		/* This is Per-Share block */
		if (class_type == QOS_SHARE) {
			struct QoS_perShare_Class *qos_class =  class_ptr;
			pthread_mutex_lock(&qos_class->lock);
			if (!qos_check_token_availablity(qos_class, request_size, op_type,  QOS_SHARE)) {
				qos_token_exausted_deffer_task(qos_class, caller_data, data, QOS_SHARE, op_type);
				pthread_mutex_unlock(&qos_class->lock);
				return false;
			}  else if (!qos_control_bw(qos_class, request_size, op_type, caller_data, QOS_SHARE)) {
				/*  Consume the ASYNC scheduled tokens */
				qos_consume_token(qos_class, request_size, op_type, QOS_SHARE);
				pthread_mutex_unlock(&qos_class->lock);
				return false;
			}
			qos_consume_token(qos_class, request_size, op_type, QOS_SHARE);
			pthread_mutex_unlock(&qos_class->lock);
		/* Per-Client check below */
		} else if (class_type == QOS_CLIENT) {
			struct QoS_perClient_Class *qos_class =  class_ptr;
			pthread_mutex_lock(&qos_class->lock);
			if (!qos_check_token_availablity(qos_class, request_size, op_type, QOS_CLIENT)){
				qos_token_exausted_deffer_task(qos_class, caller_data, data, QOS_CLIENT, op_type);
				pthread_mutex_unlock(&qos_class->lock);
				return false;
			}  else if (!qos_control_bw(qos_class, request_size, op_type, caller_data, QOS_CLIENT)) {
				qos_consume_token(qos_class, request_size, op_type, QOS_CLIENT);
				pthread_mutex_unlock(&qos_class->lock);
				return false;
			}
			qos_consume_token(qos_class, request_size, op_type, QOS_CLIENT);
			pthread_mutex_unlock(&qos_class->lock);
		/* PerShare-PerClient checks below */
		} else if (class_type == QOS_PSPC) {
			struct QoS_perShare_Class *s_qos_class =  class_ptr;
			struct QoS_perClient_Class *c_qos_class =  pspc_get_client_from_list(s_qos_class->clients, &op_ctx->client->cl_addrbuf);

			int share_token_available = qos_check_token_availablity(s_qos_class, request_size, op_type,  QOS_SHARE);
			int client_token_available = qos_check_token_availablity(c_qos_class, request_size,  op_type,  QOS_CLIENT);

			if(!share_token_available) {
				qos_token_exausted_deffer_task(s_qos_class, caller_data, data, QOS_SHARE, op_type);
				pthread_mutex_unlock(&s_qos_class->lock);
				return false;
			} else if(!client_token_available) {
				qos_token_exausted_deffer_task(c_qos_class, caller_data, data, QOS_CLIENT, op_type);
				pthread_mutex_unlock(&s_qos_class->lock);
				return false;
			} else {
				/*  consume the Tokens and schedule for ASYNC, rescheuling of IO will happend later BW control also decided later*/
				pthread_mutex_lock(&s_qos_class->lock);
				qos_consume_token(s_qos_class, request_size, op_type, QOS_SHARE);
				qos_consume_token(c_qos_class, request_size, op_type, QOS_CLIENT);
				if(c_qos_class->bw_enabled) {
					//qos_bw_deffer_task(c_qos_class, caller_data, request_size, get_time_in_usec(), op_type, QOS_CLIENT);
					// In testing(on VM with less resource) have seen a situaution where multiple free are happening
					// 1-from qos svc_resume path and other from the current caller also.
					qos_bw_deffer_task(c_qos_class, caller_data, request_size, get_time_in_usec()+BW_DELAY_USEC, op_type, QOS_CLIENT);
					pthread_mutex_unlock(&s_qos_class->lock);
					return false;
				} else {
					pthread_mutex_unlock(&s_qos_class->lock);
					return true;
				}
			}
			LogFullDebug(COMPONENT_QOS,"debugdp Something wrong");
			pthread_mutex_unlock(&s_qos_class->lock);
		} else {
			return true;
		}
	}
	return true;
}

unsigned int QoS_Process(unsigned int size, void *caller_data, compound_data_t *data, unsigned int op_type)
{
	if (g_qos_config->qos_type == QOS_NOT_ENABLED || g_qos_config->enable_qos == 0) {
		return 0;
	} else if (g_qos_config->qos_type ==  QOS_PS_ENABLED) {
		if (op_ctx->ctx_export->qos_class == NULL) {
			LogFullDebug(COMPONENT_QOS,"PS key not found for :%s, so creating new entry", op_ctx->ctx_export->cfg_fullpath);
			QoS_perShareInsert(op_ctx->ctx_export, g_qos_config);
		}
		if (!qos_check(op_ctx->ctx_export->qos_class, size, op_type, caller_data, data, QOS_SHARE)) {
			return 1;
		}
	} else if (g_qos_config->qos_type ==  QOS_PC_ENABLED) {
		if (op_ctx->client->qos_class == NULL) {
			LogFullDebug(COMPONENT_QOS,"PC client entry not found :%p, creating new client", &op_ctx->client->cl_addrbuf);
			/* Since this is QOS_PC, pass the global QOS values */
			QoS_perClientInsert(g_qos_config, op_ctx->client);
		}
		if (!qos_check(op_ctx->client->qos_class, size, op_type, caller_data, data, QOS_CLIENT)) {
			return 1;
		}
	} else if (g_qos_config->qos_type == QOS_PS_PC_ENABLED) {
		char *key = op_ctx->ctx_export->cfg_fullpath;
		struct QoS_perShare_Class *share = op_ctx->ctx_export->qos_class;
		if (share == NULL) {
			/*  Execution reached here means QOS is enabled but QOS block is not populated for this share
			 *  so apply the global values to the share values */
			QoS_perShareInsert(op_ctx->ctx_export, g_qos_config);
			share = op_ctx->ctx_export->qos_class;
		}
		/* Is QOS disabled for this particular share */
		if(share->bw_enabled || share->token_enabled) {
			struct QoS_perClient_Class *client = NULL;
			client = pspc_get_client_from_list(share->clients, &op_ctx->client->cl_addrbuf);
			if (client == NULL) {
				LogFullDebug(COMPONENT_QOS,"Share:%s Client not found: %p",key, &op_ctx->client->cl_addrbuf);
				client = pspc_allocate_init_add_client(&(share->clients), &op_ctx->client->cl_addrbuf,
						op_ctx->ctx_export->qos_block);
			}

			LogFullDebug(COMPONENT_QOS,"debugdp PerShare key found :%s", key);
			if (!qos_check(op_ctx->ctx_export->qos_class, size, op_type, caller_data, data, QOS_PSPC)) {
				return 1;
			}
		}

	}  else {
		LogFullDebug(COMPONENT_QOS," INVALID QOS_TYPE:%d",g_qos_config->qos_type);
	}
	return 0;
}


qos_svc_rcb get_qos_resume_cb(unsigned int  op_type)
{
	return (op_type == QOS_READ) ? nfs4_qos_read_cb : nfs4_qos_write_cb;
}

uint64_t qos_get_time_to_tokenrefresh(void *qos_class, unsigned int class_type, unsigned int op_type , uint64_t ctime)
{
	struct qos_token_bucket *bucket = qos_get_token_bucket(qos_class, class_type, op_type);
	LogFullDebug(COMPONENT_QOS,"LTC:%ld TRT:%ld CT:%ld TO:%ld",
		bucket->last_tokens_consumed_time, bucket->tokens_renew_time,  ctime, ((bucket->last_tokens_consumed_time + bucket->tokens_renew_time) - ctime)/1000000);

	return (((bucket->last_tokens_consumed_time + bucket->tokens_renew_time) - ctime)/1000000);
}

static inline struct qos_op_cb_arg *alloc_qos_cb_args(void *caller_data, int ratecontrol)
{
	struct qos_op_cb_arg *qos_cb_args = NULL;
	qos_cb_args =  (struct qos_op_cb_arg *)malloc(sizeof(struct qos_op_cb_arg));
	if (qos_cb_args == NULL) {
		LogFullDebug(COMPONENT_QOS,"ERROR mem allocation failed");
	}
	memset(qos_cb_args, 0, sizeof(struct qos_op_cb_arg));
	qos_cb_args->caller_data = caller_data;
	qos_cb_args->ratecontrol = ratecontrol;
	return qos_cb_args;
}

/*  obj and write_data args not required, but will keep for sometime before removing not required args */
static void qos_token_exausted_deffer_task(void *ptr, void *caller_data , compound_data_t *data, unsigned int class_type, unsigned int op_type)
{
	struct qos_op_cb_arg *qos_cb_args = alloc_qos_cb_args(caller_data, NON_RATELIMITING_IO);
	uint64_t ltime = get_time_in_usec();
	struct qos_client_entry *client = NULL;
	timer_entry_t *new_timer_entry = NULL;
	uint64_t timeout = 0;
	unsigned int *num_ios_waiting  = NULL;

	//Considering 15 seconds before returning to client
	timeout = get_time_future_useconds(ltime,  MIN(THREAD_DELAY_NFS_ERR_DELAY_DEFAULT, qos_get_time_to_tokenrefresh(ptr, class_type, op_type, ltime)), 0 , 0);

	if (class_type == QOS_SHARE) {
		client  = get_and_insert_client_details(&(((struct QoS_perShare_Class *)ptr)->client_entries), data);
		num_ios_waiting = &(((struct QoS_perShare_Class *)ptr)->num_ios_waiting);
	} else {
		client  = get_and_insert_client_details(&(((struct QoS_perClient_Class *)ptr)->client_entries), data);
		num_ios_waiting = &(((struct QoS_perClient_Class *)ptr)->num_ios_waiting);
	}

	if (client->num_ios_waiting >= 5 && client->epoll_disabled == 0) {
		client->epoll_disabled = 1;
		LogFullDebug(COMPONENT_QOS,"Suspending Client Socket true :%ld :%p ", client->clientid, client->rq_xprt);
		/*TODO: Need to uncommnet once libntirpc changes gets in by Animesh Javali */
		// svc_rqst_qos_suspend_socket(client->rq_xprt);
	} else if (client->num_ios_waiting >= 5 && client->epoll_disabled == 1) {
		timeout = get_time_future_useconds(ltime,THREAD_DELAY_NFS_ERR_DELAY_IMMED,0,0);
	}

	new_timer_entry = create_timer_entry(timeout, get_qos_resume_cb(op_type), (void *)qos_cb_args);
	insert_timer_entry(&(client->io_waitlist_qos), new_timer_entry);
	client->num_ios_waiting++;
	(*num_ios_waiting)++;

	LogFullDebug(COMPONENT_QOS,"Timer added: %p gio_waiters:%d client_io_waitlists:%d CI:%ld CT:%ld TO:%ld",
			new_timer_entry, *num_ios_waiting, client->num_ios_waiting, client->clientid, ltime, timeout);
}


static inline uint64_t get_time_future_useconds(uint64_t current, uint64_t seconds, uint64_t mseconds, uint64_t useconds)
{
	if (current == 0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		current  = (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000); // Convert to microseconds
	}
	return (current + (seconds * 1000000) + (mseconds * 1000) + useconds);
}

static inline void qos_bw_bucket_deffer_task(struct qos_token_bucket *bucket, void *caller_data, uint64_t size, uint64_t timeout, unsigned int op_type)
{
	struct qos_op_cb_arg *qos_cb_args = alloc_qos_cb_args(caller_data, RATELIMITING_IO);
	timer_entry_t *new_timer_entry = NULL;
	new_timer_entry = create_timer_entry(timeout, get_qos_resume_cb(op_type), (void *)qos_cb_args);
	new_timer_entry->size = size;
	pthread_mutex_lock(&bucket->lock);
	insert_timer_entry(&(bucket->io_waitlist_qos_bc), new_timer_entry);
	++bucket->num_ios_waiting;
	pthread_mutex_unlock(&bucket->lock);
}

// this function Controls bandwidth NON-BLOCKING-IO: Check bandwidth and delay if necessary
static inline bool check_bandwidth_and_reschedule(struct qos_token_bucket *bucket, uint64_t bytes, void *caller_data, unsigned int op_type)
{
	uint64_t last_time = bucket->bw_ldct;
	//uint64_t last_time = bucket->last_tokens_consumed_time;
	uint64_t current_time = get_time_in_usec();
	uint64_t required_time = (bytes * 1000000)/ bucket->max_bw_allowed; // Microseconds required to meet bandwidth
	LogFullDebug(COMPONENT_QOS,"ct:%ld bw_ldct:%ld rt:%ld bytes:%ld",current_time, last_time, required_time, bytes);
	/* if condition will be true for 1st time, rest for all IO's else will be true */
	if (current_time > last_time) {
		uint64_t time_since_last_op = current_time - last_time; // Microseconds elapsed since last call
		LogFullDebug(COMPONENT_QOS,"tslo:%ld rt:%ld",time_since_last_op, required_time);
		if (time_since_last_op < required_time) {
			// Calculate resume time in microseconds and rescheudle
			uint64_t resume_time = current_time + (required_time - time_since_last_op);
			LogFullDebug(COMPONENT_QOS,"ct:%ld, resumet:%ld rt:%ld tslo:%ld bw_ldct:%ld",
					current_time, resume_time, required_time, time_since_last_op, last_time);
			qos_bw_bucket_deffer_task(bucket, caller_data, bytes, resume_time, op_type);
			bucket->bw_ldct = resume_time;
			return false;
		}
		bucket->bw_ldct = current_time+required_time;
		//bucket->bw_ldct = current_time;
		return true;
	} else  {
		uint64_t resume_time = last_time + required_time;
		LogFullDebug(COMPONENT_QOS,"ct:%ld, resumet:%ld rt:%ld bw_ldct:%ld", current_time, resume_time, required_time, last_time);
		qos_bw_bucket_deffer_task(bucket, caller_data, bytes, resume_time, op_type);
		bucket->bw_ldct = resume_time;
		return false;
	}
}

// this function Controls bandwidth BLOCKING-IO : Check bandwidth and delay if necessary
static inline bool check_bandwidth_and_delay(struct qos_token_bucket *bucket, uint64_t bytes, void *caller_data, unsigned int op_type)
{
	uint64_t last_time = bucket->bw_ldct;
//	uint64_t last_time = bucket->last_tokens_consumed_time;
	uint64_t current_time = get_time_in_usec();
	uint64_t time_since_last_op = current_time - last_time; // Microseconds elapsed since last call
	uint64_t required_time = (bytes * 1000000)/ bucket->max_bw_allowed; // Microseconds required to meet bandwidth
	int ret = 0;
	if (time_since_last_op < required_time) {
		// Calculate delay in microseconds and sleep
		uint64_t delay_time = required_time - time_since_last_op;
		struct timespec delay;
		delay.tv_sec = delay_time / 1000000;
		delay.tv_nsec = (delay_time % 1000000) * 1000;
		LogFullDebug(COMPONENT_QOS,"ct:%ld, dt:%ld rt:%ld tslo:%ld bw_ldct:%ld dis:%ld dins:%ld bytes:%ld wba:%ld",
				current_time, delay_time, required_time, time_since_last_op, bucket->bw_ldct, delay.tv_sec, delay.tv_nsec, bytes, bucket->max_bw_allowed);
		LogFullDebug(COMPONENT_QOS,"ct:%ld",get_time_in_usec());
		ret = nanosleep(&delay, NULL); // Enforce delay to limit bandwidth
		if(ret != 0) {
			LogFullDebug(COMPONENT_QOS,"Sleep Failure ");
		}
		LogFullDebug(COMPONENT_QOS,"ct:%ld",get_time_in_usec());
	}
	bucket->bw_ldct = get_time_in_usec();
	return true;
}

static inline bool refresh_bucket_token(void *class_entry, unsigned int class_type, unsigned int op_type)
{
	struct qos_token_bucket *bucket = qos_get_token_bucket(class_entry, class_type, op_type);
	if (bucket == NULL)
		return 0;
	uint64_t ltime =get_time_in_usec();
	/* This is the logic for limiting the io based on  */
	if ((bucket->tokens_consumed >= bucket->max_available_tokens) &&
		(ltime > (bucket->last_tokens_consumed_time + bucket->tokens_renew_time))){
		LogFullDebug(COMPONENT_QOS,"debugdp CT:%ld LCT:%ld TRT:%ld calculation:%ld",
				ltime,
				bucket->last_tokens_consumed_time,
				bucket->tokens_renew_time,
				(bucket->last_tokens_consumed_time + bucket->tokens_renew_time));
		bucket->tokens_consumed = 0;
		return 1;
	} else {
		return 0;
	}
}

static inline bool refresh_per_share_tokens(struct QoS_perShare_Class *share_entry)
{
	// since the io waitlist queue are same for read and write ios, check for both and then return
	return (refresh_bucket_token(share_entry, QOS_SHARE, QOS_READ) || refresh_bucket_token(share_entry, QOS_SHARE, QOS_WRITE));
}

static inline bool refresh_per_client_tokens(struct QoS_perClient_Class *client_entry)
{
	// since the io waitlist queue are same for read and write ios, check for both and then return
	return (refresh_bucket_token(client_entry, QOS_CLIENT, QOS_READ) || refresh_bucket_token(client_entry, QOS_CLIENT, QOS_WRITE));
}

static inline timer_entry_t *create_timer_entry(uint64_t expiry, void (*callback)(void *), void *args)
{
	//timer_entry_t *new_entry = gsh_malloc(1, sizeof(timer_entry_t));
	timer_entry_t *new_entry = malloc(sizeof(timer_entry_t));
	memset(new_entry, 0, sizeof(timer_entry_t));
	if (!new_entry) {
		perror("Failed to allocate memory for timer entry");
		return NULL;
	}
	new_entry->expiry = expiry;
	new_entry->callback = callback;
	new_entry->args = args;
	new_entry->next = NULL;
	LogFullDebug(COMPONENT_QOS,"Timer entry created:%p", new_entry);
	return new_entry;
}

static struct qos_client_entry *alloc_clientdetails_ps(compound_data_t *data)
{
	struct qos_client_entry *new_entry = NULL;
	new_entry = (struct qos_client_entry *)malloc(sizeof(struct qos_client_entry));
	if (!new_entry) {
		perror("Failed to allocate memory for timer entry");
		return NULL;
	}
	new_entry->num_ios_waiting = 0;
	new_entry->next = NULL;
	new_entry->io_waitlist_qos = NULL;
	new_entry->client_addr = &op_ctx->client->cl_addrbuf;
	new_entry->data = data;
	new_entry->rq_xprt = data->req->rq_xprt;
	new_entry->epoll_disabled = 0;
	LogFullDebug(COMPONENT_QOS,"Adding Client entry CID:%p",new_entry->client_addr);
	return new_entry;
}

static struct qos_client_entry *get_and_insert_client_details(struct qos_client_entry **head, compound_data_t *data)
{
	struct qos_client_entry *new_entry = NULL;
	if (*head == NULL) {
		new_entry = alloc_clientdetails_ps(data);
		*head = new_entry;
		return new_entry;
	} else {
		struct qos_client_entry *current = *head;
		struct qos_client_entry *temp; /*  used to insert at the end if client is not in list */

		while (current != NULL && current->client_addr != &op_ctx->client->cl_addrbuf) {
			temp = current;
			current = current->next;
		}

		if ( current == NULL) {
			new_entry = alloc_clientdetails_ps(data);
			temp->next = new_entry;
			return new_entry;
		} else {
			return current;
		}
	}
	LogFullDebug(COMPONENT_QOS,"Something wrong in Client Specific details %p", head);
}

static void remove_client_entry(struct qos_client_entry **head, struct qos_client_entry *entry_to_remove)
{
	LogFullDebug(COMPONENT_QOS,"Removing Client entry head:%p remove: %p CID:%p ",*head, entry_to_remove, entry_to_remove->client_addr);
	if (*head == NULL) return;

	if (*head == entry_to_remove) {
		*head = (*head)->next;
		free(entry_to_remove);
		return;
	}

	struct qos_client_entry *current = *head;
	while (current->next != NULL && current->next != entry_to_remove) {
		current = current->next;
	}

	if (current->next == entry_to_remove) {
		current->next = entry_to_remove->next;
		free(entry_to_remove);
	}
}

static void insert_timer_entry(timer_entry_t **head, timer_entry_t *new_entry)
{
	if (new_entry == NULL) {
		LogFullDebug(COMPONENT_QOS,"ERROR new entry is NULL");
	}
	//LogFullDebug(COMPONENT_QOS,"Timer entry head:%p insert:%p", *head, new_entry);
	if (*head == NULL || (*head)->expiry > new_entry->expiry) {
		new_entry->next = *head;
		*head = new_entry;
	} else {
		timer_entry_t *current = *head;
		while (current->next != NULL && current->next->expiry <= new_entry->expiry) {
			current = current->next;
		}
		new_entry->next = current->next;
		current->next = new_entry;
	}
	//list_timer_entries(*head);
}

static void remove_timer_entry(timer_entry_t **head, timer_entry_t *entry_to_remove)
{
	LogFullDebug(COMPONENT_QOS,"Timer entry head:%p remove: %p",*head, entry_to_remove);
	if (*head == NULL) return;

	if (*head == entry_to_remove) {
		*head = (*head)->next;
		//free(entry_to_remove);
		free(entry_to_remove);
		return;
	}

	timer_entry_t *current = *head;
	while (current->next != NULL && current->next != entry_to_remove) {
		current = current->next;
	}

	if (current->next == entry_to_remove) {
		current->next = entry_to_remove->next;
		//free(entry_to_remove);
		free(entry_to_remove);
	}
}

void list_timer_entries(timer_entry_t *current_share_list)
{
	timer_entry_t *current =current_share_list;
	LogFullDebug(COMPONENT_QOS,"Current Timer Entries:");
	while (current != NULL) {
		LogFullDebug(COMPONENT_QOS,"Entry:%p, Expiry: %ld, Callback: %p, Args: %p",
				current, current->expiry, (void *)current->callback, current->args);
		current = current->next;
	}
}

/*  Force resume all the waiting IO's of share
 *  Condition : Io's waiting for replinsh of token and tokens got replinsh
 *		here delay added by QOS based on lease time will not be entertained
 */
static inline void release_wait_ios(timer_entry_t **head, unsigned int *counter1 ,unsigned int *counter2 )
{
	timer_entry_t *current = *head;
	timer_entry_t *expired = NULL;
	while (current != NULL) {
		current->callback(current->args);  // Call the callback for the expired timer
		LogFullDebug(COMPONENT_QOS,"Force resume Timer:%p Expiry:%ld TCounter:%d",current, current->expiry, *counter1);
		expired = current;
		current = current->next;
		remove_timer_entry(head, expired);
		--*counter1;
		--*counter2;
	}
}

/*  Resume all the waiting IO's of share, based on there lease expiry time set by QOS
 *		here delay added by QOS based on lease time will be entertained
 */
static void execute_qos_expired_timers(timer_entry_t **head, unsigned int *counter1 ,unsigned int *counter2)
{
	uint64_t current_time = get_time_in_usec();
	timer_entry_t *current = *head;
	timer_entry_t *expired = NULL;

	while (current != NULL) {
		if (current->expiry <= current_time) {
			if (counter1 != NULL) {
				LogFullDebug(COMPONENT_QOS,"Expired IO Timer:%p CT:%ld Expiry:%ld TCounter:%d",
						current, current_time, current->expiry, *counter1);
			} else {
				LogFullDebug(COMPONENT_QOS,"Expired IO Timer:%p CT:%ld Expiry:%ld",
						current, current_time, current->expiry);
			}
			current->callback(current->args);  // Call the callback for the expired timer
			expired = current;
		}
		current = current->next;
		if(expired != NULL) {
			remove_timer_entry(head, expired);
			if (counter1 != NULL && counter2 != NULL) {
				--*counter1;
				--*counter2;
			} else if (counter1 != NULL) {
				--*counter1;
			}
			expired = NULL;
			//list_timer_entries(head);
		}
	}
}

static inline void refresh_qos_client(struct qos_client_entry **clients, bool tokens_refreshed, void *qos_class, unsigned class_type)
{
	struct qos_client_entry *client = *clients;
	unsigned int *counter1 =  &(client->num_ios_waiting);
	unsigned int *counter2 =  (class_type == QOS_SHARE) ?
		&(((struct QoS_perShare_Class *)qos_class)->num_ios_waiting) :
		&(((struct QoS_perClient_Class *)qos_class)->num_ios_waiting);
	bool epd = 0;
	SVCXPRT *rq_xprt = NULL;

	LogFullDebug(COMPONENT_QOS," CI:%p CWIO's:%d ",client->client_addr, client->num_ios_waiting);
	if (tokens_refreshed) {
		release_wait_ios(&(client->io_waitlist_qos), counter1, counter2);
	} else if (client->num_ios_waiting) { /*  go with the QOS timer expiry for IO's set by QOS thread  */
		execute_qos_expired_timers(&(client->io_waitlist_qos), counter1, counter2);
	}

	if(client->num_ios_waiting == 0) {
		rq_xprt = client->rq_xprt;
		epd = client->epoll_disabled;
		LogFullDebug(COMPONENT_QOS,"Resuming Client Socket Cid:%p Xprt:%p epd:%d xprt:%p", client->client_addr, client->rq_xprt, client->epoll_disabled,rq_xprt);
		remove_client_entry(clients, client);
		if(epd == 1) {
			/*TODO: Need to uncommnet once libntirpc changes gets in by Animesh Javali */
			//svc_rqst_qos_resume_socket(rq_xprt);
			epd = 0;
		}
		rq_xprt = NULL;
	}
}

static inline void refresh_qos_token_by_class(void *class, unsigned int qos_class_type)
{
	bool tokens_refreshed = 0;
	struct qos_client_entry *client = NULL;
	struct qos_client_entry *temp = NULL;
	if(qos_class_type == QOS_SHARE) {
		struct QoS_perShare_Class *qos_class = class;
		tokens_refreshed = refresh_per_share_tokens(qos_class);
		LogFullDebug(COMPONENT_QOS," SN:%d TR:%d WIO's:%d",
					qos_class->share_id, tokens_refreshed, qos_class->num_ios_waiting);
		pthread_mutex_lock(&(qos_class->lock));
		client = qos_class->client_entries;
		while (client != NULL) {
			temp = client->next;
			refresh_qos_client(&(qos_class->client_entries), tokens_refreshed, qos_class, QOS_SHARE);
			client = temp;
		}
		pthread_mutex_unlock(&(qos_class->lock));
	} else {
		struct QoS_perClient_Class *qos_class = class;
		tokens_refreshed = refresh_per_client_tokens(qos_class);
		LogFullDebug(COMPONENT_QOS," CI:%p TR:%d WIO's:%d",
					qos_class->client_addr, tokens_refreshed, qos_class->num_ios_waiting);
		pthread_mutex_lock(&(qos_class->lock));
		client = qos_class->client_entries;
		while (client != NULL) {
			temp = client->next;
			refresh_qos_client(&(qos_class->client_entries), tokens_refreshed, qos_class, QOS_CLIENT);
			client = temp;
		}
		pthread_mutex_unlock(&(qos_class->lock));
	}
}

bool pspc_token_control_cb(struct gsh_export *export, void *state)
{
	struct QoS_perShare_Class *share = get_share_qos(export);
	if (share && share->token_enabled) {
		LogDebug(COMPONENT_QOS, "going for token refresh:%s",export->cfg_fullpath);
		refresh_qos_token_by_class(share, QOS_SHARE);
		/* In a Share check any client exausted the tockens and is replinish value reached */
		struct QoS_perClient_Class* client = share->clients;
		while (client != NULL) {
			refresh_qos_token_by_class(client, QOS_CLIENT);
			client = client->next;
		}
	}
	return true; // Continue iteration
}

bool ps_token_control_cb(struct gsh_export *export, void *state)
{
	struct QoS_perShare_Class *share = get_share_qos(export);
	if (share && share->token_enabled) {
		LogDebug(COMPONENT_QOS, "going for token refresh:%s",export->cfg_fullpath);
		refresh_qos_token_by_class(share, QOS_SHARE);
	}
	return true; // Continue iteration
}

bool pc_token_control_cb(struct gsh_client *cl, void *state)
{
	struct QoS_perClient_Class *client = get_client_qos(cl);
	if (client && client->token_enabled) {
		LogDebug(COMPONENT_QOS, "going for token refresh:%p", &cl->cl_addrbuf);
		refresh_qos_token_by_class(client, QOS_CLIENT);
	}
	return true; // Continue iteration
}

static inline void refresh_qos_token()
{
	/*  Token refershing function calls here
	 *  Share based refresing
	 *  Client based refresing
	 *  PerShare-PerClient based refresing
	 *  Group Based
	 *  Directory level */
	int op_type = 0;
	switch (g_qos_config->qos_type) {
		case QOS_NOT_ENABLED:
			LogFullDebug(COMPONENT_QOS,"QOS not enabled :%d",g_qos_config->qos_type);
			break;
		case QOS_PS_ENABLED:
			foreach_gsh_export(ps_token_control_cb, false, &op_type);
			break;
		case QOS_PC_ENABLED:
			foreach_gsh_client(pc_token_control_cb, &op_type);
			break;
		case QOS_PS_PC_ENABLED:
			foreach_gsh_export(pspc_token_control_cb, false, &op_type);
			break;
		default:
			LogFullDebug(COMPONENT_QOS," Something really wrong:%d",g_qos_config->qos_type);
			break;
	}
}

/*  Below all functions are resuming the asyncIO based on timeout set by BW calculation */
static inline void resume_bw_bucket_io(struct qos_token_bucket *bucket)
{
	execute_qos_expired_timers(&(bucket->io_waitlist_qos_bc), &(bucket->num_ios_waiting), NULL);
}

static inline void resume_bw_io_ps(struct QoS_perShare_Class* share, unsigned int op_type)
{
	if (share != NULL) {
		struct qos_token_bucket *bucket = qos_get_bw_bucket(share, QOS_SHARE, op_type);
		pthread_mutex_lock(&bucket->lock);
		resume_bw_bucket_io(bucket);
		pthread_mutex_unlock(&bucket->lock);
	}
}

static inline void resume_bw_io_pc(struct QoS_perClient_Class* client, unsigned int op_type)
{
	if (client != NULL) {
		struct qos_token_bucket *bucket = qos_get_bw_bucket(client, QOS_CLIENT, op_type);
		pthread_mutex_lock(&bucket->lock);
		resume_bw_bucket_io(bucket);
		pthread_mutex_unlock(&bucket->lock);
	}
}

static inline void resume_bw_io_pspc(struct QoS_perShare_Class* share, unsigned int op_type)
{
	if (share != NULL) {
		uint64_t current_time = get_time_in_usec();
		struct qos_token_bucket *bucket = qos_get_bw_bucket(share, QOS_PSPC, op_type);
		pthread_mutex_lock(&(bucket->lock));
		timer_entry_t *io_entry = bucket->io_waitlist_qos_bc;
		LogFullDebug(COMPONENT_QOS,"debugdp QOS_TYPE:PER_SHARE SI:%d s_wio:%d op_type:%d lct:%ld sb_io:%d ",
				share->share_id, share->num_ios_waiting, op_type,bucket->bw_ldct,bucket->num_ios_waiting) ;
		while ((io_entry != NULL) && (bucket->bw_ldct < (current_time))) {
			uint64_t required_time_for_io = (io_entry->size * 1000000) / bucket->max_bw_allowed;

			if (((bucket->bw_ldct + required_time_for_io + BW_SHARE_FW_IO_SCHEDULE) > current_time)) {
			//if (((bucket->bw_ldct + required_time_for_io) > current_time)) {
				/* Resuming BW from last IO completion */
				bucket->bw_ldct = bucket->bw_ldct + required_time_for_io;
			} else {
				/* Resuming BW from IDLE */
				bucket->bw_ldct = current_time;
			}

			--bucket->num_ios_waiting;
			io_entry->callback(io_entry->args);  // Call the callback for the expired timer
			bucket->io_waitlist_qos_bc = io_entry->next;
			free(io_entry);
			io_entry = bucket->io_waitlist_qos_bc;
		}
		pthread_mutex_unlock(&(bucket->lock));
	}
}

/*Control time indicates time per second is divided by how much granularity */
static inline void pspc_rescedule_io_to_share(struct qos_token_bucket *sbucket, struct qos_token_bucket *cbucket, uint64_t current_time)
{

	/*  we are here, since the IO in the queue are already consumed and we are suppose to schedule new IO */
	uint64_t clienttime  = current_time;
pick_next_io:
	timer_entry_t *io_entry = cbucket->io_waitlist_qos_bc;
	if (io_entry == NULL || io_entry->expiry > current_time)
		return;
	uint64_t required_time_for_io = (io_entry->size * 1000000)/ cbucket->max_bw_allowed;

	/*  This check ensures we dont exceed the Client bucket Limit */
	if (clienttime >= cbucket->bw_ldct) {
		/* below if ensures full BW is available for this client
		 * else indicate share limit has been reached so client is trottling*/
		if ((cbucket->bw_ldct + required_time_for_io + BW_CLIENT_FW_IO_SCHEDULE) > current_time) {
		//if ((cbucket->bw_ldct + required_time_for_io) > current_time) {
			cbucket->bw_ldct = cbucket->bw_ldct + required_time_for_io;
			io_entry->expiry = cbucket->bw_ldct;
		} else {
			cbucket->bw_ldct = current_time;
			io_entry->expiry = current_time;
		}
		cbucket->io_waitlist_qos_bc = io_entry->next;
		io_entry->next = NULL;
		insert_timer_entry(&(sbucket->io_waitlist_qos_bc), io_entry);
		++sbucket->num_ios_waiting;
		--cbucket->num_ios_waiting;

		if (cbucket->bw_ldct < (current_time + BW_CLIENT_FW_IO_SCHEDULE)) {
			clienttime = cbucket->bw_ldct;
			goto pick_next_io;
		}
	}
	return;
}

static inline void print_io_details(struct QoS_perShare_Class* share, struct QoS_perClient_Class* client,
		struct qos_token_bucket *sbucket, struct qos_token_bucket *cbucket,
		unsigned int op_type, unsigned int qos_class_type, uint64_t current_time, const char *str)
{
	if (qos_class_type == QOS_SHARE) {
		if (share != NULL) {
			LogFullDebug(COMPONENT_QOS,"%s debugdp QOS_TYPE:PER_SHARE SI:%d s_wio:%d op_type:%s lct:%ld pct:%ld sb_io:%d sbw_ldct:%ld",
					str, share->share_id, share->num_ios_waiting,
					(op_type == QOS_READ) ? "QOS_READ" : "QOS_WRITE",
					get_time_in_usec(), current_time,
					sbucket->num_ios_waiting, sbucket->bw_ldct);
		}
	} else if (qos_class_type == QOS_CLIENT) {
		if (client != NULL) {
			LogFullDebug(COMPONENT_QOS,"%s debugdp QOS_TYPE:PER_CLIENT CID:%p c_wio:%d op_type:%s lct:%ld pct:%ld cb_io:%d cbw_ldct:%ld",
					str, client->client_addr, client->num_ios_waiting,
					(op_type == QOS_READ) ? "QOS_READ" : "QOS_WRITE",
					get_time_in_usec(), current_time,
					cbucket->num_ios_waiting, cbucket->bw_ldct);
		}
	} else if (qos_class_type == QOS_PSPC) {
		if (share != NULL && client != NULL) {
			LogFullDebug(COMPONENT_QOS,"%s debugdp QOS_TYPE:PER_SHARE_PER_CLIENT SI:%d CID:%p s_wio:%d c_wio:%d op_type:%s lct:%ld pct:%ld sb_io:%d cb_io:%d sbw_ldct:%ld cbw_ldct:%ld",
					str, share->share_id, client->client_addr, share->num_ios_waiting, client->num_ios_waiting,
					(op_type == QOS_READ) ? "QOS_READ" : "QOS_WRITE",
					get_time_in_usec(), current_time,
					sbucket->num_ios_waiting, cbucket->num_ios_waiting, sbucket->bw_ldct, cbucket->bw_ldct);
		}
	}

}
static inline void print_all_io_details(void *qos_class, unsigned int op_type, unsigned int qos_class_type , const char *str)
{
	if (qos_class != NULL) {
		if (qos_class_type == QOS_SHARE || qos_class_type == QOS_PSPC) {
			struct QoS_perShare_Class *share = qos_class;
			struct qos_token_bucket *sbucket = qos_get_bw_bucket(share, QOS_SHARE, op_type);
			LogDebug(COMPONENT_QOS, "debugdp got the qos share ######:%d",share->share_id);
			if(qos_class_type == QOS_SHARE) {
				print_io_details(share, NULL, sbucket, NULL, op_type, QOS_SHARE, 0, str);
			} else {
				struct QoS_perClient_Class *client = share->clients;
				while (client != NULL) {
					struct qos_token_bucket *cbucket = qos_get_bw_bucket(client, QOS_CLIENT, op_type);
					print_io_details(share, client, sbucket, cbucket, op_type, QOS_PSPC, 0, str);
					client = client->next;
				}
			}
		} else { //else if (qos_class_type == QOS_CLIENT) {
			struct QoS_perClient_Class* client = qos_class;
			struct qos_token_bucket *cbucket = qos_get_bw_bucket(client, QOS_CLIENT, op_type);
			LogDebug(COMPONENT_QOS, "debugdp got the qos client ######:%p",client->client_addr);
			print_io_details(NULL, client, NULL, cbucket, op_type, QOS_CLIENT, 0, str);
		}
	}
}


static inline void pspc_reschedule_bw_io(struct QoS_perShare_Class* share, unsigned int op_type)
{
	if (share != NULL) {
		uint64_t current_time = get_time_in_usec();
		struct QoS_perClient_Class* client = share->clients;
		struct qos_token_bucket *sbucket = qos_get_bw_bucket(share, QOS_PSPC, op_type);
		if (current_time > sbucket->bw_ldct) {
			pthread_mutex_lock(&sbucket->lock);
			while (client != NULL) {
				struct qos_token_bucket *cbucket = qos_get_bw_bucket(client, QOS_CLIENT, op_type);
				pthread_mutex_lock(&cbucket->lock);
				if (current_time >= cbucket->bw_ldct && cbucket->num_ios_waiting != 0) {
					//print_io_details(share, client, sbucket, cbucket, op_type, QOS_PSPC, current_time,"before_reschedule_to_share");
					pspc_rescedule_io_to_share(sbucket, cbucket, current_time);
					//print_io_details(share, client, sbucket, cbucket, op_type, QOS_PSPC, current_time, "after_reschedule_to_share");
				}
				pthread_mutex_unlock(&cbucket->lock);
				client = client->next;
			}
			pthread_mutex_unlock(&sbucket->lock);
		}
	}
}


bool ps_bw_control_cb(struct gsh_export *export, void *state)
{
	struct QoS_perShare_Class *share = get_share_qos(export);
	if (share && share->bw_enabled) {
		print_all_io_details(share, *(unsigned int *)state , QOS_CLIENT, "hell");
		resume_bw_io_ps(share, *(unsigned int *)state);
	}
	return true; // Continue iteration
}
bool pc_bw_control_cb(struct gsh_client *cl, void *state)
{
	struct QoS_perClient_Class *client = get_client_qos(cl);
	if (client && client->bw_enabled) {
		print_all_io_details(client, *(unsigned int *)state , QOS_SHARE, "hell");
		resume_bw_io_pc(client, *(unsigned int *)state);
	}
	return true; // Continue iteration
}
bool pspc_bw_control_cb(struct gsh_export *export, void *state)
{
	struct QoS_perShare_Class *share = get_share_qos(export);
	if (share && share->bw_enabled) {
		//print_all_io_details(share, *(unsigned int *)state , QOS_PSPC, "hell");
		resume_bw_io_pspc(share, *(unsigned int *)state);
		pspc_reschedule_bw_io(share, *(unsigned int *)state);
	}
	return true; // Continue iteration
}

static inline void resume_bw_io(unsigned int op_type)
{
	switch (g_qos_config->qos_type) {
		case QOS_NOT_ENABLED:
			LogFullDebug(COMPONENT_QOS,"QOS not enabled :%d",g_qos_config->qos_type);
			break;
		case QOS_PS_ENABLED:
			foreach_gsh_export(ps_bw_control_cb, false, &op_type);
			break;
		case QOS_PC_ENABLED:
			foreach_gsh_client(pc_bw_control_cb, &op_type);
			break;
		case QOS_PS_PC_ENABLED:
			foreach_gsh_export(pspc_bw_control_cb, false, &op_type);
			break;
		default:
			LogFullDebug(COMPONENT_QOS," Something really wrong:%d",g_qos_config->qos_type);
			break;
	}
}

static void *qos_thread_func(void *arg)
{
	pthread_t current_thread_id = pthread_self();
	int counter =0;
	unsigned int  op_type =  *(unsigned int *) arg;
	LogDebug(COMPONENT_QOS, "debugdp runnning from arg:%d thread.id:%lu ",op_type, current_thread_id);
	while (true) {
		resume_bw_io(op_type);
		/* Currently combined tokenization is enabledi only in write bucket,
		 * once pnfs and nconnect gets properly enabled
		 * need to revisit this condition: "op_type == QOS_WRITE"
		 */
		if (g_qos_config->enable_tokens && counter >= TOKEN_REFRESH_DELAY && op_type == QOS_WRITE) {
			LogDebug(COMPONENT_QOS, "Running periodic task in qos worker thread.id:%lu ",
					current_thread_id);
			refresh_qos_token();
			counter = 0;
		}
		usleep(BW_DELAY_USEC);  // Periodic wake-up
		counter++;
	}
	return NULL;
}

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t qos_thread[2] = {0,0};
int var[2] = {QOS_READ, QOS_WRITE};
static void qos_thread_init(void)
{
	if(g_qos_config->enable_qos == 0)
		return;

	pthread_mutex_lock(&lock);
	if (qos_initalized == 0) {
		qos_initalized = 1;
		for(int i=0; i <2 ; i++)
		{
			pthread_create(&qos_thread[i], NULL, qos_thread_func, &var[i]);
			LogDebug(COMPONENT_QOS, "Qos thread created :%lu", qos_thread[i]);
			pthread_detach(qos_thread[i]);
		}
	}
	pthread_mutex_unlock(&lock);
}

clientid4 get_clientid_from_ip(sockaddr_t *client_ip) {
	struct gsh_client *client;

	client = get_gsh_client(client_ip, true);
	if (client == NULL) {
		return 0;
	}

	return 1;
}
/*
clientid4 get_clientid_from_gsh_client(struct gsh_client *client) {
	struct nfs_client *nfsclient;
	nfsclient = container_of(client->connection_manager.connections.next,
			struct nfs_client, gsh_client);
	return nfsclient->clientid;
}
*/

struct QoS_perClient_Class *get_client_qos(struct gsh_client *client)
{
	struct QoS_perClient_Class *qos_class = NULL;

	if (!client) {
		LogDebug(COMPONENT_QOS, " client is NULL");
		return NULL;
	}

	if (client->qos_class != NULL) {
		qos_class = client->qos_class;
	} else {
		LogDebug(COMPONENT_QOS, " qos_block is null and hoststr is :%s", client->hostaddr_str);
	}

	return qos_class;
}

struct QoS_perShare_Class *get_share_qos(struct gsh_export *export)
{
	struct QoS_perShare_Class *qos_class = NULL;

	if (!export) {
		LogDebug(COMPONENT_QOS, "gsh_export is NULL");
		return NULL;
	}
	if (export->qos_block != NULL) {
		qos_class = export->qos_class;
		if (qos_class == NULL) {
			LogDebug(COMPONENT_QOS, "qos_block is NULL path:%s", export->cfg_fullpath);
			return NULL;
		}
	} else {
		LogDebug(COMPONENT_QOS, "qos_block is NULL path:%s", export->cfg_fullpath);
		return NULL;
	}

	return qos_class;
}

bool set_pspc_bandwidth(sockaddr_t *client_ip, struct gsh_export *export,
		uint32_t read_bw, uint32_t write_bw)
{
	struct QoS_perShare_Class *s_qos_class;
	struct QoS_perClient_Class *c_qos_class;
	struct gsh_client *client = NULL;
	client = get_gsh_client(client_ip, true);

	if (!export || !client)
		return false;

	s_qos_class = get_share_qos(export);
	c_qos_class = pspc_get_client_from_list(s_qos_class->clients, &client->cl_addrbuf);

	if (c_qos_class == NULL)
		return false;

	PTHREAD_MUTEX_lock(&c_qos_class->lock);
	c_qos_class->read_bucket.max_bw_allowed = read_bw;
	c_qos_class->write_bucket.max_bw_allowed = write_bw;
	PTHREAD_MUTEX_unlock(&c_qos_class->lock);

	return true;
}

bool get_pspc_bandwidth(sockaddr_t *client_ip, struct gsh_export *export,
		uint32_t *read_bw, uint32_t *write_bw)
{
	struct QoS_perShare_Class *s_qos_class;
	struct QoS_perClient_Class *c_qos_class;
	struct gsh_client *client = NULL;
	client = get_gsh_client(client_ip, true);

	if (!export || !client)
		return false;

	s_qos_class = get_share_qos(export);
	c_qos_class = pspc_get_client_from_list(s_qos_class->clients, &client->cl_addrbuf);

	if (c_qos_class == NULL)
		return false;

	PTHREAD_MUTEX_lock(&c_qos_class->lock);
	*read_bw = c_qos_class->read_bucket.max_bw_allowed;
	*write_bw = c_qos_class->write_bucket.max_bw_allowed;
	PTHREAD_MUTEX_unlock(&c_qos_class->lock);

	return true;
}

bool set_pspc_tokens(sockaddr_t *client_ip, struct gsh_export *export,
		uint32_t *max_tokens, uint32_t *token_renewal)
{
	struct QoS_perShare_Class *s_qos_class;
	struct QoS_perClient_Class *c_qos_class;
	struct gsh_client *client = NULL;
	client = get_gsh_client(client_ip, true);

	if (!export || !client)
		return false;

	s_qos_class = get_share_qos(export);
	c_qos_class = pspc_get_client_from_list(s_qos_class->clients, &client->cl_addrbuf);

	if (c_qos_class == NULL)
		return false;

	PTHREAD_MUTEX_lock(&c_qos_class->lock);
	c_qos_class->read_bucket.max_available_tokens = *max_tokens;
	c_qos_class->write_bucket.max_available_tokens = *max_tokens;
	c_qos_class->read_bucket.tokens_renew_time = *token_renewal;
	c_qos_class->write_bucket.tokens_renew_time = *token_renewal;
	PTHREAD_MUTEX_unlock(&c_qos_class->lock);

	return true;
}

bool get_pspc_tokens(sockaddr_t *client_ip, struct gsh_export *export,
		uint32_t max_tokens, uint32_t token_renewal)
{
	struct QoS_perShare_Class *s_qos_class;
	struct QoS_perClient_Class *c_qos_class;
	struct gsh_client *client = NULL;
	client = get_gsh_client(client_ip, true);

	if (!export || !client)
		return false;

	s_qos_class = get_share_qos(export);
	c_qos_class = pspc_get_client_from_list(s_qos_class->clients, &client->cl_addrbuf);

	if (c_qos_class == NULL)
		return false;

	PTHREAD_MUTEX_lock(&c_qos_class->lock);
	max_tokens = c_qos_class->read_bucket.max_available_tokens;
	token_renewal = c_qos_class->read_bucket.tokens_renew_time;
	PTHREAD_MUTEX_unlock(&c_qos_class->lock);

	return true;
}

uint32_t get_share_client_count(struct QoS_perShare_Class *s_qos_class) {
	uint32_t count = 0;
	if (s_qos_class ==  NULL)
		return count;

	struct QoS_perClient_Class *c_qos_class = s_qos_class->clients;
	while (c_qos_class) {
		count++;
		c_qos_class = c_qos_class->next;
	}

	return count;
}
