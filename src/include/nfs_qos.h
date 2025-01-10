#include <time.h>

#define IS_QOS_IO      1 << 0
#define IS_QOS_IO_READ_BYPASS  1<<1

#define  NON_RATELIMITING_IO	0
#define  RATELIMITING_IO	1


struct qos_op_cb_arg {
	void *caller_data;
	uint32_t ratecontrol;
};
// Timer entry structure
typedef struct timer_entry {
	uint64_t expiry;              // Expiry time
	void (*callback)(void *);   // Callback function to call when the timer expires
	void *args;                 // Arguments for the callback
	uint64_t size;
	struct timer_entry *next;   // Pointer to the next entry in the list
} timer_entry_t;

enum qos_operation_type {
	QOS_READ,
	QOS_WRITE
};

enum qos_class_type {
	QOS_SHARE,
	QOS_CLIENT,
	QOS_PSPC
};

struct qos_token_bucket {
	pthread_mutex_t lock; /* IO path lock while adding/updating the structure entries */
	timer_entry_t *io_waitlist_qos_bc; /*   Used for BW conrtol, io wait queue */
	uint32_t num_ios_waiting;
	uint64_t max_bw_allowed;
	uint64_t bw_ldct; /*   Used to control BW, Last Data Consumed time* */
	uint64_t max_available_tokens;
	uint64_t tokens_consumed;
	uint64_t tokens_renew_time; /*   In useconds */
	uint64_t last_tokens_consumed_time;
};

struct qos_client_entry {
	clientid4 clientid; // data->preserved_client->clientid4
	sockaddr_t *client_addr; /*socket address should be valid till gsh_remove_client is not called */
	unsigned int num_ios_waiting;
	timer_entry_t *io_waitlist_qos;
	SVCXPRT *rq_xprt; // should we save svc_req ? data-> req->rq_xprt
	bool epoll_disabled;
	compound_data_t *data;
	struct qos_client_entry *next;
};

struct QoS_perClient_Class
{
	sockaddr_t *client_addr; /*socket address should be valid till gsh_remove_client is not called */
	unsigned int num_ios_waiting;/*  getting used temp , will remove in future */
	bool combined_rw_bw_control;
	bool combined_rw_token_control;
	bool bw_enabled;
	bool token_enabled;
	struct qos_token_bucket read_bucket;
	struct qos_token_bucket write_bucket;
	pthread_mutex_t lock;
	struct qos_client_entry *client_entries;  /*  this entry is used to store the IO's after token exausted  */
	struct QoS_perClient_Class *next;
	//struct QoS_perShare_Class *parent;
};

struct QoS_perShare_Class
{
	unsigned int share_id;
	unsigned int num_ios_waiting; /*   this should be used by clients waiting on token refresh  */
	pthread_mutex_t lock; /*  lock used for adding/removing clients, client_entries etc */
	struct qos_token_bucket read_bucket;
	struct qos_token_bucket write_bucket;
	bool combined_rw_bw_control;
	bool combined_rw_token_control;
	bool bw_enabled;
	bool token_enabled;
	uint64_t max_client_wbw;
	uint64_t max_client_rbw;
	struct qos_client_entry *client_entries; /*   this entry is used to store the IO's after token exausted */
	struct QoS_perClient_Class *clients;
};
struct qos_block_config {
	bool enable_qos;
	bool enable_tokens;
	bool enable_bw_control;
	bool combined_rw_bw_control;
        bool combined_rw_token_control;
	int qos_type;
	uint64_t max_export_write_bw;
	uint64_t max_export_read_bw;
	uint64_t max_client_write_bw;
	uint64_t max_client_read_bw;

	uint64_t max_export_read_tokens;
	uint64_t max_export_write_tokens;
	uint64_t max_client_read_tokens;
	uint64_t max_client_write_tokens;
	uint64_t export_read_tokens_renew_time;
	uint64_t export_write_tokens_renew_time;
	uint64_t client_read_tokens_renew_time;
	uint64_t client_write_tokens_renew_time;
//	struct QoS_perShare_Class *qos_class;
};
/* Structured for  Future Generic Class implementation
struct Qos_Class
{
	enum qos_entity_type type;
	union  {
		uint64_t clientid;
		uint64_t shareid;
	}
	pthread_mutex_t lock;
	unsigned int num_ios_waiting;
	struct qos_token_bucket read_bucket;
	struct qos_token_bucket write_bucket;
	struct Qos_Class *subclass;
	struct Qos_Class *next;
}
*/


void QoS_perShareInsert(struct gsh_export *export, struct qos_block_config *qos_block);
void qos_free_mem(void *gsh_ptr, unsigned int qos_class_type);
void qos_free_mem_session(void);
unsigned int QoS_Process(unsigned int size, void *caller_data, compound_data_t *data, unsigned int op_type);
