#include "nfs_core.h"
#include "nfs_qos.h"
#include "nfs_qosmgr.h"
#include "gsh_dbus.h"
char *errormsg = "EINVAL";
/*  Bandwidth Control Methods */
extern struct gsh_client *lookup_client(DBusMessageIter *args, char **errormsg);

static bool dbus_qos_client_bw_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t read_bw, write_bw;
	struct QoS_perClient_Class *client_qos;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }

	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &read_bw);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &write_bw);

	client_qos = get_client_qos(&client_ip->cl_addrbuf);
	if (client_qos) {
		PTHREAD_MUTEX_lock(&client_qos->lock);
		client_qos->read_bucket.max_bw_allowed = read_bw;
		client_qos->write_bucket.max_bw_allowed = write_bw;
		PTHREAD_MUTEX_unlock(&client_qos->lock);
		return true;
	}
	return false;
}

/*  Token Control Methods */
static bool dbus_qos_client_token_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t max_tokens;
	uint64_t token_renewal;
	struct QoS_perClient_Class *client_qos;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }

	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &max_tokens);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &token_renewal);

	client_qos = get_client_qos(&client_ip->cl_addrbuf);
	if (client_qos) {
		PTHREAD_MUTEX_lock(&client_qos->lock);
		client_qos->read_bucket.max_available_tokens = max_tokens;
		client_qos->write_bucket.max_available_tokens = max_tokens;
		client_qos->read_bucket.tokens_renew_time = token_renewal;
		client_qos->write_bucket.tokens_renew_time = token_renewal;
		PTHREAD_MUTEX_unlock(&client_qos->lock);
		return true;
	}
	return false;
}

static bool dbus_qos_client_token_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	struct QoS_perClient_Class *client_qos;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }

	client_qos = get_client_qos(&client_ip->cl_addrbuf);
	if (client_qos) {
		PTHREAD_MUTEX_lock(&client_qos->lock);
		uint32_t max_tokens = client_qos->read_bucket.max_available_tokens;
		uint64_t token_renewal = client_qos->read_bucket.tokens_renew_time;
		PTHREAD_MUTEX_unlock(&client_qos->lock);

		dbus_message_iter_init_append(reply, args);
		dbus_message_iter_append_basic(args, DBUS_TYPE_UINT32, &max_tokens);
		dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &token_renewal);
		return true;
	}
	return false;
}
static bool dbus_qos_client_bw_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	struct QoS_perClient_Class *client_qos;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }
	client_qos = get_client_qos(&client_ip->cl_addrbuf);

	if (client_qos) {
		PTHREAD_MUTEX_lock(&client_qos->lock);
		uint64_t read_bw = client_qos->read_bucket.max_bw_allowed;
		uint64_t write_bw = client_qos->write_bucket.max_bw_allowed;
		PTHREAD_MUTEX_unlock(&client_qos->lock);

		dbus_message_iter_init_append(reply, args);
		dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &read_bw);
		dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &write_bw);
		return true;
	}
	return false;
}
	/*  Share Bandwidth Control Methods */
static bool dbus_qos_share_bw_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	uint32_t read_bw, write_bw;
	struct QoS_perShare_Class *share_qos;
	struct gsh_export *export = NULL;
	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}
	//dbus_message_iter_get_basic(args, &export_path);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &read_bw);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &write_bw);

	share_qos = get_share_qos(export);
	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		share_qos->read_bucket.max_bw_allowed = read_bw;
		share_qos->write_bucket.max_bw_allowed = write_bw;
		PTHREAD_MUTEX_unlock(&share_qos->lock);
		if (export != NULL)
			put_gsh_export(export);
		return true;
	}
	if (export != NULL)
		put_gsh_export(export);
	return false;
}

static bool dbus_qos_share_bw_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	uint32_t read_bw = 0, write_bw = 0;
	struct QoS_perShare_Class *share_qos;
	struct gsh_export *export = NULL;
	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

	//dbus_message_iter_get_basic(args, &export_path);
	share_qos = get_share_qos(export);
	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		read_bw = share_qos->read_bucket.max_bw_allowed;
		write_bw = share_qos->write_bucket.max_bw_allowed;
		PTHREAD_MUTEX_unlock(&share_qos->lock);
	}

	dbus_message_iter_init_append(reply, args);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &read_bw);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &write_bw);
	if (export != NULL)
		put_gsh_export(export);
	return true;
}

/*  Share Token Control Methods */
static bool dbus_qos_share_token_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	uint32_t max_tokens;
	uint64_t token_renewal;
	struct QoS_perShare_Class *share_qos;

	struct gsh_export *export = NULL;
	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}
	//dbus_message_iter_get_basic(args, &export_path);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &max_tokens);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &token_renewal);

	share_qos = get_share_qos(export);
	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		share_qos->read_bucket.max_available_tokens = max_tokens;
		share_qos->write_bucket.max_available_tokens = max_tokens;
		share_qos->read_bucket.tokens_renew_time = token_renewal;
		share_qos->write_bucket.tokens_renew_time = token_renewal;
		PTHREAD_MUTEX_unlock(&share_qos->lock);
		return true;
	}
	if (export != NULL)
		put_gsh_export(export);
	return false;
}

static bool dbus_qos_share_token_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	uint32_t max_tokens = 0;
	uint64_t token_renewal = 0;
	struct QoS_perShare_Class *share_qos;
	struct gsh_export *export = NULL;
	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

	//dbus_message_iter_get_basic(args, &export_path);
	share_qos = get_share_qos(export);
	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		max_tokens = share_qos->read_bucket.max_available_tokens;
		token_renewal = share_qos->read_bucket.tokens_renew_time;
		PTHREAD_MUTEX_unlock(&share_qos->lock);
	}

	dbus_message_iter_init_append(reply, args);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT32, &max_tokens);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &token_renewal);
	if (export != NULL)
		put_gsh_export(export);
	return true;
}

/*  Client-Share Bandwidth Control Methods */
static bool dbus_qos_pspc_bw_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t read_bw, write_bw;
	struct gsh_export *export = NULL;
	uint16_t export_id;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

	//dbus_message_iter_get_basic(args, &export_path);

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &read_bw);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &write_bw);

	if (export != NULL)
		put_gsh_export(export);
	return set_pspc_bandwidth(&client_ip->cl_addrbuf, export, read_bw, write_bw);
}

static bool dbus_qos_pspc_bw_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t read_bw = 0, write_bw = 0;

	struct gsh_export *export = NULL;
	uint16_t export_id;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }

	//dbus_message_iter_get_basic(args, &export_path);
	get_pspc_bandwidth(&client_ip->cl_addrbuf, export, &read_bw, &write_bw);

	dbus_message_iter_init_append(reply, args);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &read_bw);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &write_bw);
	if (export != NULL)
		put_gsh_export(export);
	return true;
}

/*  Client-Share Token Control Methods */
static bool dbus_qos_pspc_token_set(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t max_tokens;
	uint64_t token_renewal;
	struct gsh_export *export = NULL;
	uint16_t export_id;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}


        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }
	//dbus_message_iter_get_basic(args, &export_path);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &max_tokens);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &token_renewal);
	if (export != NULL)
		put_gsh_export(export);

	return set_pspc_tokens(&client_ip->cl_addrbuf, export, max_tokens, token_renewal);
}

static bool dbus_qos_pspc_token_get(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	struct gsh_client *client_ip;
	uint32_t max_tokens = 0;
	uint64_t token_renewal = 0;
	struct gsh_export *export = NULL;
        DBusMessageIter iter;
        bool success = true;
        char *errormsg = "OK";

	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

        dbus_message_iter_init_append(reply, &iter);
        client_ip = lookup_client(args, &errormsg);
        if (client_ip == NULL) {
                success = false;
                errormsg = "Client IP address not found";
		gsh_dbus_status_reply(&iter, success, errormsg);
		return true;
        }

	//dbus_message_iter_get_basic(args, &export_path);

	get_pspc_tokens(&client_ip->cl_addrbuf, export, &max_tokens, (uint32_t *)&token_renewal);

	dbus_message_iter_init_append(reply, args);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT32, &max_tokens);
	dbus_message_iter_append_basic(args, DBUS_TYPE_UINT64, &token_renewal);
	if (export != NULL)
		put_gsh_export(export);
	return true;
}

/*  Share clients bandwidth listing method with pagination */
static bool dbus_qos_share_clients_bw_list(DBusMessageIter *args,
		DBusMessage *reply,
		DBusError *error)
{
	uint32_t offset, limit;
	struct QoS_perShare_Class *share_qos;
	struct QoS_perClient_Class *client_qos;
	DBusMessageIter iter, array_iter;
	uint32_t count = 0, total_clients = 0;
	struct gsh_export *export = NULL;

	uint16_t export_id;
	dbus_message_iter_get_basic(args, &export_id);
	export = get_gsh_export(export_id);
	if (export == NULL) {
		LogDebug(COMPONENT_EXPORT, "lookup_export failed with %s",
			 errormsg);
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
			       "lookup_export failed with %s", errormsg);
		return false;
	}

	//dbus_message_iter_get_basic(args, &export_path);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &offset);
	dbus_message_iter_next(args);
	dbus_message_iter_get_basic(args, &limit);

	share_qos = get_share_qos(export);
	dbus_message_iter_init_append(reply, &iter);

	/*  First append total number of clients */
	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		total_clients = get_share_client_count(share_qos);
		PTHREAD_MUTEX_unlock(&share_qos->lock);
	}
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &total_clients);

	/*  Open array container for client data */
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sttuu)", &array_iter);

	if (share_qos) {
		PTHREAD_MUTEX_lock(&share_qos->lock);
		client_qos = share_qos->clients;

		/*  Skip to offset */
		while (client_qos && count < offset) {
			client_qos = client_qos->next;
			count++;
		}

		/*  Add requested number of entries */
		count = 0;
		while (client_qos && count < limit) {
			DBusMessageIter struct_iter;
			dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL, &struct_iter);

			/*  Client details */
			//dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &client_qos->clientid);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT64, &client_qos->read_bucket.max_bw_allowed);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT64, &client_qos->write_bucket.max_bw_allowed);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32, &client_qos->read_bucket.tokens_consumed);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32, &client_qos->write_bucket.tokens_consumed);

			dbus_message_iter_close_container(&array_iter, &struct_iter);

			client_qos = client_qos->next;
			count++;
		}
		PTHREAD_MUTEX_unlock(&share_qos->lock);
	}

	dbus_message_iter_close_container(&iter, &array_iter);
	if (export != NULL)
		put_gsh_export(export);
	return true;
}
/*  QoS Method Arguments */
#define CLIENT_IP_ARG    {"client_ip", "s", "in"}
#define READ_BW_IN_ARG   {"read_bw", "t", "in"}
#define WRITE_BW_IN_ARG  {"write_bw", "t", "in"}
#define READ_BW_OUT_ARG  {"read_bw", "t", "out"}
#define WRITE_BW_OUT_ARG {"write_bw", "t", "out"}
#define SUCCESS_ARG      {"success", "b", "out"}
#define SHARE_ID_ARG     {"id", "q", "in"}
#define MAX_TOKENS_IN    {"max_tokens", "u", "in"}
#define MAX_TOKENS_OUT   {"max_tokens", "u", "out"}
#define TOKEN_RENEW_IN   {"token_renewal", "t", "in"}
#define TOKEN_RENEW_OUT  {"token_renewal", "t", "out"}
#define CLIENT_LIST_ARG  {"client_list", "a(sttuu)", "out"}
#define TOTAL_CLIENTS    {"total_clients", "u", "out"}
#define OFFSET_ARG       {"offset", "u", "in"}
#define LIMIT_ARG        {"limit", "u", "in"}

static struct gsh_dbus_method qos_client_bw_set = {
	.name = "SetClientBandwidth",
	.method = dbus_qos_client_bw_set,
	.args = {CLIENT_IP_ARG, READ_BW_IN_ARG, WRITE_BW_IN_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_client_bw_get = {
	.name = "GetClientBandwidth",
	.method = dbus_qos_client_bw_get,
	.args = {CLIENT_IP_ARG, READ_BW_OUT_ARG, WRITE_BW_OUT_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_client_token_set = {
	.name = "SetClientTokens",
	.method = dbus_qos_client_token_set,
	.args = {CLIENT_IP_ARG, MAX_TOKENS_IN, TOKEN_RENEW_IN, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_client_token_get = {
	.name = "GetClientTokens",
	.method = dbus_qos_client_token_get,
	.args = {CLIENT_IP_ARG, MAX_TOKENS_OUT, TOKEN_RENEW_OUT, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_share_bw_set = {
	.name = "SetShareBandwidth",
	.method = dbus_qos_share_bw_set,
	.args = {SHARE_ID_ARG, READ_BW_IN_ARG, WRITE_BW_IN_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_share_bw_get = {
	.name = "GetShareBandwidth",
	.method = dbus_qos_share_bw_get,
	.args = {SHARE_ID_ARG, READ_BW_OUT_ARG, WRITE_BW_OUT_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_share_token_set = {
	.name = "SetShareTokens",
	.method = dbus_qos_share_token_set,
	.args = {SHARE_ID_ARG, MAX_TOKENS_IN, TOKEN_RENEW_IN, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_share_token_get = {
	.name = "GetShareTokens",
	.method = dbus_qos_share_token_get,
	.args = {SHARE_ID_ARG, MAX_TOKENS_OUT, TOKEN_RENEW_OUT, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_pspc_bw_set = {
	.name = "SetClientShareBandwidth",
	.method = dbus_qos_pspc_bw_set,
	.args = {SHARE_ID_ARG, CLIENT_IP_ARG, READ_BW_IN_ARG, WRITE_BW_IN_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_pspc_bw_get = {
	.name = "GetClientShareBandwidth",
	.method = dbus_qos_pspc_bw_get,
	.args = {SHARE_ID_ARG, CLIENT_IP_ARG, READ_BW_OUT_ARG, WRITE_BW_OUT_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_pspc_token_set = {
	.name = "SetClientShareTokens",
	.method = dbus_qos_pspc_token_set,
	.args = {SHARE_ID_ARG, CLIENT_IP_ARG, MAX_TOKENS_IN, TOKEN_RENEW_IN, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_pspc_token_get = {
	.name = "GetClientShareTokens",
	.method = dbus_qos_pspc_token_get,
	.args = {SHARE_ID_ARG, CLIENT_IP_ARG, MAX_TOKENS_OUT, TOKEN_RENEW_OUT, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method qos_share_clients_list = {
	.name = "ListShareClientsBandwidth",
	.method = dbus_qos_share_clients_bw_list,
	.args = {SHARE_ID_ARG, OFFSET_ARG, LIMIT_ARG, TOTAL_CLIENTS, CLIENT_LIST_ARG, SUCCESS_ARG, END_ARG_LIST}
};

static struct gsh_dbus_method *qos_methods[] = {
	&qos_client_bw_set,
	&qos_client_bw_get,
	&qos_client_token_set,
	&qos_client_token_get,
	&qos_share_bw_set,
	&qos_share_bw_get,
	&qos_share_token_set,
	&qos_share_token_get,
	&qos_pspc_bw_set,
	&qos_pspc_bw_get,
	&qos_pspc_token_set,
	&qos_pspc_token_get,
	&qos_share_clients_list,
	NULL
};

static struct gsh_dbus_interface qos_interface = {
	.name = "org.ganesha.nfsd.qos",
	.props = NULL,
	.methods = qos_methods,
	.signals = NULL
};

static struct gsh_dbus_interface *nqos_interface[] = { &qos_interface, NULL};
/*
static struct gsh_dbus_method qos_methods[13] = {
	//Client Methods
	{
		.name = "SetClientBandwidth",
		.method = dbus_qos_client_bw_set,
		.args = {
			{"client_ip", "s", "in"},
			{"read_bw", "t", "in"},
			{"write_bw", "t", "in"},
			{"success", "b", "out"},
			{NULL,NULL,NULL}
		},
	},
	{
		.name = "GetClientBandwidth",
		.method = dbus_qos_client_bw_get,
		.args = {
			{"client_ip", "s", "in"},
			{"read_bw", "t", "out"},
			{"write_bw", "t", "out"},
			{"success", "b", "out"},
			NULL
		},
	},
	{
		.name = "SetClientTokens",
		.method = dbus_qos_client_token_set,
		.args = {
			{"client_ip", "s", "in"},
			{"max_tokens", "u", "in"},
			{"token_renewal", "t", "in"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "GetClientTokens",
		.method = dbus_qos_client_token_get,
		.args = {
			{"client_ip", "s", "in"},
			{"max_tokens", "u", "out"},
			{"token_renewal", "t", "out"},
			{"success", "b", "out"},
			NULL
		}
	},

	//  Share methods
	{
		.name = "SetShareBandwidth",
		.method = dbus_qos_share_bw_set,
		.args = {
			{"id", "q", "in"},
			{"read_bw", "t", "in"},
			{"write_bw", "t", "in"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "GetShareBandwidth",
		.method = dbus_qos_share_bw_get,
		.args = {
			{"id", "q", "in"},
			{"read_bw", "t", "out"},
			{"write_bw", "t", "out"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "SetShareTokens",
		.method = dbus_qos_share_token_set,
		.args = {
			{"id", "q", "in"},
			{"max_tokens", "u", "in"},
			{"token_renewal", "t", "in"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "GetShareTokens",
		.method = dbus_qos_share_token_get,
		.args = {
			{"id", "q", "in"},
			{"max_tokens", "u", "out"},
			{"token_renewal", "t", "out"},
			{"success", "b", "out"},
			NULL
		}
	},

	//  Share-Client methods
	{
		.name = "SetClientShareBandwidth",
		.method = dbus_qos_pspc_bw_set,
		.args = {
			{"id", "q", "in"},
			{"client_ip", "s", "in"},
			{"read_bw", "t", "in"},
			{"write_bw", "t", "in"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "GetClientShareBandwidth",
		.method = dbus_qos_pspc_bw_get,
		.args = {
			{"id", "q", "in"},
			{"client_ip", "s", "in"},
			{"read_bw", "t", "out"},
			{"write_bw", "t", "out"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "SetClientShareTokens",
		.method = dbus_qos_pspc_token_set,
		.args = {
			{"id", "q", "in"},
			{"client_ip", "s", "in"},
			{"max_tokens", "u", "in"},
			{"token_renewal", "t", "in"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "GetClientShareTokens",
		.method = dbus_qos_pspc_token_get,
		.args = {
			{"id", "q", "in"},
			{"client_ip", "s", "in"},
			{"max_tokens", "u", "out"},
			{"token_renewal", "t", "out"},
			{"success", "b", "out"},
			NULL
		}
	},
	{
		.name = "ListShareClientsBandwidth",
		.method = dbus_qos_share_clients_bw_list,
		.args = {
			{"id", "q", "in"},
			{"offset", "u", "in"},
			{"limit", "u", "in"},
			{"total_clients", "u", "out"},
			{"client_list", "a(sttuu)", "out"},
			NULL
		}
	},
};
*/
/*  Client Methods args */
/*
static struct gsh_dbus_arg SetClientBandwidth_args[] = {
	{ "client_ip", "s", "in" },
	{ "read_bw", "t", "in" },
	{ "write_bw", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetClientBandwidth_args[] = {
	{ "client_ip", "s", "in" },
	{ "read_bw", "t", "out" },
	{ "write_bw", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg SetClientTokens_args[] = {
	{ "client_ip", "s", "in" },
	{ "max_tokens", "u", "in" },
	{ "token_renewal", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetClientTokens_args[] = {
	{ "client_ip", "s", "in" },
	{ "max_tokens", "u", "out" },
	{ "token_renewal", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg SetShareBandwidth_args[] = {
	{ "id", "q", "in" },
	{ "read_bw", "t", "in" },
	{ "write_bw", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetShareBandwidth_args[] = {
	{ "id", "q", "in" },
	{ "read_bw", "t", "out" },
	{ "write_bw", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg SetShareTokens_args[] = {
	{ "id", "q", "in" },
	{ "max_tokens", "u", "in" },
	{ "token_renewal", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetShareTokens_args[] = {
	{ "id", "q", "in" },
	{ "max_tokens", "u", "out" },
	{ "token_renewal", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg SetClientShareBandwidth_args[] = {
	{ "id", "q", "in" },
	{ "client_ip", "s", "in" },
	{ "read_bw", "t", "in" },
	{ "write_bw", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetClientShareBandwidth_args[] = {
	{ "id", "q", "in" },
	{ "client_ip", "s", "in" },
	{ "read_bw", "t", "out" },
	{ "write_bw", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg SetClientShareTokens_args[] = {
	{ "id", "q", "in" },
	{ "client_ip", "s", "in" },
	{ "max_tokens", "u", "in" },
	{ "token_renewal", "t", "in" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg GetClientShareTokens_args[] = {
	{ "id", "q", "in" },
	{ "client_ip", "s", "in" },
	{ "max_tokens", "u", "out" },
	{ "token_renewal", "t", "out" },
	{ "success", "b", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_arg ListShareClientsBandwidth_args[] = {
	{ "id", "q", "in" },
	{ "offset", "u", "in" },
	{ "limit", "u", "in" },
	{ "total_clients", "u", "out" },
	{ "client_list", "a(sttuu)", "out" },
	{ NULL, NULL, NULL }
};

static struct gsh_dbus_method qos_methods[13] = {
	{
		.name = "SetClientBandwidth",
		.method = dbus_qos_client_bw_set,
		.args = SetClientBandwidth_args
	},
	{
		.name = "GetClientBandwidth",
		.method = dbus_qos_client_bw_get,
		.args = GetClientBandwidth_args
	},
	{
		.name = "SetClientTokens",
		.method = dbus_qos_client_token_set,
		.args = SetClientTokens_args
	},
	{
		.name = "GetClientTokens",
		.method = dbus_qos_client_token_get,
		.args = GetClientTokens_args
	},
	{
		.name = "SetShareBandwidth",
		.method = dbus_qos_share_bw_set,
		.args = SetShareBandwidth_args
	},
	{
		.name = "GetShareBandwidth",
		.method = dbus_qos_share_bw_get,
		.args = GetShareBandwidth_args
	},
	{
		.name = "SetShareTokens",
		.method = dbus_qos_share_token_set,
		.args = SetShareTokens_args
	},
	{
		.name = "GetShareTokens",
		.method = dbus_qos_share_token_get,
		.args = GetShareTokens_args
	},
	{
		.name = "SetClientShareBandwidth",
		.method = dbus_qos_pspc_bw_set,
		.args = SetClientShareBandwidth_args
	},
	{
		.name = "GetClientShareBandwidth",
		.method = dbus_qos_pspc_bw_get,
		.args = GetClientShareBandwidth_args
	},
	{
		.name = "SetClientShareTokens",
		.method = dbus_qos_pspc_token_set,
		.args = SetClientShareTokens_args
	},
	{
		.name = "GetClientShareTokens",
		.method = dbus_qos_pspc_token_get,
		.args = GetClientShareTokens_args
	},
	{
		.name = "ListShareClientsBandwidth",
		.method = dbus_qos_share_clients_bw_list,
		.args = ListShareClientsBandwidth_args
	}
};
*/
/*  D-Bus interface definition */
/*
static struct gsh_dbus_interface qos_interface = {
	.name = "org.ganesha.nfsd.qos",
	.signal_props = false,
	.props = NULL,
	.methods = qos_methods,
	.signals = NULL
};
*/
/*  Initialize QoS manager */
void dbus_qosmgr_init(void)
{
	//gsh_dbus_register_interface(&qos_interface);
	gsh_dbus_register_path("QosMgr", nqos_interface);
}
