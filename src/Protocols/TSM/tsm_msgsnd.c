// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2025, IBM . All rights reserved.
 * Author: Naresh Chillarege<Naresh.Chillarege@ibm.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.  see <http://www.gnu.org/licenses/
 *
 * ---------------------------------------
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "log.h"
#include "gsh_rpc.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "tsm.h"
#include "transparent_recovery.h"
/**
 * @brief CQOS notification
 *
 * @param[in]  args
 * @param[in]  req
 * @param[out] res
 */

int tsm_rpc_msg_recv(nfs_arg_t *args, struct svc_req *req, nfs_res_t *res)
{
	tsm_rpc_info *arg = &args->arg_tsm_msg;

	if (arg->msg_type == TSM_SET_STATE && arg->rec_type == 1) {
		/* RECEIVE + OPEN */
		LogFullDebug(COMPONENT_TSM,
			     "rec_type=OPEN "
			     "fsid_maj=%lu fsid_min=%lu fileid=%lu "
			     "share_access=%d share_deny=%d owner_str=%s",
			     arg->fsid_maj, arg->fsid_min, arg->fileid,
			     arg->open_info.share_access,
			     arg->open_info.share_deny,
			     arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE && arg->rec_type == 1) {
		/* RECEIVE + CLOSE */
		LogFullDebug(COMPONENT_TSM,
			     "rec_type=CLOSE "
			     "fsid_maj=%lu fsid_min=%lu fileid=%lu "
			     "share_access=%d share_deny=%d owner_str=%s",
			     arg->fsid_maj, arg->fsid_min, arg->fileid,
			     arg->open_info.share_access,
			     arg->open_info.share_deny,
			     arg->open_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		/* SET + LOCK */
		LogFullDebug(
			COMPONENT_TSM,
			"rec_type=LOCK "
			"fsid_maj=%lu fsid_min=%lu fileid=%lu "
			"lock_start=%lu lock_length=%lu lock_type=%d owner_str=%s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_LOCK_REC) {
		/* DELETE + UNLOCK */
		LogFullDebug(
			COMPONENT_TSM,
			"rec_type=UNLOCK "
			"fsid_maj=%lu fsid_min=%lu fileid=%lu "
			"lock_start=%lu lock_length=%lu lock_type=%d owner_str=%s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->lock_info.start, arg->lock_info.length,
			arg->lock_info.type, arg->lock_info.owner_str);

	} else if (arg->msg_type == TSM_SET_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		/* SET + DELEG */
		LogFullDebug(
			COMPONENT_TSM,
			"rec_type=DELEG "
			"fsid_maj=%lu fsid_min=%lu fileid=%lu "
			"deleg_type=%d share_access=%d share_deny=%d owner_str=%s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);

	} else if (arg->msg_type == TSM_DELETE_STATE &&
		   arg->rec_type == TSM_DELEG_REC) {
		/* DELETE + DELEG RETURN */
		LogFullDebug(
			COMPONENT_TSM,
			"rec_type=DELEGRETURN "
			"fsid_maj=%lu fsid_min=%lu fileid=%lu "
			"deleg_type=%d share_access=%d share_deny=%d owner_str=%s",
			arg->fsid_maj, arg->fsid_min, arg->fileid,
			arg->deleg_info.sd_type, arg->deleg_info.share_access,
			arg->deleg_info.share_deny, arg->deleg_info.owner_str);

	} else if (arg->msg_type == TSM_EXPORT_ID_NOTIFY) {
		LogFullDebug(COMPONENT_TSM, "TSM_EXPORT_ID_NOTIFY received");
	}

	tsm_process_recd_msg(arg);
	return NFS_REQ_OK;
}

int tsm_rpc_states_recv(nfs_arg_t *args, struct svc_req *req, nfs_res_t *res)
{
	tsm_rpc_states *arg = &args->arg_tsm_states;

	tsm_process_recd_states(arg);
	return NFS_REQ_OK;
}

/**
 * tsm_rpc_msg_Free: Frees the result structure allocated for cqos_rpc_msg_recv
 *
 * Frees the result structure allocated for cqos_rpc_msg_recv. Does Nothing in
 * fact.
 *
 * @param res        [INOUT]   Pointer to the result structure.
 *
 */
void tsm_rpc_msg_Free(nfs_res_t *res)
{
	/* Nothing to do */
}
