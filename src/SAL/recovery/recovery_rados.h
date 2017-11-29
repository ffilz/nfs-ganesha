#ifndef _RECOVERY_RADOS_H
#define _RECOVERY_RADOS_H
/*
 * Select bits from recovery_rados_kv.c that we can reuse elsewhere
 */
extern rados_t		rados_recov_cluster;
extern rados_ioctx_t	rados_recov_io_ctx;
extern char		rados_recov_oid[NI_MAXHOST];

struct rados_kv_parameter {
	/** Connection to ceph cluster */
	char *ceph_conf;
	/** User ID to ceph cluster */
	char *userid;
	/** Pool for client info */
	char *pool;
};
extern struct rados_kv_parameter rados_kv_param;

typedef void (*pop_clid_entry_t)(char *, char*, add_clid_entry_hook,
				 add_rfh_entry_hook, bool old, bool takeover);
typedef struct pop_args {
	add_clid_entry_hook add_clid_entry;
	add_rfh_entry_hook add_rfh_entry;
	bool old;
	bool takeover;
} *pop_args_t;

int rados_kv_get(char *key, char **val, size_t *val_len, char *object);
void rados_kv_create_key(nfs_client_id_t *clientid, char *key);
void rados_kv_create_val(nfs_client_id_t *clientid, char *val);
int rados_kv_traverse(pop_clid_entry_t pop_func, pop_args_t pop_args,
			const char *object);
void rados_kv_append_val_rdfh(char *val, char *rdfh, int rdfh_len);
#endif	/* _RECOVERY_RADOS_H */
