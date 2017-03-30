#include "config.h"
#include "log.h"
#include "nfs_core.h"
#include "nfs4.h"
#include "sal_functions.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#include "bsd-base64.h"
#include "client_mgr.h"
#include "fsal.h"
#include "netdb.h"
#include <rados/librados.h>

#define OLD_PREFIX		"old"
#define RECOV_PREFIX		"recov"

#define CL_KEY_FMT		"%s-%s-%lu"  /* nodeid-old/recov-${cid} */

#define CMD_PREFIX_MAX_LEN	64
#define KEY_MAX_LEN		NAME_MAX
#define VAL_MAX_LEN		PATH_MAX

#define RADOS_CMD_MAX_LEN	(VAL_MAX_LEN + \
				KEY_MAX_LEN + \
				CMD_PREFIX_MAX_LEN)  /* val + key + prefix */

#define RADOS_CMD_PUT_FMT	"{ \"prefix\" : \"%s\", \"key\" : \"%s\", \"val\" : \"%s\" }"

#define RADOS_CMD_GET_FMT	"{ \"prefix\" : \"%s\",	\"key\" : \"%s\" }"

#define RADOS_CMD_DEL_FMT	"{ \"prefix\" : \"%s\",	\"key\" : \"%s\" }"

#define RADOS_CMD_LST_FMT	"{ \"prefix\" : \"%s\" }"

#define CMD_GET			"config-key get"
#define CMD_PUT			"config-key put"
#define CMD_DEL			"config-key del"
#define CMD_LST			"config-key list"

static rados_t cluster;
static char rados_conf[PATH_MAX];
static char user_id[NAME_MAX];
static bool clustered;
static char myhostname[NI_MAXHOST];

static int convert_opaque_val(struct display_buffer *dspbuf,
			      void *value,
			      int len,
			      int max)
{
	unsigned int i = 0;
	int b_left = display_start(dspbuf);
	int cpy = len;

	if (b_left <= 0)
		return 0;

	/* Check that the length is ok
	 * If the value is empty, display EMPTY value. */
	if (len <= 0 || len > max)
		return 0;

	/* If the value is NULL, display NULL value. */
	if (value == NULL)
		return 0;

	/* Determine if the value is entirely printable characters, */
	/* and it contains no slash character (reserved for filename) */
	for (i = 0; i < len; i++)
		if ((!isprint(((char *)value)[i])) ||
		    (((char *)value)[i] == '/'))
			break;

	if (i == len) {
		/* Entirely printable character, so we will just copy the
		 * characters into the buffer (to the extent there is room
		 * for them).
		 */
		b_left = display_len_cat(dspbuf, value, cpy);
	} else {
		b_left = display_opaque_bytes(dspbuf, value, cpy);
	}

	if (b_left <= 0)
		return 0;

	return b_left;
}

static char *next_key(const char *cur, char *next)
{
	char *p, *q;
	size_t len;

	p = strchr(cur, '"');
	if (!p)
		return NULL;

	q = strchr(p + 1, '"');
	if (!q)
		return NULL;

	assert(q != p);

	len = (size_t)(q - p);
	strncpy(next, p + 1, len - 1);
	*(next + len - 1) = '\0';

	return q + 1;
}

static bool is_end_key(const char *key)
{
	if (!key)
		return true;
	if (!strncmp(key, "]", 1))
		return true;
	return false;
}

static bool is_valid_key(const char *key)
{
	char *dup_key = gsh_strdup(key);
	char *nodeid, *prefix, *cid;
	char *endptr;
	bool valid = true;

	nodeid = strtok(dup_key, "-");
	if (!nodeid) {
		valid = false;
		goto out;
	}

	if (clustered) {
		strtol(nodeid, &endptr, 10);
		if (endptr[0] != '\0') {
			valid = false;
			goto out;
		}
	}

	prefix = strtok(NULL, "-");
	if (!prefix) {
		valid = false;
		goto out;
	}

	if (strcmp(prefix, OLD_PREFIX) && strcmp(prefix, RECOV_PREFIX)) {
		valid = false;
		goto out;
	}

	cid = strtok(NULL, "-");
	if (!cid) {
		valid = false;
		goto out;
	}

	strtoul(cid, &endptr, 10);
	if (endptr[0] != '\0') {
		valid = false;
	}
out:
	if (!valid)
		LogWarn(COMPONENT_CLIENTID, "Invalid key: %s", key);
	free(dup_key);
	return valid;
}

static bool is_old_key(const char *key)
{
	if (strstr(key, OLD_PREFIX))
		return true;
	return false;
}

static bool is_own_key(const char *key)
{
	char *nodeid;
	char *dup_key = gsh_strdup(key);
	bool is_own = false;

	nodeid = strtok(dup_key, "-");

	if (clustered) {
		if (g_nodeid == atoi(nodeid))
			is_own = true;
	} else {
		if (!strcmp(nodeid, myhostname))
			is_own = true;
	}

	gsh_free(dup_key);
	return is_own;
}

static bool is_takeover_key(const char *key, const char *id)
{
	if (strstr(key, id))
		return true;
	return false;
}

static void recov_to_old_key(const char *recov_key, char *old_key)
{
	char *cid;
	char nodeid[NI_MAXHOST];
	char *dup_key = gsh_strdup(recov_key);

	strtok(dup_key, "-");
	strtok(NULL, "-");
	cid = strtok(NULL, "-");

	if (clustered) {
		snprintf(nodeid, NI_MAXHOST, "%d", g_nodeid);
	} else {
		snprintf(nodeid, NI_MAXHOST, "%s", myhostname);
	}
	snprintf(old_key, KEY_MAX_LEN, CL_KEY_FMT,
		 nodeid, OLD_PREFIX, strtoul(cid, NULL, 10));

	gsh_free(dup_key);
}

static void create_key(nfs_client_id_t *clientid, char *prefix, char *key)
{
	char nodeid[NI_MAXHOST];

	if (clustered) {
		snprintf(nodeid, NI_MAXHOST, "%d", g_nodeid);
	} else {
		snprintf(nodeid, NI_MAXHOST, "%s", myhostname);
	}
	snprintf(key, KEY_MAX_LEN, CL_KEY_FMT, nodeid, prefix,
		 (uint64_t)clientid->cid_clientid);
}

static void create_val(nfs_client_id_t *clientid, char *val)
{
	char *src = clientid->cid_client_record->cr_client_val;
	int src_len = clientid->cid_client_record->cr_client_val_len;
	const char *str_client_addr = "(unknown)";
	char cidstr[PATH_MAX] = { 0, };
	struct display_buffer dspbuf = {sizeof(cidstr), cidstr, cidstr};
	char cidstr_len[20];
	int total_len;
	int ret;

	/* get the caller's IP addr */
	if (clientid->gsh_client != NULL)
		str_client_addr = clientid->gsh_client->hostaddr_str;

	ret = convert_opaque_val(&dspbuf, src, src_len, PATH_MAX);
	assert(ret > 0);

	snprintf(cidstr_len, sizeof(cidstr_len), "%zd", strlen(cidstr));
	total_len = strlen(cidstr) + strlen(str_client_addr) + 5 +
		    strlen(cidstr_len);

	/* hold both long form clientid and IP */
	snprintf(val, total_len, "%s-(%s:%s)",
		 str_client_addr, cidstr_len, cidstr);

	LogDebug(COMPONENT_CLIENTID, "Created client name [%s]",
		 clientid->cid_recov_tag);
}

static void create_cmd(char *cmd, const char *fmt, const char *op,
		       const char *key, const char *val)
{
	if (val == NULL) {
		if (key == NULL) {
			snprintf(cmd, RADOS_CMD_MAX_LEN, fmt, op);
		} else {
			snprintf(cmd, RADOS_CMD_MAX_LEN, fmt, op, key);
		}
	} else {
		snprintf(cmd, RADOS_CMD_MAX_LEN, fmt, op, key, val);
	}
}

static int rados_kv_put(const char *key, const char *val)
{
	char *cmd[2];
	int ret;

	cmd[1] = NULL;
	cmd[0] = gsh_malloc(RADOS_CMD_MAX_LEN);

	create_cmd(cmd[0], RADOS_CMD_PUT_FMT, CMD_PUT, key, val);

	ret = rados_mon_command(cluster, (const char **)cmd, 1,
				"", 0, NULL, 0, NULL, 0);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to put kv ret=%d, key=%s, val=%s",
				ret, key, val);
	}

	gsh_free(cmd[0]);
	return ret;
}

static int rados_kv_get(const char *key, char **val, size_t *val_len)
{
	char *cmd[2];
	char *outbuf;
	size_t outlen;
	int ret;

	cmd[1] = NULL;
	cmd[0] = gsh_malloc(RADOS_CMD_MAX_LEN);

	create_cmd(cmd[0], RADOS_CMD_GET_FMT, CMD_GET, key, NULL);

	ret = rados_mon_command(cluster, (const char **)cmd, 1,
				"", 0, &outbuf, &outlen, NULL, 0);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to get kv ret=%d, key=%s",
				ret, key);
		goto out;
	}

	outbuf[outlen] = '\0';
	*val = outbuf;
	*val_len = outlen;
out:
	gsh_free(cmd[0]);
	return ret;
}

static int rados_kv_del(const char *key)
{
	char *cmd[2];
	int ret;

	cmd[1] = NULL;
	cmd[0] = gsh_malloc(RADOS_CMD_MAX_LEN);

	create_cmd(cmd[0], RADOS_CMD_DEL_FMT, CMD_DEL, key, NULL);

	ret = rados_mon_command(cluster, (const char **)cmd, 1,
				"", 0, NULL, 0, NULL, 0);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to del kv ret=%d, key=%s",
				ret, key);
	}

	gsh_free(cmd[0]);
	return ret;
}

static int rados_kv_lst(char **val, size_t *val_len)
{
	char *cmd[2];
	char *outbuf;
	size_t outlen;
	int ret;

	cmd[1] = NULL;
	cmd[0] = gsh_malloc(RADOS_CMD_MAX_LEN);

	create_cmd(cmd[0], RADOS_CMD_LST_FMT, CMD_LST, NULL, NULL);

	ret = rados_mon_command(cluster, (const char **)cmd, 1,
				"", 0, &outbuf, &outlen, NULL, 0);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to lst kv ret=%d", ret);
		goto out;
	}

	outbuf[outlen] = '\0';
	*val = outbuf;
	*val_len = outlen;
out:
	gsh_free(cmd[0]);
	return ret;
}

static void append_val_rdfh(char *val, char *rdfh, int rdfh_len)
{
	char rdfhstr[NAME_MAX];
	int rdfhstr_len;
	int ret;

	/* Convert nfs_fh4_val into base64 encoded string */
	ret = base64url_encode(rdfh, rdfh_len, rdfhstr, NAME_MAX);
	assert(ret != -1);
	rdfhstr_len = strlen(rdfhstr);

	strncat(val, "#", 1);
	strncat(val, rdfhstr, rdfhstr_len);
	val[rdfhstr_len + 1] = '\0';
}

void rados_kv_init(void)
{
	int ret;

	snprintf(user_id, NAME_MAX, "%s", "admin");
	snprintf(rados_conf, PATH_MAX, "%s", "/etc/ceph/ceph.conf");
	clustered = nfs_param.core_param.clustered;
	if (!clustered) {
		ret = gethostname(myhostname, sizeof(myhostname));
		if (ret) {
			LogEvent(COMPONENT_CLIENTID, "Failed to gethostname");
			return;
		}
	}

	ret = rados_create(&cluster, user_id);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to rados create");
		return;
	}
	ret = rados_conf_read_file(cluster, rados_conf);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to read conf");
		rados_shutdown(cluster);
		return;
	}
	ret = rados_connect(cluster);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to connect to cluster");
		rados_shutdown(cluster);
		return;
	}

	LogEvent(COMPONENT_CLIENTID, "Rados kv store init done");
}

void rados_kv_add_clid(nfs_client_id_t *clientid)
{
	char ckey[KEY_MAX_LEN];
	char *cval;
	int ret;

	cval = gsh_malloc(VAL_MAX_LEN);

	create_key(clientid, RECOV_PREFIX, ckey);
	create_val(clientid, cval);

	ret = rados_kv_put(ckey, cval);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to add clid %lu",
			 clientid->cid_clientid);
		goto out;
	}

	clientid->cid_recov_tag = gsh_malloc(strlen(cval) + 1);
	strncpy(clientid->cid_recov_tag, cval, strlen(cval) + 1);
out:
	gsh_free(cval);
}

void rados_kv_rm_clid(nfs_client_id_t *clientid)
{
	char ckey[KEY_MAX_LEN];
	int ret;

	create_key(clientid, RECOV_PREFIX, ckey);

	ret = rados_kv_del(ckey);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to del clid %lu",
			 clientid->cid_clientid);
		return;
	}

	free(clientid->cid_recov_tag);
	clientid->cid_recov_tag = NULL;
}

static void rados_kv_pop_clid_entry(char *key,
				    add_clid_entry_hook add_clid_entry,
				    add_rfh_entry_hook add_rfh_entry,
				    bool old,
				    bool takeover)
{
	int ret;
	char old_key[KEY_MAX_LEN];
	char *outbuf, *dupbuf;
	size_t outlen;
	char *cl_name, *rfh_names, *rfh_name;
	clid_entry_t *clid_ent;

	ret = rados_kv_get(key, &outbuf, &outlen);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to pop clid entry for %s", key);
		return;
	}

	/* extract clid records */
	dupbuf = gsh_strdup(outbuf);
	cl_name = strtok(dupbuf, "#");
	if (!cl_name)
		cl_name = dupbuf;
	clid_ent = add_clid_entry(cl_name);

	rfh_names = strtok(NULL, "#");
	rfh_name = strtok(rfh_names, "#");
	while (rfh_name) {
		add_rfh_entry(clid_ent, rfh_name);
		rfh_name = strtok(NULL, "#");
	}
	gsh_free(dupbuf);

	if (!old) {
		recov_to_old_key(key, old_key);
		ret = rados_kv_put(old_key, outbuf);
		if (ret) {
			LogEvent(COMPONENT_CLIENTID,
					"Failed to move %s to %s",
					key, old_key);
			goto out;
		}
	}

	if (!takeover) {
		ret = rados_kv_del(key);
		if (ret) {
			LogEvent(COMPONENT_CLIENTID,
					"Failed to del %s", key);
			goto out;
		}
	}

out:
	gsh_free(outbuf);
}

void rados_kv_read_recov_clids_recover(add_clid_entry_hook add_clid_entry,
				       add_rfh_entry_hook add_rfh_entry)
{
	int ret;
	char ckey[KEY_MAX_LEN];
	char *outbuf, *cur;
	size_t outlen;

	ret = rados_kv_lst(&outbuf, &outlen);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to lst clids");
		return;
	}

	cur = next_key(outbuf, ckey);
	while (!is_end_key(cur)) {
		if (!is_valid_key(ckey))
			goto next;

		if (!is_own_key(ckey))
			goto next;

		rados_kv_pop_clid_entry(ckey, add_clid_entry,
					add_rfh_entry,
					is_old_key(ckey),
					false);
next:
		cur = next_key(cur, ckey);
	}

	gsh_free(outbuf);
}

void rados_kv_read_recov_clids_takeover(nfs_grace_start_t *gsp,
					add_clid_entry_hook add_clid_entry,
					add_rfh_entry_hook add_rfh_entry)
{
	int ret;
	char ckey[KEY_MAX_LEN];
	char *outbuf, *cur;
	size_t outlen;

	ret = rados_kv_lst(&outbuf, &outlen);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to lst clids");
		return;
	}

	cur = next_key(outbuf, ckey);
	while (!is_end_key(cur)) {
		if (!is_valid_key(ckey))
			goto next;

		if (is_old_key(ckey))
			goto next;

		if (!is_takeover_key(ckey, gsp->ipaddr))
			goto next;

		rados_kv_pop_clid_entry(ckey, add_clid_entry,
					add_rfh_entry,
					false,
					true);
next:
		cur = next_key(cur, ckey);
	}

	gsh_free(outbuf);
}


void rados_kv_cleanup_old(void)
{
	int ret;
	char ckey[KEY_MAX_LEN];
	char *outbuf, *cur;
	size_t outlen;

	ret = rados_kv_lst(&outbuf, &outlen);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to lst clids");
		return;
	}

	cur = next_key(outbuf, ckey);
	while (!is_end_key(cur)) {
		if (!is_valid_key(ckey))
			goto next;

		if (!is_old_key(ckey))
			goto next;

		if (!is_own_key(ckey))
			goto next;

		ret = rados_kv_del(ckey);
		if (ret) {
			LogEvent(COMPONENT_CLIENTID, "Failed to del %s", ckey);
		}
next:
		cur = next_key(cur, ckey);
	}

	gsh_free(outbuf);
}

void rados_kv_add_revoke_fh(nfs_client_id_t *delr_clid, nfs_fh4 *delr_handle)
{
	int ret;
	char ckey[KEY_MAX_LEN];
	char *cval;
	char *outbuf;
	size_t outlen;

	cval = gsh_malloc(VAL_MAX_LEN);

	create_key(delr_clid, RECOV_PREFIX, ckey);
	ret = rados_kv_get(ckey, &outbuf, &outlen);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID, "Failed to get %s", ckey);
		goto out;
	}

	strncpy(cval, outbuf, outlen);
	append_val_rdfh(cval, delr_handle->nfs_fh4_val,
			      delr_handle->nfs_fh4_len);

	ret = rados_kv_put(ckey, cval);
	if (ret) {
		LogEvent(COMPONENT_CLIENTID,
				"Failed to add rdfh for clid %lu",
				delr_clid->cid_clientid);
	}

	gsh_free(outbuf);
out:
	gsh_free(cval);
}

bool rados_kv_check_clid(nfs_client_id_t *clientid, clid_entry_t *clid_ent)
{
	LogDebug(COMPONENT_CLIENTID, "compare %s to %s",
		 clientid->cid_recov_tag, clid_ent->cl_name);

	if (!clientid->cid_recov_tag)
		return false;
	if (!strncmp(clientid->cid_recov_tag,
		     clid_ent->cl_name, PATH_MAX))
		return true;
	return false;
}

struct nfs4_recovery_backend_t rados_kv_backend = {
	.recovery_init = rados_kv_init,
	.recovery_cleanup = rados_kv_cleanup_old,
	.recovery_read_clids_recover = rados_kv_read_recov_clids_recover,
	.recovery_read_clids_takeover = rados_kv_read_recov_clids_takeover,
	.add_clid = rados_kv_add_clid,
	.rm_clid = rados_kv_rm_clid,
	.add_revoke_fh = rados_kv_add_revoke_fh,
	.check_clid = rados_kv_check_clid,
};

void rados_kv_backend_init(struct nfs4_recovery_backend_t **backend)
{
	*backend = &rados_kv_backend;
}
