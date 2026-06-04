// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2025, IBM . All rights reserved.
 * Author: Deeraj Patil <deeraj.patil@ibm.com>
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
 * 02110-1301 USA.  see <http://www.gnu.org/licenses/>
 *
 * ---------------------------------------
 */

/**
 * @file nfs4_op_copy.c
 * @brief NFS4_OP_COPY - sync + async intra-server copy implementation.
 *
 * Implements the NFSv4.2 COPY operation (RFC 7862 Sec.15.2) supporting both
 * synchronous inline copying and asynchronous offload via CB_OFFLOAD.
 *
 * Per RFC 7862 15.2:
 *  - SAVED_FH  : source file
 *  - CURRENT_FH: destination file
 *  - ca_source_server_len == 0  -> intra-server (this file)
 *  - ca_source_server_len  > 0  -> inter-server (between the servers NOTSUPP)
 *
 * Path selection (controlled by nfs.conf):
 *
 *   Copy_Offload = false  OR  size < Copy_Offload_Min_Size
 *     -> SYNC path: copy completes inline on the svc thread, response sent
 *       with wr_ids=0 / cr_synchronous=TRUE.  Client sees an ordinary
 *       blocking operation.
 *
 *   Copy_Offload = true  AND  size >= Copy_Offload_Min_Size
 *     AND  ca_synchronous == FALSE
 *     -> ASYNC path :
 *       1. Server allocates a copy stateid (STATE_TYPE_COPY_OFFLOAD),
 *          registers it in the SAL, returns COPY response IMMEDIATELY
 *          with wr_ids=1 / cr_synchronous=FALSE and the copy stateid.
 *       2. An xcopy fridge worker copies data in copy_chunk_size chunks.
 *       3. When done the worker fires CB_OFFLOAD over the back channel.
 *
 *
 *   Fallback (allowed): if the xcopy fridge is full (EWOULDBLOCK), the server
 *   falls back to the sync path silently(at start itself).
 */

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "common_utils.h"
#include "fsal.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "sal_data.h"
#include "nfs_convert.h"
#include "fridgethr.h"
#include "nfs_rpc_callback.h"
#include "gsh_lttng/gsh_lttng.h"
#if defined(USE_LTTNG) && !defined(LTTNG_PARSING)
#include "gsh_lttng/generated_traces/nfs4.h"
#endif

/**
 * Maximum number of CB_OFFLOAD retransmits after a transient client error.
 *
 * RFC 7862 15.9.3: "If the client returns NFS4ERR_DELAY (or the server
 * receives no reply), the server SHOULD retransmit CB_OFFLOAD."
 * NFS4ERR_RESOURCE is treated the same way — it signals a transient
 * condition on the client, not a permanent rejection.
 *
 * After NFS4_COPY_CB_MAX_RETRIES additional attempts the server gives up
 * and destroys the copy state.
 */
#define NFS4_COPY_CB_MAX_RETRIES 2

static uint64_t copy_chunk_size;

/**
 * Dedicated fridge thread pool for NFSv4.2 COPY offload operations.
 *
 * Threads are named xcopy0, xcopy1, … in logs so copy workers are
 * distinguishable from the shared Gen_Fridge threads.  Pool size is
 * set at startup from nfs_param.nfsv4_param.max_copy_workers.
 */
static struct fridgethr *copy_fridge;

/**
 * Monotonically increasing counter for per-thread xcopyN naming.
 * Each new thread atomically claims the next value via
 * atomic_postadd_uint32_t (abstract_atomic.h, __ATOMIC_SEQ_CST).
 */
static uint32_t copy_thr_next_id;

/** TLS flag: true once this thread has set its xcopyN name. */
static __thread bool copy_thr_named;

/**
 * Validation context — stack-allocated in nfs4_op_copy.
 *
 * Holds the copy parameters produced by copy_validate() and consumed by
 * copy_run_sync(). (copy_run_async to be implemented)
 * Its lifetime ends the moment the
 * dispatch call returns:
 *
 *   Sync path:  copy_run_sync() reads all fields directly from ctx.
 *               nfs4_op_copy's out: label dec's src_state/dst_state.
 *
 *   Async path: copy_run_async() populates the matching execution fields
 *               in copy_offload_state and submits the cos to the worker.
 *               On successful dispatch copy_run_async() nulls src_state and
 *               dst_state in ctx (ownership transferred to cos) so the
 *               out: label's null-guarded dec is a no-op.
 *               ctx itself is on the stack — no free required.
 */
struct copy_ctx {
	struct fsal_obj_handle *src; /* source file handle */
	struct fsal_obj_handle *dst; /* destination file handle */
	state_t *src_state; /* source open/lock stateid */
	state_t *dst_state; /* destination open/lock stateid */
	uint64_t src_off; /* source file offset */
	uint64_t dst_off; /* destination file offset */
	uint64_t to_copy; /* number of bytes to copy */
};

/**
 * Shutdown-in-progress flag.
 *
 * Written once by nfs4_copy_fridge_shutdown() via atomic_store_uint8_t
 * BEFORE issuing fridgethr_comm_stop. copy_worker checks it every chunk
 * via atomic_fetch_uint8_t so in-flight copies abort promptly.
 */
static uint8_t copy_fridge_stopping;

int nfs4_copy_fridge_init(void)
{
	struct fridgethr_params frp;
	int rc;

	memset(&frp, 0, sizeof(frp));
	frp.thr_max = nfs_param.nfsv4_param.max_copy_workers;
	frp.thr_min = 0;
	frp.flavor = fridgethr_flavor_worker;
	/* i.e in 1 iteration to FSAL, tries to copy 4 times
	 * the size min offload, to reduce multiple round trips to FSAL*/
	copy_chunk_size = nfs_param.nfsv4_param.copy_offload_min_size * 4;
	/*
	 * fridgethr_defer_fail: when all max_copy_workers threads are busy,
	 * fridgethr_submit() returns EWOULDBLOCK immediately rather than
	 * silently queuing the job.  nfs4_op_copy() catches EWOULDBLOCK and
	 * falls back to an inline (synchronous) copy.
	 *
	 * Using fridgethr_defer_queue would create an unbounded work queue:
	 * parked RPC compounds pile up indefinitely and can exhaust memory.
	 * The sync fallback is transparent to the client — it gets a
	 * successful NFS4_OK response, just completed inline.
	 */
	frp.deferment = fridgethr_defer_fail;

	rc = fridgethr_init(&copy_fridge, "xcopy", &frp);
	if (rc != 0) {
		LogMajor(COMPONENT_THREAD,
			 "Unable to initialize copy fridge (xcopy), error %d",
			 rc);
		return rc;
	}
	LogEvent(COMPONENT_THREAD,
		 "COPY fridge (xcopy) started, max_workers=%" PRIu32
		 " (deferment=fail -> sync fallback when full)",
		 nfs_param.nfsv4_param.max_copy_workers);
	return 0;
}

int nfs4_copy_fridge_shutdown(void)
{
	int rc;

	if (copy_fridge == NULL)
		return 0;

	/* Signal all copy_worker threads to stop at the next chunk. */
	atomic_store_uint8_t(&copy_fridge_stopping, 1);
	LogEvent(COMPONENT_THREAD,
		 "COPY fridge: signalled in-flight workers to stop.");

	rc = fridgethr_sync_command(copy_fridge, fridgethr_comm_stop, 30);
	if (rc == ETIMEDOUT) {
		LogMajor(COMPONENT_THREAD,
			 "COPY fridge shutdown timedout, force-cancelling");
		fridgethr_cancel(copy_fridge);
	} else if (rc != 0) {
		LogMajor(COMPONENT_THREAD, "COPY fridge shutdown error: %d",
			 rc);
	} else {
		LogEvent(COMPONENT_THREAD, "COPY fridge shut down.");
	}
	return rc;
}

/**
 * @brief Validate a stateid for COPY source or destination.
 *
 * Checks that the stateid is valid and that the open/lock/delegation
 * state allows read (source) or write (destination) access.
 *
 * @param[in]  sid        Stateid from the client
 * @param[in]  obj        File handle being accessed
 * @param[in]  data       Compound request data
 * @param[in]  need_write TRUE for destination (write check)
 * @param[out] state_out  Resolved state_t (caller must dec_state_t_ref)
 *
 * @return NFS4_OK on success, error code otherwise
 */
static nfsstat4 check_copy_stateid(stateid4 *sid, struct fsal_obj_handle *obj,
				   compound_data_t *data, bool need_write,
				   state_t **state_out)
{
	state_t *state = NULL;
	state_t *state_open = NULL;
	nfsstat4 status;

	status = nfs4_Check_Stateid(sid, obj, &state, data, STATEID_SPECIAL_ANY,
				    0, false,
				    need_write ? "COPY_DST" : "COPY_SRC");
	if (status != NFS4_OK)
		return status;

	if (state != NULL) {
		switch (state->state_type) {
		case STATE_TYPE_SHARE:
			if (need_write &&
			    !(state->state_data.share.share_access &
			      OPEN4_SHARE_ACCESS_WRITE)) {
				dec_state_t_ref(state);
				return NFS4ERR_OPENMODE;
			}
			if (!need_write &&
			    !(state->state_data.share.share_access &
			      OPEN4_SHARE_ACCESS_READ)) {
				dec_state_t_ref(state);
				return NFS4ERR_OPENMODE;
			}
			break;
		case STATE_TYPE_LOCK:
			state_open = nfs4_State_Get_Pointer(
				state->state_data.lock.openstate_key);
			if (state_open == NULL) {
				dec_state_t_ref(state);
				return NFS4ERR_BAD_STATEID;
			}
			if (need_write &&
			    !(state_open->state_data.share.share_access &
			      OPEN4_SHARE_ACCESS_WRITE)) {
				dec_state_t_ref(state_open);
				dec_state_t_ref(state);
				return NFS4ERR_OPENMODE;
			}
			if (!need_write &&
			    !(state_open->state_data.share.share_access &
			      OPEN4_SHARE_ACCESS_READ)) {
				dec_state_t_ref(state_open);
				dec_state_t_ref(state);
				return NFS4ERR_OPENMODE;
			}
			dec_state_t_ref(state_open);
			break;
		case STATE_TYPE_DELEG:
			if (need_write) {
				struct state_deleg *sd =
					&state->state_data.deleg;
				if (sd->sd_type != OPEN_DELEGATE_WRITE &&
				    sd->sd_type !=
					    OPEN_DELEGATE_WRITE_ATTRS_DELEG) {
					return NFS4ERR_BAD_STATEID;
				}
			}
			break;
		default:
			dec_state_t_ref(state);
			return NFS4ERR_BAD_STATEID;
		}
	} else if (state_deleg_conflict(obj, need_write)) {
		return NFS4ERR_DELAY;
	}

	*state_out = state;
	return NFS4_OK;
}

/**
 * @brief state_free callback invoked by dec_state_t_ref when refcount
 *        reaches zero.
 *
 * This is the ONLY place where the copy_offload_state memory is freed.
 * It is invoked automatically through the state_t refcount machinery,
 */
static void copy_offload_state_free(struct state_t *state)
{
	struct copy_offload_state *cos =
		container_of(state, struct copy_offload_state, cos_state);

	/*
	 * Defensive: copy_worker() should have released and NULLed all of
	 * these before firing CB_OFFLOAD.  If the worker crashed mid-flight
	 * these non-NULL guards prevent a double-free / leak.
	 */
	if (cos->cos_src_state) {
		dec_state_t_ref(cos->cos_src_state);
		cos->cos_src_state = NULL;
	}
	if (cos->cos_dst_state) {
		dec_state_t_ref(cos->cos_dst_state);
		cos->cos_dst_state = NULL;
	}
	if (cos->cos_src) {
		cos->cos_src->obj_ops->put_ref(cos->cos_src);
		cos->cos_src = NULL;
	}
	if (cos->cos_dst) {
		cos->cos_dst->obj_ops->put_ref(cos->cos_dst);
		cos->cos_dst = NULL;
	}

	/* Free the destination FH buffer stored for CB_OFFLOAD coa_fh */
	gsh_free(cos->cos_dst_fh.nfs_fh4_val);
	cos->cos_dst_fh.nfs_fh4_val = NULL;

	gsh_free(cos);
}

/**
 * @brief Allocate and register a copy offload state.
 *
 * Sets state_obj and state_owner so nfs4_Check_Stateid() can validate:
 *   state_obj   = dst  — for CURRENT_FH comparison
 *   state_owner = cid_owner — for lease check via so_clientrec
 *
 * IMPORTANT: we set state_obj and state_owner for field access by
 * nfs4_Check_Stateid() only.  We deliberately do NOT add this state to
 * obj->state_hdl->file.list_of_states or cid_owner->so_state_list.
 * Those lists are walked by _state_del_locked() which unconditionally
 * calls obj->obj_ops->close2() (nfs4_state.c:577).  close2 requires a
 * valid op_ctx->fsal_export, but destroy_copy_offload_state() can be
 * called from the RPC callback thread (cb_offload_completion) where
 * op_ctx has no valid fsal_export -> crash in mdc_cur_export().
 *
 * Cleanup is handled manually in destroy_copy_offload_state() using
 * nfs4_State_Del() (hash-only removal) without going through state_del().
 *
 * Refcount starts at 1 (owner's reference).
 *
 * @param data Compound request's data
 * @param dst  Destination fsal_obj_handle (CURRENT_FH at COPY time).
 * @param count Amount of data needs to be copied.
 *
 * @return copy_offload_state structure
 */
static struct copy_offload_state *create_copy_offload_state(
	compound_data_t *data, struct fsal_obj_handle *dst, uint64_t tcount)
{
	struct copy_offload_state *cos;
	nfs_client_id_t *clientid = data->preserved_clientid;

	cos = gsh_malloc(sizeof(struct copy_offload_state));
	cos->cos_state.state_type = STATE_TYPE_COPY_OFFLOAD;
	/*
	 * Start seqid at 0 — same as _state_add_impl() (nfs4_state.c:167).
	 * update_stateid() in nfs4_op_copy() will increment it to 1 and
	 * stamp the correct value into the COPY response stateid.
	 */
	cos->cos_state.state_seqid = 0;
	cos->cos_state.state_refcount = 1; /* owner's reference */
	cos->cos_state.state_free = copy_offload_state_free;
	cos->cos_state.state_export = op_ctx->ctx_export;
	if (cos->cos_state.state_export)
		get_gsh_export_ref(cos->cos_state.state_export);

	/*
	 * state_obj = destination file (with a ref).
	 * Needed by get_state_obj_ref_locked() inside nfs4_Check_Stateid()
	 * to return the obj for CURRENT_FH comparison. Released manually in
	 * destroy_copy_offload_state() under state_mutex, NOT via state_del()
	 */
	cos->cos_state.state_obj = dst;
	dst->obj_ops->get_ref(dst);

	/*
	 * state_owner = cid_owner (STATE_CLIENTID_OWNER_NFSV4).
	 * The copy stateid belongs to the clientid.
	 * nfs4_Check_Stateid() reads owner2->so_nfs4_owner.so_clientrec to
	 * check data->preserved_clientid == clientid (nfs4_state_id.c:1076).
	 * inc_state_owner_ref is the permanent hold on cid_owner that matches
	 * dec_state_owner_ref in destroy_copy_offload_state().
	 * We do NOT add to cid_owner->so_state_list — see comment above.
	 */
	cos->cos_state.state_owner = &clientid->cid_owner;
	inc_state_owner_ref(&clientid->cid_owner);

	/*
	 * state_mutex is required by dec_state_t_ref: it calls
	 * PTHREAD_MUTEX_destroy(&state->state_mutex) when refcount hits 0.
	 */
	PTHREAD_MUTEX_init(&cos->cos_state.state_mutex, NULL);

	/*
	 * glist_head fields must be self-referential (empty-list sentinel).
	 */
	glist_init(&cos->cos_state.state_list);
	glist_init(&cos->cos_state.state_owner_list);
	glist_init(&cos->cos_state.state_export_list);

	cos->cos_clientid = clientid;
	inc_client_id_ref(clientid);
	cos->cos_total_count = tcount;
	cos->cos_status = NFS4_OK;

	{
		struct timespec now;

		clock_gettime(CLOCK_REALTIME, &now);
		cos->cos_start_time.seconds = now.tv_sec;
		cos->cos_start_time.nseconds = now.tv_nsec;
	}

	nfs4_BuildStateId_Other(clientid, cos->cos_state.stateid_other);

	if (nfs4_State_Set(&cos->cos_state) != STATE_SUCCESS) {
		if (cos->cos_state.state_export)
			put_gsh_export(cos->cos_state.state_export);
		dst->obj_ops->put_ref(dst);
		cos->cos_state.state_obj = NULL;
		dec_state_owner_ref(&clientid->cid_owner);
		cos->cos_state.state_owner = NULL;
		dec_client_id_ref(clientid);
		PTHREAD_MUTEX_destroy(&cos->cos_state.state_mutex);
		gsh_free(cos);
		return NULL;
	}

	/*
	 * Capture the destination file handle for CB_OFFLOAD coa_fh.
	 *
	 * Requires coa_fh as the FIRST field of CB_OFFLOAD4args.
	 * At this point data->currentFH is the NFS4 file handle of the
	 * destination file (CURRENT_FH == dst for COPY).
	 * copy it here so it is available after the compound returns
	 *
	 * copy_offload_state_free() frees it on final refcount drop.
	 */
	cos->cos_dst_fh.nfs_fh4_len = data->currentFH.nfs_fh4_len;
	cos->cos_dst_fh.nfs_fh4_val = gsh_malloc(data->currentFH.nfs_fh4_len);
	memcpy(cos->cos_dst_fh.nfs_fh4_val, data->currentFH.nfs_fh4_val,
	       data->currentFH.nfs_fh4_len);

	LogDebug(COMPONENT_NFS_V4, "COPY async: created offload state seqid=%u",
		 cos->cos_state.state_seqid);
	return cos;
}

/**
 * @brief Release the owner's reference on a copy offload state.
 *
 * Uses nfs4_State_Del() (hash-only removal) — NOT state_del().
 *
 * WHY NOT state_del():
 *   state_del() -> _state_del_locked() unconditionally calls
 *   obj->obj_ops->close2() (nfs4_state.c:577).  mdcache_close2() calls
 *   mdc_cur_export() which dereferences op_ctx->fsal_export.  This
 *   function is called from cb_offload_completion() which runs in the
 *   RPC callback thread where op_ctx has no valid fsal_export -> crash.
 *   Additionally, _state_del_locked() calls dec_state_t_ref() as the
 *   "sentinel" release (nfs4_state.c:589).  Our refcount starts at 1
 *   (not 2 as _state_add_impl sets), so that drop would immediately
 *   trigger gsh_free(cos) before we finish this function -> use-after-free.
 *
 * Manual cleanup mirrors _state_del_locked() without the close2 call:
 *   1. nfs4_State_Del       — removes from ht_state_id (no list changes)
 *   2. state_mutex          — clear state_obj + put_ref(dst)
 *   3. dec_state_owner_ref  — drops the permanent hold on cid_owner
 *   4. put_gsh_export       — drops the export ref
 *   5. dec_client_id_ref    — drops the clientid hold taken at create
 *   6. dec_state_t_ref      — drops owner's ref; gsh_free(cos) when last
 */
static void destroy_copy_offload_state(struct copy_offload_state *cos)
{
	struct fsal_obj_handle *dst;
	state_owner_t *owner;
	struct gsh_export *export;

	if (cos == NULL)
		return;

	/* Remove from stateid hash table only. */
	nfs4_State_Del(&cos->cos_state);

	/*
	 * Release state_obj ref under state_mutex so that
	 * get_state_obj_ref_locked() in a concurrent nfs4_Check_Stateid()
	 * cannot race with the NULL assignment.
	 */
	PTHREAD_MUTEX_lock(&cos->cos_state.state_mutex);
	dst = cos->cos_state.state_obj;
	cos->cos_state.state_obj = NULL;
	owner = cos->cos_state.state_owner;
	cos->cos_state.state_owner = NULL;
	export = cos->cos_state.state_export;
	cos->cos_state.state_export = NULL;
	PTHREAD_MUTEX_unlock(&cos->cos_state.state_mutex);

	if (dst)
		dst->obj_ops->put_ref(dst);

	/* Drop the permanent hold on cid_owner taken during create. */
	if (owner)
		dec_state_owner_ref(owner);

	/* Drop the export ref taken during create. */
	if (export)
		put_gsh_export(export);

	/* cos-specific fields not tracked by state_t. */
	if (cos->cos_clientid) {
		dec_client_id_ref(cos->cos_clientid);
		cos->cos_clientid = NULL;
	}

	/*
	 * Drop the owner's reference on state_t.
	 * When refcount reaches zero dec_state_t_ref() calls:
	 *   PTHREAD_MUTEX_destroy(&cos->cos_state.state_mutex)
	 *   copy_offload_state_free() -> gsh_free(cos)
	 * Any concurrent nfs4_Check_Stateid() that bumped the refcount
	 * defers this until its own dec_state_t_ref() fires last.
	 */
	dec_state_t_ref(&cos->cos_state);
}

/**
 * Forward declaration — nfs4_copy_send_cb_offload is defined below but
 * called from cb_offload_completion (retry path). */
static void nfs4_copy_send_cb_offload(struct copy_offload_state *cos,
				      uint64_t bytes_copied, fsal_status_t st,
				      const verifier4 write_verifier);

/**
 * @brief Completion hook called by the RPC machinery after CB_OFFLOAD
 *        is acknowledged (or aborted).
 *
 * If the client returns a transient error  (NFS4ERR_RESOURCE, NFS4ERR_DELAY)
 * the server SHOULD retransmit CB_OFFLOAD.
 * We implement this by retrying up to NFS4_COPY_CB_MAX_RETRIES times using the
 * completion parameters cached in cos->cb_*.
 * Each retry calls nfs4_copy_send_cb_offload(), which registers a new
 * cb_offload_completion hook; on success cos
 * ownership transfers to that new hook, on failure it is destroyed.
 *
 * On NFS_CB_CALL_ABORTED (back-channel down) or after exhausting all
 * retries we destroy the state unconditionally
 *
 * For NFSv4.1 the back-channel slot must be released via
 * nfs41_release_single() BEFORE we retry or destroy, since
 * nfs41_release_single() touches call->chan->source.session which is
 * only valid while 'call' is alive.
 *
 * @param[in] call  The completed (or aborted) RPC call.
 *                  call->call_arg carries the copy_offload_state pointer.
 */
static void cb_offload_completion(rpc_call_t *call)
{
	struct copy_offload_state *cos = call->call_arg;
	bool do_retry = false;

	if (call->states & NFS_CB_CALL_ABORTED) {
		LogWarn(COMPONENT_NFS_V4,
			"CB_OFFLOAD: call aborted (back-channel down) seqid=%u",
			cos ? cos->cos_state.state_seqid : 0);
		/* No retry when the back-channel itself is gone */
	} else {
		nfsstat4 compound_st = call->cbt.v_u.v4.res.status;

		if (compound_st == NFS4_OK) {
			LogDebug(COMPONENT_NFS_V4,
				 "CB_OFFLOAD: acknowledged seqid=%u",
				 cos ? cos->cos_state.state_seqid : 0);
		} else if (cos != NULL &&
			   cos->cos_cb_retry_count < NFS4_COPY_CB_MAX_RETRIES) {
			/*
			 * Transient client error — schedule a retry.
			 * Increment the counter now so cb_offload_completion
			 * on the next call knows how many attempts remain.
			 */
			cos->cos_cb_retry_count++;
			do_retry = true;
			LogWarn(COMPONENT_NFS_V4,
				"CB_OFFLOAD: ret stat %d retry %d/%d seqid=%u",
				compound_st, cos->cos_cb_retry_count,
				NFS4_COPY_CB_MAX_RETRIES,
				cos->cos_state.state_seqid);
		} else {
			LogWarn(COMPONENT_NFS_V4,
				"CB_OFFLOAD: ret stat %d giving up seqid=%u",
				compound_st,
				cos ? cos->cos_state.state_seqid : 0);
		}
	}

	/*
	 * For NFSv4.1, release the back-channel slot BEFORE any retry or
	 * destroy.  nfs41_release_single() touches call->chan->source.session
	 * which must still be valid here.
	 */
	if (cos != NULL && cos->cos_clientid != NULL &&
	    cos->cos_clientid->cid_minorversion != 0)
		nfs41_release_single(call);

	if (do_retry) {
		/* Re-send CB_OFFLOAD */
		nfs4_copy_send_cb_offload(cos, cos->cos_cb_bytes_copied,
					  cos->cos_cb_fsal_status,
					  cos->cos_cb_write_verifier);
		return;
	}

	destroy_copy_offload_state(cos);
}

/**
 * @brief Build and dispatch a CB_OFFLOAD callback for a completed copy.
 *
 * On success the copy_offload_state ownership is transferred to
 * cb_offload_completion (which calls destroy_copy_offload_state).
 * On send failure we call destroy_copy_offload_state directly so the
 * state is always cleaned up.
 *
 * @param[in] cos            Copy offload state (caller gives up ownership)
 * @param[in] bytes_copied   Bytes successfully transferred
 * @param[in] st             Final FSAL status (ERR_FSAL_NO_ERROR on success)
 * @param[in] write_verifier Server write verifier captured while op_ctx
 *                           was still valid (only meaningful when !error)
 */
static void nfs4_copy_send_cb_offload(struct copy_offload_state *cos,
				      uint64_t bytes_copied, fsal_status_t st,
				      const verifier4 write_verifier)
{
	nfs_cb_argop4 op;
	CB_OFFLOAD4args *args;
	int rc;

	if (cos == NULL)
		return;

	if (cos->cos_clientid == NULL) {
		LogWarn(COMPONENT_NFS_V4,
			"CB_OFFLOAD: no clientid, skipping callback");
		destroy_copy_offload_state(cos);
		return;
	}

	memset(&op, 0, sizeof(op));
	op.argop = NFS4_OP_CB_OFFLOAD;
	args = &op.nfs_cb_argop4_u.opcboffload;

	args->coa_fh = cos->cos_dst_fh;
	COPY_STATEID(&args->coa_stateid, &cos->cos_state);

	if (FSAL_IS_ERROR(st)) {
		/*
		 * On error:
		 * coa_status  : carries the error
		 * coa_bytes_copied : carries how many bytes we
		 * managed to copy before the failure/cancellation.
		 */
		args->coa_status = nfs4_Errno_status(st);
		args->coa_payload.coa_bytes_copied = (length4)bytes_copied;
		LogDebug(COMPONENT_NFS_V4,
			 "CB_OFFLOAD: sending error status=%d "
			 "bytes_copied=%" PRIu64 " seqid=%u",
			 args->coa_status, bytes_copied,
			 cos->cos_state.state_seqid);
	} else {
		/* success */
		args->coa_status = NFS4_OK;
		args->coa_payload.coa_resok4.wr_ids = 0;
		args->coa_payload.coa_resok4.wr_count = (length4)bytes_copied;
		args->coa_payload.coa_resok4.wr_committed = FILE_SYNC4;
		memcpy(args->coa_payload.coa_resok4.wr_writeverf,
		       write_verifier, sizeof(verifier4));

		LogDebug(COMPONENT_NFS_V4,
			 "CB_OFFLOAD: sending OK bytes=%" PRIu64 " seqid=%u",
			 bytes_copied, cos->cos_state.state_seqid);
	}

	/*
	 * Dispatch the callback.  On success, ownership of cos transfers
	 * to cb_offload_completion.  On failure, we clean up here.
	 */
	rc = nfs_rpc_cb_single(cos->cos_clientid, &op, NULL,
			       cb_offload_completion, cos);
	if (rc != 0) {
		LogWarn(COMPONENT_NFS_V4,
			"CB_OFFLOAD: failed (%d) destroying state directly",
			rc);
		destroy_copy_offload_state(cos);
	}
}

/**
 * @brief Check whether the copy worker should continue to the next chunk.
 *
 * Returns true  — continue copying.
 * Returns false — stop; logs the specific reason.
 *
 * Checked at the top of every chunk iteration so server
 * shutdown honoured promptly without waiting for a full chunk.
 */
static bool copy_worker_should_continue(const struct copy_offload_state *cos)
{
	if (atomic_fetch_uint8_t(&copy_fridge_stopping)) {
		LogEvent(COMPONENT_THREAD,
			 "COPY worker stopping, server shutdown in progress");
		return false;
	}

	if (cos->cos_clientid != NULL &&
	    cos->cos_clientid->cid_confirmed == EXPIRED_CLIENT_ID) {
		LogDebug(COMPONENT_NFS_V4,
			 "client (id=%" PRIx64 ") lease expired or was revoked",
			 cos->cos_clientid->cid_clientid);
		return false;
	}

	return true;
}

/**
 * @brief fridgethr worker that performs the actual data copy.
 *
 * ASYNC path:
 *  - The COPY response was already sent to the client (wr_ids=1,
 *    cr_synchronous=FALSE) before this worker was dispatched.
 *  - This worker copies data in copy_chunk_size chunks so that
 *    shutdown is honoured promptly between chunks.
 *  - All execution context (src, dst, state refs, export, offsets) lives
 *    in the cos worker fields populated by copy_run_async().
 *  - When done (success, FSAL error) this worker
 *    releases all worker refs, fires CB_OFFLOAD, and returns.  cos itself
 *    is freed when its SAL refcount drops to zero — no gsh_free here.
 */
static void copy_worker(struct fridgethr_context *fridge_ctx)
{
	struct copy_offload_state *cos = fridge_ctx->arg;
	uint64_t remaining = cos->cos_total_count;
	uint64_t src_off = cos->cos_src_off;
	uint64_t dst_off = cos->cos_dst_off;
	uint64_t bytes_copied = 0;
	uint64_t chunk_num = 0;
	fsal_status_t st = { ERR_FSAL_NO_ERROR, 0 };
	verifier4 write_verifier;
	struct req_op_context op_context;
	struct timespec t_start, t_end;
	nsecs_elapsed_t elapsed_ns;
	uint64_t elapsed_ms, rate_kbps;

	/* Claim a unique per-thread name ("xcopy0", "xcopy1", …) once. */
	if (!copy_thr_named) {
		char thr_name[32];
		uint32_t my_id = atomic_postadd_uint32_t(&copy_thr_next_id, 1);

		snprintf(thr_name, sizeof(thr_name), "xcopy%" PRIu32, my_id);
		SetNameFunction(thr_name);
		copy_thr_named = true;
	}

	/*
	 * Establish a minimal request context so mdcache / FSAL layers
	 * can read op_ctx->fsal_export.  Export pointers were captured at
	 * submission time in copy_run_async and stored in cos.
	 */
	init_op_context_simple(&op_context, cos->cos_ctx_export,
			       cos->cos_fsal_export);

	memset(write_verifier, 0, sizeof(write_verifier));

	now_mono(&t_start);
	LogDebug(COMPONENT_NFS_V4,
		 "COPY async worker START: total=%" PRIu64 " bytes "
		 "src_off=%" PRIu64 " dst_off=%" PRIu64 " chunk_size=%" PRIu64,
		 cos->cos_total_count, cos->cos_src_off, cos->cos_dst_off,
		 (uint64_t)copy_chunk_size);

	while (remaining > 0) {
		uint64_t chunk = MIN(remaining, copy_chunk_size);
		uint64_t chunk_copied = 0;

		if (!copy_worker_should_continue(cos)) {
			st = fsalstat(ERR_FSAL_INTERRUPT, 0);
			break;
		}

		LogFullDebug(COMPONENT_NFS_V4,
			     "COPY chunk #%" PRIu64 ": src_off=%" PRIu64
			     " len=%" PRIu64 " progress=%" PRIu64 "/%" PRIu64,
			     chunk_num, src_off, chunk, bytes_copied,
			     cos->cos_total_count);

		st = cos->cos_src->obj_ops->copy_file_range(
			cos->cos_src, cos->cos_src_state, cos->cos_dst,
			cos->cos_dst_state, src_off, dst_off, chunk,
			&chunk_copied);
		if (FSAL_IS_ERROR(st)) {
			LogWarn(COMPONENT_NFS_V4,
				"COPY async worker FSAL ERR at chunk #%" PRIu64
				" byte_offset=%" PRIu64 ": major=%u minor=%u"
				" (copied so far: %" PRIu64 " of %" PRIu64 ")",
				chunk_num, src_off, st.major, st.minor,
				bytes_copied, cos->cos_total_count);
			break;
		}

		bytes_copied += chunk_copied;
		src_off += chunk_copied;
		dst_off += chunk_copied;
		remaining -= chunk_copied;
		chunk_num++;

		if (chunk_copied < chunk) {
			LogDebug(COMPONENT_NFS_V4,
				 "COPY async worker short chunk at #%" PRIu64
				 ": requested=%" PRIu64 " got=%" PRIu64
				 " (EOF or short write) - stopping",
				 chunk_num - 1, chunk, chunk_copied);
			break;
		}
	}

	now_mono(&t_end);
	elapsed_ns = timespec_diff(&t_start, &t_end);
	elapsed_ms = (uint64_t)(elapsed_ns / 1000000ULL);
	rate_kbps = elapsed_ms > 0
			    ? (bytes_copied * 1000ULL / elapsed_ms / 1024ULL)
			    : 0;

	if (FSAL_IS_ERROR(st)) {
		LogWarn(COMPONENT_NFS_V4,
			"COPY async worker DONE with ERROR: copied=%" PRIu64
			" of %" PRIu64 " bytes in %" PRIu64 " ms (%" PRIu64
			" KB/s) FSAL error major=%u minor=%u",
			bytes_copied, cos->cos_total_count, elapsed_ms,
			rate_kbps, st.major, st.minor);
	} else {
		LogDebug(COMPONENT_NFS_V4,
			 "COPY async worker DONE OK: copied=%" PRIu64
			 " bytes in %" PRIu64 " ms (%" PRIu64
			 " KB/s) chunks=%" PRIu64,
			 bytes_copied, elapsed_ms, rate_kbps, chunk_num);
	}

	cos->cos_status = FSAL_IS_ERROR(st) ? nfs4_Errno_status(st) : NFS4_OK;
	atomic_store_uint64_t(&cos->cos_bytes_copied, bytes_copied);
	atomic_store_uint8_t(&cos->cos_complete, 1);

	/*
	 * Capture the write verifier while op_ctx is still valid.
	 * Required for the CB_OFFLOAD success payload (write_response4).
	 */
	if (!FSAL_IS_ERROR(st)) {
		struct gsh_buffdesc verf_desc;

		verf_desc.addr = write_verifier;
		verf_desc.len = sizeof(verifier4);
		op_ctx->fsal_export->exp_ops.get_write_verifier(
			op_ctx->fsal_export, &verf_desc);
	}

	/*
	 * Cache CB_OFFLOAD parameters in cos for retry.
	 * Written once here before the first dispatch; read-only after that.
	 */
	cos->cos_cb_bytes_copied = bytes_copied;
	cos->cos_cb_fsal_status = st;
	memcpy(cos->cos_cb_write_verifier, write_verifier, sizeof(verifier4));

	/*
	 * Release all worker execution refs in cos.  NULL each field after
	 * release so copy_offload_state_free()'s defensive checks are no-ops.
	 */
	dec_state_t_ref(cos->cos_src_state);
	cos->cos_src_state = NULL;
	dec_state_t_ref(cos->cos_dst_state);
	cos->cos_dst_state = NULL;
	cos->cos_src->obj_ops->put_ref(cos->cos_src);
	cos->cos_src = NULL;
	cos->cos_dst->obj_ops->put_ref(cos->cos_dst);
	cos->cos_dst = NULL;

	/*
	 * Release op_ctx.  release_op_context() calls put_gsh_export on
	 * ctx_export, consuming the ref taken in copy_run_async.
	 */
	release_op_context();
	cos->cos_ctx_export = NULL;
	cos->cos_fsal_export = NULL;

	/*
	 * Fire CB_OFFLOAD.  On success, ownership of cos transfers to
	 * cb_offload_completion.  On CB send failure, cos is destroyed
	 * inside nfs4_copy_send_cb_offload.
	 * cos is freed when its SAL refcount drops to zero, no gsh_free here.
	 */
	nfs4_copy_send_cb_offload(cos, bytes_copied, st, write_verifier);
}

/** Result of the async dispatch attempt. */
enum copy_async_dispatch {
	COPY_ASYNC_DISPATCHED,
	COPY_ASYNC_FALLBACK_SYNC,
};

static void copy_log_request(const COPY4args *args)
{
	/*
	 * ca_synchronous == TRUE  -> client demands inline copy; server MUST
	 *                           honour it or return NFS4ERR_OFFLOAD_NO_REQS
	 *                           with cr_synchronous=FALSE.
	 * ca_synchronous == FALSE -> client allows async; server MAY choose
	 *                           async (returns wr_ids=1,
	 *                           cr_synchronous=FALSE + copy stateid)
	 *                           OR
	 *                           fall back to sync.
	 */
	LogDebug(COMPONENT_NFS_V4,
		 "COPY request: ca_src_off=%" PRIu64 " ca_dst_off=%" PRIu64
		 " ca_count=%" PRIu64 " ca_con=%d ca_sync=%d ca_src_ser_len=%u",
		 args->ca_src_offset, args->ca_dst_offset, args->ca_count,
		 (int)args->ca_consecutive, (int)args->ca_synchronous,
		 args->ca_source_server_len);
}

/**
 * @brief Validate handles, stateids, access, and compute copy length.
 *
 * Writes into the already-allocated @a ctx (zeroed by the caller).
 *
 * On success the caller owns ctx->src_state / ctx->dst_state and MUST
 * release them via dec_state_t_ref().
 *
 * On error this function releases any state refs it has already acquired
 * before returning, so the caller never needs to inspect ctx on failure.
 */
static nfsstat4 copy_validate(COPY4args *args, compound_data_t *data,
			      struct copy_ctx *ctx)
{
	struct fsal_attrlist attrs;
	struct saved_export_context saved_src_ctx = { NULL };
	fsal_status_t fsal_st;
	uint64_t src_size = 0;
	uint64_t to_copy = args->ca_count;
	uint64_t MaxOffsetWrite;
	nfsstat4 status;

	ctx->src_off = args->ca_src_offset;
	ctx->dst_off = args->ca_dst_offset;

	if (args->ca_source_server_len > 0)
		return NFS4ERR_NOTSUPP;

	status = nfs4_sanity_check_FH(data, REGULAR_FILE, false);
	if (status != NFS4_OK)
		return status;

	status = nfs4_sanity_check_saved_FH(data, REGULAR_FILE, false);
	if (status != NFS4_OK)
		return status;

	ctx->src = data->saved_obj;
	ctx->dst = data->current_obj;

	if (ctx->src == ctx->dst)
		return NFS4ERR_INVAL;

	status = check_copy_stateid(&args->ca_src_stateid, ctx->src, data,
				    false, &ctx->src_state);
	if (status != NFS4_OK)
		return status;

	status = check_copy_stateid(&args->ca_dst_stateid, ctx->dst, data, true,
				    &ctx->dst_state);
	if (status != NFS4_OK)
		goto err_release;

	fsal_st = ctx->src->obj_ops->test_access(ctx->src, FSAL_READ_ACCESS,
						 NULL, NULL, true);
	if (FSAL_IS_ERROR(fsal_st)) {
		status = nfs4_Errno_status(fsal_st);
		goto err_release;
	}

	fsal_st = ctx->dst->obj_ops->test_access(ctx->dst, FSAL_WRITE_ACCESS,
						 NULL, NULL, true);
	if (FSAL_IS_ERROR(fsal_st)) {
		status = nfs4_Errno_status(fsal_st);
		goto err_release;
	}

	/*
	 * Cross-FSAL guard: op_ctx->fsal_export currently points at the dst
	 * export (set by the preceding PUTFH for the copy destination).
	 * Temporarily switch op_ctx to the saved (src) export for this call.
	 */
	get_gsh_export_ref(data->saved_export);
	save_op_context_export_and_set_export(&saved_src_ctx,
					      data->saved_export);
	op_ctx->export_perms = data->saved_export_perms;
	fsal_prepare_attrs(&attrs, ATTR_SIZE);
	fsal_st = ctx->src->obj_ops->getattrs(ctx->src, &attrs);
	restore_op_context_export(&saved_src_ctx);

	if (FSAL_IS_ERROR(fsal_st)) {
		fsal_release_attrs(&attrs);
		status = nfs4_Errno_status(fsal_st);
		goto err_release;
	}
	if (FSAL_TEST_MASK(attrs.valid_mask, ATTR_SIZE))
		src_size = attrs.filesize;
	fsal_release_attrs(&attrs);

	if (args->ca_count == 0) {
		to_copy = (ctx->src_off < src_size) ? (src_size - ctx->src_off)
						    : 0;
	} else if (ctx->src_off >= src_size ||
		   ctx->src_off + to_copy > src_size) {
		/*
		 * If the source offset or the source offset plus count
		 * is greater than the size of the source file, the
		 * operation MUST fail with NFS4ERR_INVAL.
		 * Silently clipping the range is NOT permitted.
		 */
		status = NFS4ERR_INVAL;
		goto err_release;
	}

	ctx->to_copy = to_copy;
	if (to_copy == 0)
		return NFS4_OK;

	MaxOffsetWrite =
		atomic_fetch_uint64_t(&op_ctx->ctx_export->MaxOffsetWrite);
	if (MaxOffsetWrite < UINT64_MAX &&
	    ctx->dst_off + to_copy > MaxOffsetWrite) {
		status = NFS4ERR_FBIG;
		goto err_release;
	}

	return NFS4_OK;

err_release:
	if (ctx->src_state != NULL) {
		dec_state_t_ref(ctx->src_state);
		ctx->src_state = NULL;
	}
	if (ctx->dst_state != NULL) {
		dec_state_t_ref(ctx->dst_state);
		ctx->dst_state = NULL;
	}
	return status;
}

/**
 * @brief Decide whether to use the async offload path.
 *
 * Async requires ALL of:
 *   1. Copy_Offload = true    (allow_copy_offload)
 *   2. ca_synchronous == FALSE (client has NOT required inline copy)
 *   3. size >= Copy_Offload_Min_Size
 *
 * NFSv4.0 clients with cb_chan_down=true fall back to sync so CB_OFFLOAD
 * is not silently lost.
 */
static bool copy_want_async(const COPY4args *args, compound_data_t *data,
			    uint64_t to_copy)
{
	bool do_async = false;
	nfs_client_id_t *clid = data->preserved_clientid;
	uint32_t minorver;

	do_async = nfs_param.nfsv4_param.allow_copy_offload &&
		   !args->ca_synchronous &&
		   (to_copy >= nfs_param.nfsv4_param.copy_offload_min_size);

	if (!do_async || clid == NULL)
		return do_async;

	minorver = clid->cid_minorversion;

	if (minorver == 0 && get_cb_chan_down(clid)) {
		LogWarn(COMPONENT_NFS_V4,
			"COPY async failed clientid %" PRIx64
			"falling back to synchronous copy",
			clid->cid_clientid);
		return false;
	}

	LogDebug(COMPONENT_NFS_V4,
		 "COPY back-channel check: minor_ver=%" PRIu32
		 "%s - back-channel %s for CB_OFFLOAD",
		 minorver,
		 minorver == 0	 ? " (NFSv4.0)"
		 : minorver == 1 ? " (NFSv4.1/session)"
				 : " (NFSv4.2/session)",
		 (minorver == 0 && get_cb_chan_down(clid)) ? "DOWN (will abort)"
							   : "UP");

	return do_async;
}

static void copy_log_dispatch(const COPY4args *args, uint64_t to_copy,
			      bool do_async)
{
	LogDebug(COMPONENT_NFS_V4,
		 "COPY dispatch: to_copy=%" PRIu64
		 " ca_consecutive=%d ca_synchronous=%d"
		 " allow_offload=%d min_size=%" PRIu64 " -> %s"
		 " [SERVER RESPONSE will have: wr_ids=%d cr_synchronous=%d]",
		 to_copy, (int)args->ca_consecutive, (int)args->ca_synchronous,
		 (int)nfs_param.nfsv4_param.allow_copy_offload,
		 nfs_param.nfsv4_param.copy_offload_min_size,
		 do_async ? "ASYNC (CB_OFFLOAD)" : "SYNC (inline)",
		 do_async ? 1 : 0, do_async ? 0 : 1);
}

/**
 * @brief Fill COPY4resok — the single place that stamps the RPC reply.
 *
 * Sync (RFC 7862 15.2.3):
 *   wr_ids=0, wr_count=<bytes>, FILE_SYNC4, cr_synchronous=TRUE
 *
 * Async (RFC 7862 15.2.3):
 *   wr_ids=1, wr_callback_id=<copy stateid>, wr_count=0, UNSTABLE4,
 *   cr_synchronous=FALSE; CB_OFFLOAD follows when the worker finishes.
 */
static void fill_copy_response(COPY4res *res, compound_data_t *data, bool async,
			       struct copy_offload_state *cos,
			       uint64_t sync_bytes_copied)
{
	COPY4resok *resok = &res->COPY4res_u.cr_resok4;

	res->cr_status = NFS4_OK;
	resok->cr_consecutive = TRUE;

	if (async) {
		resok->cr_response.wr_ids = 1;

		/*
		 * update_stateid() increments cos->cos_state.state_seqid from
		 * 0 -> 1 and stamps both the response stateid and
		 * data->current_stateid, exactly as nfs4_op_open() does.
		 * This is the standard SAL convention for seqid initialisation
		 * — do NOT use COPY_STATEID() here.
		 */
		update_stateid_locked(&cos->cos_state,
				      &resok->cr_response.wr_callback_id, data,
				      "COPY-OFFLOAD");

		resok->cr_response.wr_count = 0;
		resok->cr_response.wr_committed = UNSTABLE4;
		memset(resok->cr_response.wr_writeverf, 0, sizeof(verifier4));
		resok->cr_synchronous = FALSE;

		LogDebug(COMPONENT_NFS_V4,
			 "COPY async RESPONSE: wr_ids=1 to_copy=%" PRIu64
			 "cr_sync=FALSE cr_cons=TRUE copy_stateid_seqid=%u",
			 cos->cos_total_count, cos->cos_state.state_seqid);
		return;
	} else {
		struct gsh_buffdesc verf_desc;

		resok->cr_response.wr_ids = 0;
		resok->cr_response.wr_count = (length4)sync_bytes_copied;
		resok->cr_response.wr_committed = FILE_SYNC4;

		verf_desc.addr = resok->cr_response.wr_writeverf;
		verf_desc.len = sizeof(verifier4);
		op_ctx->fsal_export->exp_ops.get_write_verifier(
			op_ctx->fsal_export, &verf_desc);

		resok->cr_synchronous = TRUE;

		LogDebug(COMPONENT_NFS_V4,
			 "COPY sync response: wr_count=%" PRIu64
			 " wr_ids=0 cr_synchronous=TRUE",
			 sync_bytes_copied);
	}
}

/**
 * @brief Execute the inline (synchronous) copy on the service thread.
 *
 * Pure work function: calls copy_file_range and returns the byte count.
 * Does NOT touch the RPC response — the caller fills it via
 * fill_copy_response() so that all reply stamping is visible in one place.
 */
static nfsstat4 copy_run_sync(const struct copy_ctx *ctx, uint64_t *copied_out)
{
	fsal_status_t fsal_st;

	fsal_st = ctx->src->obj_ops->copy_file_range(ctx->src, ctx->src_state,
						     ctx->dst, ctx->dst_state,
						     ctx->src_off, ctx->dst_off,
						     ctx->to_copy, copied_out);
	if (FSAL_IS_ERROR(fsal_st))
		return nfs4_Errno_status(fsal_st);

	LogDebug(COMPONENT_NFS_V4,
		 "COPY sync done: src_off=%" PRIu64 " dst_off=%" PRIu64
		 " req=%" PRIu64 " copied=%" PRIu64,
		 ctx->src_off, ctx->dst_off, ctx->to_copy, *copied_out);

	return NFS4_OK;
}

/**
 * @brief Create a copy offload state, populate its worker fields, and submit
 *        it to the fridge.
 *
 * On COPY_ASYNC_DISPATCHED:
 *   - cos worker fields are owned by the worker thread; caller must not
 *     touch them again.
 *   - *cos_out is set; caller passes it to fill_copy_response().
 *   - ctx->src_state / dst_state are NULLed here (ownership transferred to
 *     cos->cos_src_state / dst_state).
 *
 * On COPY_ASYNC_FALLBACK_SYNC:
 *   - *cos_out is NULL; all cos worker refs are undone here.
 *   - ctx is untouched; caller proceeds to copy_run_sync().
 */
static enum copy_async_dispatch
copy_run_async(compound_data_t *data, struct copy_ctx *ctx,
	       struct copy_offload_state **cos_out)
{
	struct copy_offload_state *cos;
	int submit_rc;

	*cos_out = NULL;

	cos = create_copy_offload_state(data, ctx->dst, ctx->to_copy);
	if (cos == NULL) {
		LogWarn(COMPONENT_NFS_V4,
			"COPY : failed offloadstate, falling back to sync");
		return COPY_ASYNC_FALLBACK_SYNC;
	}

	/*
	 * Populate the worker execution fields inside cos.
	 *
	 * Extra FSAL refs for src and dst: the compound returns immediately
	 * after this function and releases data->saved_obj / data->current_obj;
	 * the worker keeps independent refs via cos->cos_src and cos->cos_dst.
	 * dst is also held by cos->cos_state.state_obj (the SAL ref taken in
	 * create_copy_offload_state), but the worker gets its own ref so it
	 * never reaches into the state_t internals.
	 *
	 * State ref ownership: src_state/dst_state are pointer-copied into cos
	 * here (not yet transferred, keep them in ctx for the fallback path).
	 * Ownership moves to cos only on successful fridgethr_submit.
	 */
	ctx->src->obj_ops->get_ref(ctx->src);
	ctx->dst->obj_ops->get_ref(ctx->dst);
	cos->cos_src = ctx->src;
	cos->cos_dst = ctx->dst;
	cos->cos_src_state = ctx->src_state;
	cos->cos_dst_state = ctx->dst_state;
	cos->cos_src_off = ctx->src_off;
	cos->cos_dst_off = ctx->dst_off;
	cos->cos_ctx_export = op_ctx->ctx_export;
	cos->cos_fsal_export = op_ctx->fsal_export;
	if (cos->cos_ctx_export)
		get_gsh_export_ref(cos->cos_ctx_export);

	/* Worker receives cos directly — no wrapper ctx needed. */
	submit_rc = fridgethr_submit(copy_fridge, copy_worker, cos);
	if (submit_rc == 0) {
		/*
		 * Transfer complete: cos owns src_state/dst_state now.
		 * Null them in ctx so nfs4_op_copy's out: label skips the dec.
		 */
		ctx->src_state = NULL;
		ctx->dst_state = NULL;
		*cos_out = cos;
		return COPY_ASYNC_DISPATCHED;
	}

	if (submit_rc == EWOULDBLOCK) {
		LogDebug(COMPONENT_NFS_V4,
			 "COPY: all %" PRIu32
			 "workers busy, falling back to sync",
			 nfs_param.nfsv4_param.max_copy_workers);
	} else {
		LogWarn(COMPONENT_NFS_V4,
			"COPY: fridgethr_submit rc=%d, falling back to sync",
			submit_rc);
	}

	/*
	 * Undo the worker field setup so the fallback sync path is clean.
	 * Null src_state/dst_state in cos before destroy so that
	 * destroy_copy_offload_state does not double-free them.
	 */
	cos->cos_src->obj_ops->put_ref(cos->cos_src);
	cos->cos_dst->obj_ops->put_ref(cos->cos_dst);
	cos->cos_src = NULL;
	cos->cos_dst = NULL;
	cos->cos_src_state = NULL;
	cos->cos_dst_state = NULL;
	if (cos->cos_ctx_export)
		put_gsh_export(cos->cos_ctx_export);
	cos->cos_ctx_export = NULL;
	cos->cos_fsal_export = NULL;
	destroy_copy_offload_state(cos);
	return COPY_ASYNC_FALLBACK_SYNC;
}

/**
 * @brief NFS4_OP_COPY — validate, dispatch, and complete the RPC reply.
 *
 * fill_copy_response() is called here for every outcome so that a reader
 * sees all response-field assignments in one function:
 *
 *   to_copy == 0  fill_copy_response(sync, 0 bytes)
 *   async path    fill_copy_response(async, cos)
 *   sync path     fill_copy_response(sync, copied bytes)
 *   error         res->cr_status = <error code>  (no resok filled)
 */
enum nfs_req_result nfs4_op_copy(struct nfs_argop4 *op, compound_data_t *data,
				 struct nfs_resop4 *resp)
{
	COPY4args *const args = &op->nfs_argop4_u.opcopy;
	COPY4res *const res = &resp->nfs_resop4_u.opcopy;
	struct copy_ctx ctx = { 0 };
	nfsstat4 status;
	uint64_t copied = 0;
	bool do_async;

	resp->resop = NFS4_OP_COPY;
	res->cr_status = NFS4_OK;

	copy_log_request(args);

	status = copy_validate(args, data, &ctx);
	if (status != NFS4_OK) {
		res->cr_status = status;
		goto out;
	}

	if (ctx.to_copy == 0) {
		fill_copy_response(res, data, false, NULL, 0);
		goto out;
	}

	do_async = copy_want_async(args, data, ctx.to_copy);
	copy_log_dispatch(args, ctx.to_copy, do_async);

	if (do_async) {
		struct copy_offload_state *cos;

		if (copy_run_async(data, &ctx, &cos) == COPY_ASYNC_DISPATCHED) {
			/*
			 * copy_run_async() nulled ctx.src_state/dst_state
			 * (ownership transferred to cos).  The out: label's
			 * null-guarded dec_state_t_ref calls are no-ops.
			 */
			fill_copy_response(res, data, true, cos, 0);
			goto out;
		}
	}

	status = copy_run_sync(&ctx, &copied);
	if (status == NFS4_OK)
		fill_copy_response(res, data, false, NULL, copied);
	else
		res->cr_status = status;

out:
	/*
	 * Sync path and error path: release stateid refs still held by ctx.
	 * Async dispatch path (future): copy_run_async() already nulled both;
	 * these null-guards are no-ops.
	 * ctx is on the stack — no gsh_free.
	 */
	if (ctx.src_state != NULL)
		dec_state_t_ref(ctx.src_state);
	if (ctx.dst_state != NULL)
		dec_state_t_ref(ctx.dst_state);

	GSH_AUTO_TRACEPOINT(nfs4, op_copy_end, TRACE_INFO,
			    "COPY status={} bytes={}", res->cr_status,
			    res->cr_status,
			    res->cr_status == NFS4_OK ? copied : 0ULL);

	return nfsstat4_to_nfs_req_result(res->cr_status);
}

void nfs4_op_copy_Free(nfs_resop4 *resp)
{
	/* Nothing to do */
}
