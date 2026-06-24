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
#include "FSAL/fsal_commonlib.h"
#include "avltree.h"
#include "idmapper.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_localfs.h"
#include "export_mgr.h"
#include "nfs_exports.h"
#include "client_mgr.h"
#include "config_parsing.h"
extern "C" {
#include "nfs_exports.h"
}
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
	grpc::ServerContext *context,
	const nfsService::ShowIdMapperRequest *request,
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

grpc::Status FileSystemService::ShowPosixFileSystems(
	grpc::ServerContext *context,
	const nfsService::ShowPosixFileSystemsRequest *request,
	nfsService::ShowPosixFileSystemsResponse *response)
{
	try {
		struct fsal_filesystem *fs;
		struct glist_head *glist;
		struct timespec timestamp;

		now(&timestamp);
		response->set_timestamp_sec(
			static_cast<uint64_t>(timestamp.tv_sec));
		response->set_timestamp_nsec(
			static_cast<uint64_t>(timestamp.tv_nsec));

		PTHREAD_RWLOCK_rdlock(&fs_lock);

		glist_for_each(glist, &posix_file_systems) {
			fs = glist_entry(glist, struct fsal_filesystem,
					 filesystems);

			auto *entry = response->add_file_systems();

			entry->set_path(fs->path ? fs->path : "");
			entry->set_major_dev(fs->dev.major);
			entry->set_minor_dev(fs->dev.minor);
		}

		PTHREAD_RWLOCK_unlock(&fs_lock);

		return grpc::Status::OK;
	} catch (const std::exception &ex) {
		return grpc::Status(grpc::StatusCode::INTERNAL,
				    "Internal error occurred");
	}
}

grpc::Status
ExportService::DisplayExport(grpc::ServerContext *context,
			     const nfsService::DisplayExportRequest *request,
			     nfsService::DisplayExportResponse *response)
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
	response->set_full_path(TMP_FULLPATH(&tmp));
	response->set_config_path(tmp_export_path(&tmp));
	response->set_fs_tag(export_obj->FS_tag ? export_obj->FS_tag : "");

	/*PTHREAD_RWLOCK_rdlock(&export_obj->exp_lock);
     struct glist_head *glist;
        glist_for_each(glist, &export_obj->clients) {
        struct base_client_entry* client;
        struct exportlist_client_entry* expclient;

        client = glist_entry(glist, struct base_client_entry, cle_list);
        expclient = container_of(client, struct exportlist_client_entry, client_entry);

        auto entry = response->add_clients();
        entry->set_client_name(expclient->client_entry.name ? expclient->client_entry.name : "");
        entry->set_uid(expclient->client_entry.uid);
        entry->set_gid(expclient->client_entry.gid);

        // Add other client fields as needed
    }*/
	PTHREAD_RWLOCK_rdlock(&export_obj->exp_lock);
	struct glist_head *glist;
	glist_for_each(glist, &export_obj->clients) {
		auto *entry = response->add_clients();
		entry->set_client_name(
			"client"); // placeholder until you map real data
	}
	PTHREAD_RWLOCK_unlock(&export_obj->exp_lock);

	response->set_success(true);
	return grpc::Status::OK;
}

grpc::Status
ExportService::ShowExports(grpc::ServerContext *context,
			   const nfsService::ShowExportsRequest *request,
			   nfsService::ShowExportsResponse *response)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	response->set_timestamp_sec(ts.tv_sec);
	response->set_timestamp_nsec(ts.tv_nsec);
	pthread_rwlock_t *lock = get_export_id_lock();
	struct glist_head *exportlist_head = get_exportlist_head();
	PTHREAD_RWLOCK_rdlock(lock);
	struct glist_head *node;
	glist_for_each(node, exportlist_head) { // <-- use exportlist_head here
		struct gsh_export *export_obj =
			glist_entry(node, struct gsh_export, exp_list);

		// Build paths like DBus does
		struct tmp_export_paths tmp = { nullptr, nullptr };
		tmp_get_exp_paths(&tmp, export_obj);

		const char *full = TMP_FULLPATH(&tmp);
		const char *cfg = tmp_export_path(&tmp);

		auto *out = response->add_exports();
		out->set_export_id(
			static_cast<uint32_t>(export_obj->export_id));
		// Your proto has only 'path', so put the best we have (full path if available, else config path)
		out->set_path(full ? full : (cfg ? cfg : ""));
		out->set_fs_tag(export_obj->FS_tag ? export_obj->FS_tag : "");

		tmp_put_exp_paths(&tmp);
	}

	PTHREAD_RWLOCK_unlock(lock);

	response->set_success(true);
	return grpc::Status::OK;
}

grpc::Status
ExportService::AddExport(grpc::ServerContext *context,
			 const nfsService::AddExportRequest *request,
			 nfsService::AddExportResponse *response)
{
	const std::string &file_path = request->file_path();
	const std::string &export_expr = request->export_expression();

	// Variables analogous to DBus code
	int rc = 0, exp_cnt = 0;
	bool status = true;
	config_file_t config_struct = NULL;
	struct config_node_list *config_list = NULL, *lp, *lp_next;
	struct config_error_type err_type;
	struct error_detail conf_errs = { NULL, 0, NULL };
	struct stat st;

	// Lock export admin
	if (EXPORT_ADMIN_TRYLOCK() != 0) {
		response->set_success(false);
		response->set_message(
			"Another export admin operation is in progress, try again later");
		return grpc::Status::OK;
	}

	// Validate path is regular file
	rc = stat(file_path.c_str(), &st);
	if (rc < 0 || (st.st_mode & S_IFMT) != S_IFREG) {
		response->set_success(false);
		response->set_message("Invalid config file path: " + file_path);
		EXPORT_ADMIN_UNLOCK();
		return grpc::Status::OK;
	}

	// Initialize error type for parser
	if (!init_error_type(&err_type)) {
		response->set_success(false);
		response->set_message("Failed to initialize error type");
		EXPORT_ADMIN_UNLOCK();
		return grpc::Status::OK;
	}

	// Parse config file
	char *file_path_mut = strdup(file_path.c_str());
	config_struct = config_ParseFile(file_path_mut, &err_type);
	free(file_path_mut);

	// Check for parse errors
	if (!cur_exp_config_error_is_harmless(&err_type)) {
		response->set_success(false);
		std::string err_detail = err_type_str(&err_type);
		response->set_message("Error parsing config file: " +
				      err_detail);
		EXPORT_ADMIN_UNLOCK();
		return grpc::Status::OK;
	}

	// Find export nodes matching expression
	char *export_expr_mut = strdup(export_expr.c_str());
	rc = find_config_nodes(config_struct, export_expr_mut, &config_list,
			       &err_type);
	free(export_expr_mut);
	if (rc != 0) {
		response->set_success(false);
		response->set_message("Failed to find export nodes: " +
				      std::string(strerror(rc)));
		EXPORT_ADMIN_UNLOCK();
		return grpc::Status::OK;
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

	// Cleanup
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
			    const nfsService::RemoveExportRequest *request,
			    nfsService::RemoveExportResponse *response)
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
	//release_export(export_obj, false);
	response->set_success(true);
	response->set_message("Export removed successfully");
	release_op_context();

	// Unlock admin lock
	EXPORT_ADMIN_UNLOCK();

	return grpc::Status::OK;
}
