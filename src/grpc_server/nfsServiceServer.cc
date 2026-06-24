/* Copyright (C) 2025, IBM
 * Contributor : Avani Rateria <arateria@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

#include <string>
#include "nfsService.h"
#include "avltree.h"
#include "idmapper.h"
#include "export_mgr.h"
#include "nfs_exports.h"
#include "nfs_exports.h"
#include "client_mgr.h"
#include "config_parsing.h"
#include "nfs_proto_functions.h"

static struct avltree uname_tree;

grpc::Status GetClientIdService::GetClientIds(
	grpc::ServerContext *context, const nfsProtoUtil::EmptyRequest *request,
	nfsService::GetClientIdsResponse *response)
{
	hash_table_t *ht = ht_confirmed_client_id;

	for (uint32_t i = 0; i < ht->parameter.index_size; ++i) {
		struct rbt_head *head_rbt = &(ht->partitions[i].rbt);

		PTHREAD_RWLOCK_rdlock(&(ht->partitions[i].ht_lock));
		struct rbt_node *pn;
		RBT_LOOP(head_rbt, pn)
		{
			const struct hash_data *pdata =
				(hash_data *)RBT_OPAQ(pn);
			const nfs_client_id_t *pclientid =
				(nfs_client_id_t *)pdata->val.addr;
			const uint64_t clientid = pclientid->cid_clientid;
			// Add the client ID to the list
			response->add_client_ids(clientid);
			RBT_INCREMENT(pn);
		} // RBT_LOOP

		PTHREAD_RWLOCK_unlock(&(ht->partitions[i].ht_lock));
	} // for loop

	return grpc::Status::OK;
}

grpc::Status GetNfsGraceService::GetGracePeriod(
	grpc::ServerContext *context, const nfsProtoUtil::EmptyRequest *request,
	nfsService::GetNfsGraceResponse *response)
{
	// Set the response
	response->set_ingrace(nfs_in_grace());

	return grpc::Status::OK;
}

grpc::Status StartNfsGraceService::StartGraceWithEvent(
	grpc::ServerContext *context, const nfsService::GraceWithEvent *request,
	nfsService::GraceStatus *response)
{
	int ret;
	int event = request->event();
	int nodeid = request->nodeid();
	std::string ip_addr = request->ipaddr();
	std::string resp;
	nfs_grace_start_t gsp;

	// Carry out required action
	gsp.nodeid = nodeid;
	gsp.event = event;
	gsp.ipaddr = (char *)ip_addr.c_str();
	do {
		ret = nfs_start_grace(&gsp);
		/*
                 * grace could fail if there are refs taken.
                 * wait for no refs and retry.
                 */
		if (ret == -EAGAIN) {
			LogEvent(COMPONENT_GRPC, "Retry grace");
			nfs_wait_for_grace_norefs();
		} else if (ret) {
			LogCrit(COMPONENT_GRPC, "Start grace failed %d", ret);
			resp = "Unable to start grace";
			response->set_gracestarted(false);
			break;
		}
	} while (ret);
	// Send back the response
	if (!ret) {
		resp = "Grace started successfully";
		response->set_gracestarted(true);
	}
	response->set_response_msg(resp);

	return grpc::Status::OK;
}

grpc::Status GetSessionIdService::GetSessionIds(
	grpc::ServerContext *context, const nfsProtoUtil::EmptyRequest *request,
	nfsService::GetSessionIdsResponse *response)
{
	uint32_t i;
	hash_table_t *ht = ht_session_id;
	struct rbt_head *head_rbt;
	struct hash_data *pdata = NULL;
	struct rbt_node *pn;
	std::string session_id('\0', 2 * NFS4_SESSIONID_SIZE);
	nfs41_session_t *session_data;

	for (i = 0; i < ht->parameter.index_size; i++) {
		head_rbt = &(ht->partitions[i].rbt);
		PTHREAD_RWLOCK_rdlock(&(ht->partitions[i].ht_lock));
		RBT_LOOP(head_rbt, pn)
		{
			pdata = (hash_data *)RBT_OPAQ(pn);
			session_data = (nfs41_session_t *)pdata->val.addr;

			b64_ntop((unsigned char *)session_data->session_id,
				 NFS4_SESSIONID_SIZE, session_id.data(),
				 (2 * NFS4_SESSIONID_SIZE));
			// Set the response
			response->add_session_ids(session_id);
			RBT_INCREMENT(pn);
		}
		PTHREAD_RWLOCK_unlock(&(ht->partitions[i].ht_lock));
	}
	return grpc::Status::OK;
}

grpc::Status ShowIdMapperService::ShowIdMapper(
	grpc::ServerContext *context, const nfsProtoUtil::EmptyRequest *request,
	nfsService::ShowIdMapperResponse *response)
{
	try {
		struct avltree_node *node;
		char namebuff[256 + 1];
		struct timespec timestamp;

		// Get current time
		now(&timestamp);
		response->set_timestamp_sec(
			static_cast<uint64_t>(timestamp.tv_sec));
		response->set_timestamp_nsec(
			static_cast<uint64_t>(timestamp.tv_nsec));

		PTHREAD_RWLOCK_rdlock(&idmapper_user_lock);

		// Traverse idmapper cache
		for (node = avltree_first(&uname_tree); node != nullptr;
		     node = avltree_next(node)) {
			const struct cache_user *user =
				avltree_container_of(node, struct cache_user,
						     uname_node);

			size_t len = user->uname.len > 255 ? 255
							   : user->uname.len;
			memcpy(namebuff, user->uname.addr, len);
			namebuff[len] = '\0'; // null terminate

			// Add new entry in protobuf repeated field
			auto *entry = response->add_entries();
			entry->set_name(namebuff);
			entry->set_uid(user->uid);
			entry->set_gid_set(user->gid_set);
			entry->set_gid(user->gid_set ? user->gid : 0);
		}

		PTHREAD_RWLOCK_unlock(&idmapper_user_lock);

		return grpc::Status::OK;
	} catch (const std::exception &ex) {
		return grpc::Status(grpc::StatusCode::INTERNAL,
				    "Internal error occurred");
	}
}

grpc::Status
ExportService::DisplayExport(grpc::ServerContext *context,
			     const exportService::DisplayExportRequest *request,
			     exportService::DisplayExportResponse *response)
{
	char *errormsg;
	struct gsh_export *export_obj =
		lookup_export_by_id(request->export_id(), &errormsg);
	if (!export_obj) {
		response->set_success(false);
		response->set_error_message(errormsg);
		return grpc::Status::OK;
	}

	struct tmp_export_paths tmp;
	tmp_get_exp_paths(&tmp, export_obj);

	response->set_export_id(export_obj->export_id);

	const char *full = TMP_FULLPATH(&tmp);
	if (full && full[0] != '\0') {
		response->set_full_path(full);
	}

	if (export_obj->cfg_pseudopath != nullptr)
		response->set_pseudo_path(export_obj->cfg_pseudopath);

	struct glist_head *glist;

	PTHREAD_RWLOCK_rdlock(&export_obj->exp_lock);

	glist_for_each(glist, &export_obj->clients) {
		struct base_client_entry *client;
		struct exportlist_client_entry *expclient;

		client = glist_entry(glist, struct base_client_entry, cle_list);
		expclient = container_of(client, struct exportlist_client_entry,
					 client_entry);

		client = glist_entry(glist, struct base_client_entry, cle_list);

		expclient = container_of(client, struct exportlist_client_entry,
					 client_entry);
		auto *out = response->add_clients();

		out->set_client_name(client->str ? client->str : "");

		if (client->type == NETWORK_CLIENT && client->cidr != NULL) {
			unsigned char addr[16] = { 0 };
			unsigned char mask[16] = { 0 };

			out->set_client_type("NETWORK_CLIENT");

			out->set_cidr_version(cidr_version(client->cidr));

			cidr_ipaddr_to_chars(client->cidr, addr);
			out->set_cidr_addr(std::string(
				reinterpret_cast<char *>(addr), sizeof(addr)));

			cidr_mask_to_chars(client->cidr, mask);
			out->set_cidr_mask(std::string(
				reinterpret_cast<char *>(mask), sizeof(mask)));

			out->set_cidr_proto(cidr_proto(client->cidr));
		} else {
			switch (client->type) {
			case NETGROUP_CLIENT:
				out->set_client_type("NETGROUP_CLIENT");
				break;
			case GSSPRINCIPAL_CLIENT:
				out->set_client_type("GSSPRINCIPAL_CLIENT");
				break;
			case MATCH_ANY_CLIENT:
				out->set_client_type("MATCH_ANY_CLIENT");
				break;
			case WILDCARDHOST_CLIENT:
				out->set_client_type("WILDCARDHOST_CLIENT");
				break;
			default:
				out->set_client_type("UNKNOWN_CLIENT");
				break;
			}

			/* Match the D-Bus behavior for non-network clients. */
			out->set_cidr_version(0);
			out->set_cidr_addr("");
			out->set_cidr_mask("");
			out->set_cidr_proto(0);
		}

		out->set_anonymous_uid(expclient->client_perms.anonymous_uid);
		out->set_anonymous_gid(expclient->client_perms.anonymous_gid);
		out->set_expire_time_attr(
			expclient->client_perms.expire_time_attr);
		out->set_options(expclient->client_perms.options);
		out->set_set(expclient->client_perms.set);

		char perm_buf[1024] = "\0";
		struct display_buffer dspbuf = { sizeof(perm_buf), perm_buf,
						 perm_buf };

		grpc_StrExportOptions(&dspbuf, &expclient->client_perms);

		out->set_permissions(perm_buf);
	}

	PTHREAD_RWLOCK_unlock(&export_obj->exp_lock);

	tmp_put_exp_paths(&tmp);
	put_gsh_export(export_obj);

	response->set_success(true);
	return grpc::Status::OK;
}

/*
 * TODO: Extend ShowExports to include the server_stats_summary information
 * returned by the DBus interface. This will be implemented together with
 * the remaining export statistics gRPC APIs.
 */
grpc::Status ExportService::ShowExports(
	grpc::ServerContext *context, const nfsProtoUtil::EmptyRequest *request,
	exportService::ShowExportsResponse *response)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	response->set_timestamp_sec(ts.tv_sec);
	response->set_timestamp_nsec(ts.tv_nsec);
	pthread_rwlock_t *lock = get_export_id_lock();
	struct glist_head *exportlist_head = get_exportlist_head();
	PTHREAD_RWLOCK_rdlock(lock);
	struct glist_head *node;
	glist_for_each(node, exportlist_head) {
		struct gsh_export *export_obj =
			glist_entry(node, struct gsh_export, exp_list);

		struct tmp_export_paths tmp = { nullptr, nullptr };
		tmp_get_exp_paths(&tmp, export_obj);
		const char *full = TMP_FULLPATH(&tmp);
		auto *out = response->add_exports();
		out->set_export_id(
			static_cast<uint32_t>(export_obj->export_id));
		if (full != nullptr)
			out->set_full_path(full);

		if (export_obj->cfg_pseudopath != nullptr)
			out->set_pseudo_path(export_obj->cfg_pseudopath);

		if (export_obj->FS_tag != nullptr)
			out->set_fs_tag(export_obj->FS_tag);

		tmp_put_exp_paths(&tmp);
	}
	PTHREAD_RWLOCK_unlock(lock);
	response->set_success(true);
	return grpc::Status::OK;
}

grpc::Status
ExportService::AddExport(grpc::ServerContext *context,
			 const exportService::AddExportRequest *request,
			 exportService::AddExportResponse *response)
{
	const std::string &file_path = request->file_path();
	const std::string &export_expr = request->export_expression();

	int rc = 0, exp_cnt = 0;
	bool status = true;
	config_file_t config_struct = NULL;
	struct config_node_list *config_list = NULL, *lp, *lp_next;
	struct config_error_type err_type;
	struct error_detail conf_errs = { NULL, 0, NULL };
	struct stat st;
	char *file_path_mut = nullptr;
	char *export_expr_mut = nullptr;

	response->set_success(false);

	// Lock export admin
	if (EXPORT_ADMIN_TRYLOCK() != 0) {
		response->set_message(
			"Another export admin operation is in progress, try again later");
		return grpc::Status::OK;
	}

	// Validate path is regular file
	rc = stat(file_path.c_str(), &st);
	if (rc < 0 || (st.st_mode & S_IFMT) != S_IFREG) {
		response->set_message("Invalid config file path: " + file_path);
		status = false;
		goto out_unlock;
	}

	// Initialize error type for parser
	if (!init_error_type(&err_type)) {
		response->set_message("Failed to initialize error type");
		goto out_unlock;
	}

	// Parse config file
	file_path_mut = strdup(file_path.c_str());
	config_struct = config_ParseFile(file_path_mut, &err_type);
	free(file_path_mut);

	// Check for parse errors
	if (!cur_exp_config_error_is_harmless(&err_type)) {
		std::string err_detail = err_type_str(&err_type);
		response->set_message("Error parsing config file: " +
				      err_detail);
		status = false;
		goto out_unlock;
	}

	// Find export nodes matching expression
	export_expr_mut = strdup(export_expr.c_str());
	rc = find_config_nodes(config_struct, export_expr_mut, &config_list,
			       &err_type);
	free(export_expr_mut);
	if (rc != 0) {
		response->set_message("Failed to find export nodes: " +
				      std::string(strerror(rc)));
		status = false;
		goto out_unlock;
	}

	// Load exports and count success
	for (lp = config_list; lp != NULL; lp = lp_next) {
		lp_next = lp->next;
		if (status) {
			rc = load_config_from_node(lp->tree_node,
						   &add_export_param, NULL,
						   false, &err_type);
			if (rc == 0 ||
			    cur_exp_config_error_is_harmless(&err_type)) {
				exp_cnt++;
			} else {
				status = false;
			}
		}
		gsh_free(lp);
	}

	// Report results
	if (status) {
		std::string msg = std::to_string(exp_cnt) + " exports added";
		if (exp_cnt > 0 && conf_errs.buf) {
			msg += ". Errors found:\n" + std::string(conf_errs.buf);
		}
		response->set_success(true);
		response->set_message(msg);
	} else {
		std::string err_detail = err_type_str(&err_type);
		response->set_success(false);
		response->set_message(
			std::to_string(exp_cnt) +
			" exports added but with errors: " + err_detail);
	}

out_unlock:

	if (conf_errs.fp)
		fclose(conf_errs.fp);
	if (conf_errs.buf)
		gsh_free(conf_errs.buf);
	if (err_type.exists)
		config_Free(config_struct);
	EXPORT_ADMIN_UNLOCK();

	return grpc::Status::OK;
}

grpc::Status
ExportService::RemoveExport(grpc::ServerContext *context,
			    const exportService::RemoveExportRequest *request,
			    exportService::RemoveExportResponse *response)
{
	// Variable declarations
	struct gsh_export *export_obj = NULL;
	char *errormsg = NULL;

	// Lookup export by ID
	export_obj = lookup_export_by_id(request->id(), &errormsg);
	if (export_obj == NULL) {
		response->set_success(false);
		response->set_message("lookup_export failed: " +
				      std::string(errormsg));
		return grpc::Status::OK;
	}

	// Check for invalid ID 0
	if (export_obj->export_id == 0) {
		put_gsh_export(export_obj);
		response->set_success(false);
		response->set_message("Cannot remove export with id 0");
		return grpc::Status::OK;
	}

	// Lock for admin operation
	if (EXPORT_ADMIN_TRYLOCK() != 0) {
		response->set_success(false);
		response->set_message(
			"another export admin operation is in progress");
		put_gsh_export(export_obj);
		return grpc::Status::OK;
	}

	// Check for mounted subexports
	PTHREAD_RWLOCK_rdlock(&export_obj->exp_lock);
	bool is_empty = glist_empty(&export_obj->mounted_exports_list);
	PTHREAD_RWLOCK_unlock(&export_obj->exp_lock);
	if (!is_empty) {
		put_gsh_export(export_obj);
		response->set_success(false);
		response->set_message("Cannot remove export with submounts");
		EXPORT_ADMIN_UNLOCK();
		return grpc::Status::OK;
	}

	// Prepare operation context and release
	struct req_op_context op_context;
	init_op_context_simple(&op_context, export_obj,
			       export_obj->fsal_export);
	release_export(export_obj, false);
	release_op_context();

	// Unlock admin lock
	EXPORT_ADMIN_UNLOCK();

	response->set_success(true);
	response->set_message("Export removed successfully");

	return grpc::Status::OK;
}

grpc::Status
ExportService::UpdateExport(grpc::ServerContext *context,
			    const exportService::UpdateExportRequest *request,
			    exportService::UpdateExportResponse *response)
{
	int rc = 0, exp_cnt = 0;
	bool status = true;
	const std::string &file_path = request->file_path();
	const std::string &export_expr = request->export_expr();

	config_file_t config_struct = NULL;
	struct config_node_list *config_list = NULL, *lp = NULL,
				*lp_next = NULL;
	struct config_error_type err_type;
	char *err_detail = NULL;
	struct error_detail conf_errs = { NULL, 0, NULL };

	response->set_success(false);
	response->set_message("");

	if (file_path.empty()) {
		response->set_message("Pathname is empty");
		return grpc::Status::OK;
	}

	if (export_expr.empty()) {
		response->set_message("export expression is empty");
		return grpc::Status::OK;
	}

	if (!init_error_type(&err_type)) {
		response->set_message(
			"failed to initialize parser error state");
		goto out;
	}

	config_struct = config_ParseFile(const_cast<char *>(file_path.c_str()),
					 &err_type);
	if (!cur_exp_config_error_is_harmless(&err_type)) {
		err_detail = err_type_str(&err_type);
		(void)report_config_errors(&err_type, &conf_errs,
					   config_errs_to_log);
		response->set_message(
			"Error while parsing " + file_path + " because of " +
			std::string(err_detail ? err_detail : "unknown") +
			" errors. Details:\n" +
			std::string(conf_errs.buf ? conf_errs.buf : ""));
		status = false;
		goto out;
	}

	rc = find_config_nodes(config_struct,
			       const_cast<char *>(export_expr.c_str()),
			       &config_list, &err_type);
	if (rc != 0) {
		(void)report_config_errors(&err_type, &conf_errs,
					   config_errs_to_log);
		response->set_message("Error finding exports: " + export_expr +
				      " because " + std::string(strerror(rc)));
		status = false;
		goto out;
	}

	for (lp = config_list; lp != NULL; lp = lp_next) {
		lp_next = lp->next;
		if (status) {
			rc = load_config_from_node(lp->tree_node,
						   &update_export_param, NULL,
						   false, &err_type);
			if (rc == 0 ||
			    cur_exp_config_error_is_harmless(&err_type))
				exp_cnt++;
			else if (!err_type.exists)
				status = false;
		}
		gsh_free(lp);
	}

	if (status && exp_cnt > 0) {
		synchronize_updated_exports();
	}

	(void)report_config_errors(&err_type, &conf_errs, config_errs_to_log);

	if (status) {
		if (exp_cnt > 0) {
			std::string msg =
				std::to_string(exp_cnt) + " exports updated";
			if (conf_errs.buf != NULL &&
			    strlen(conf_errs.buf) > 0) {
				msg += ". Errors found:\n";
				msg += conf_errs.buf;
			}
			response->set_success(true);
			response->set_message(msg);
		} else if (err_type.exists) {
			response->set_message("Selected entries in " +
					      file_path + " already active!!!");
			status = false;
		} else {
			response->set_message(
				"No new export entries found in " + file_path);
			status = false;
		}
	} else {
		err_detail = err_detail ? err_detail : err_type_str(&err_type);
		response->set_message(
			std::to_string(exp_cnt) + " export entries in " +
			file_path + " updated because " +
			std::string(err_detail ? err_detail : "unknown") +
			" errors. Details:\n" +
			std::string(conf_errs.buf ? conf_errs.buf : ""));
	}

out:
	if (conf_errs.fp != NULL)
		fclose(conf_errs.fp);
	if (conf_errs.buf != NULL)
		gsh_free(conf_errs.buf);
	if (err_detail != NULL)
		gsh_free(err_detail);
	config_Free(config_struct);

	return grpc::Status::OK;
}
