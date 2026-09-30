/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (C) 2026, IBM. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file  nfs4_uring_optab.c
 * @brief optabv4_uring[] and the personality hooks.
 *
 * This table is the counterpart of optabv4[] in nfs4_Compound.c, and is
 * indexed the same way: by opcode. It only ever *adds* a way to run an
 * operation; optabv4[] stays the authority for the synchronous path and is
 * still what runs for every row here whose prep_sqe is NULL.
 *
 * A row with prep_sqe == NULL is category A (protocol state) or category C
 * (too complex to express as an SQE). count_batchable_ops() stops on those,
 * so an unfilled row simply means "this operation is never batched" — the
 * compound still produces the identical response, just sequentially. That
 * is what makes it safe to enable operations one at a time.
 */

#include "config.h"

#ifdef USE_FSAL_URING

#include "fsal.h"
#include "FSAL/fsal_uring.h"
#include "FSAL/fsal_uring_fops.h"
#include "FSAL_URING/uring_pers.h"
#include "log.h"
#include "nfs_proto_functions.h"

#include "nfs4_uring_internal.h"

/* -------------------------------------------------------------------------
 * Credentials
 * -------------------------------------------------------------------------
 * Deliberately absent from this table.
 *
 * io_uring runs an SQE as whichever thread submitted it, so a batched
 * operation needs a personality or it executes as the Ganesha daemon. But
 * which identity an SQE needs is a property of how the storage layer
 * expresses the operation, not of the NFS opcode that asked for it: a
 * backend whose command payload already carries the caller must not also
 * stamp a personality, and one acting on a control descriptor it opened
 * itself has no client identity to apply.
 *
 * So the policy lives on the FSAL's fop descriptor as pers_resolve(), is
 * resolved per SQE, and defaults to fsal_uring_pers_caller(). See
 * include/FSAL/fsal_uring_fops.h.
 */

/* -------------------------------------------------------------------------
 * optabv4_uring[]
 * -------------------------------------------------------------------------
 * Rows are added by the per-operation subtasks. Until a row has both
 * prep_sqe and finish_cqe it stays NULL here and the operation runs through
 * optabv4[] exactly as before.
 */

const struct nfs4_uring_op_desc optabv4_uring[] = {
	/* Index 0 and the unfilled indices are category A/C by construction:
	 * a zeroed row has prep_sqe == NULL.
	 */
	[0] = {
		.name = "OP_ILLEGAL",
		.fop = FSAL_URING_FOP_LAST,
		.prep_sqe = NULL,
		.finish_cqe = NULL,
		.max_slots = 0,
	},

	/* --- Category B, pending their per-operation subtask --------------
	 *
	 * Each of these gets .prep_sqe, .finish_cqe and
	 * .max_slots filled in by its own subtask. The .fop mapping is
	 * already fixed because it is what the FSAL vector is keyed on, and
	 * is what decides which IORING_OP_* the storage layer will emit.
	 */
	[NFS4_OP_GETATTR] = {
		.name = "OP_GETATTR",
		.fop = FSAL_URING_FOP_STATX,
		.prep_sqe = nfs4_op_getattr_uring_prep,
		.finish_cqe = nfs4_op_getattr_uring_finish,
		.abort_prep = nfs4_op_getattr_uring_abort,
		.max_slots = 1,
	},
	[NFS4_OP_READ] = {
		.name = "OP_READ",
		.fop = FSAL_URING_FOP_READ,
		.prep_sqe = nfs4_op_read_uring_prep,
		.finish_cqe = nfs4_op_read_uring_finish,
		.abort_prep = nfs4_op_read_uring_abort,
		.max_slots = 1,
	},
	[NFS4_OP_WRITE] = {
		.name = "OP_WRITE",
		.fop = FSAL_URING_FOP_WRITE,
		/* A stable write is WRITEV plus a linked sync. */
		.prep_sqe = nfs4_op_write_uring_prep,
		.finish_cqe = nfs4_op_write_uring_finish,
		.abort_prep = nfs4_op_write_uring_abort,
		.max_slots = 2,
	},
	[NFS4_OP_COMMIT] = {
		.name = "OP_COMMIT",
		.fop = FSAL_URING_FOP_SYNC,
		.prep_sqe = nfs4_op_commit_uring_prep,
		.finish_cqe = nfs4_op_commit_uring_finish,
		.abort_prep = nfs4_op_commit_uring_abort,
		.max_slots = 1,
	},
	[NFS4_OP_ACCESS] = {
		.name = "OP_ACCESS",
		.fop = FSAL_URING_FOP_STATX,
		.prep_sqe = nfs4_op_access_uring_prep,
		.finish_cqe = nfs4_op_access_uring_finish,
		.abort_prep = nfs4_op_access_uring_abort,
		.max_slots = 1,
	},
	[NFS4_OP_CLOSE] = {
		.name = "OP_CLOSE",
		.fop = FSAL_URING_FOP_CLOSE,
		/*
		 * Deliberately NOT batched, despite being the obvious
		 * candidate the design called for.
		 *
		 * SAL owns the descriptor. _state_del_locked()
		 * (SAL/nfs4_state.c:577) ends open-state teardown with an
		 * unconditional obj_ops->close2(), and there is no SAL entry
		 * point that tears the state down while leaving the fd
		 * alone. An IORING_OP_CLOSE in the chain plus that close2()
		 * is a double close of the same descriptor number, in a
		 * daemon that is opening files on other threads — so the
		 * second close lands on an unrelated file. Silent and
		 * unbounded.
		 *
		 * The ordering is also backwards: the SQE necessarily runs
		 * before finish_cqe, so the fd would die before the state is
		 * released, leaving a live stateid pointing at a recycled
		 * descriptor for the length of that window.
		 *
		 * The payoff would have been near zero anyway: close(2) on a
		 * regular file does not wait for writeback, and the
		 * expensive half of an NFSv4 CLOSE is the SAL teardown,
		 * which must run on a worker under the state lock either
		 * way. FSAL_VFS_URING independently refuses this operation
		 * in vfs_uring_op_supported().
		 *
		 * Batching it needs: a SAL teardown that skips close2(), an
		 * FSAL "detach fd from this state" call usable from prep,
		 * and a policy for a CLOSE whose SQE fails.
		 */
		.prep_sqe = NULL,
		.max_slots = 0,
	},
	[NFS4_OP_ALLOCATE] = {
		.name = "OP_ALLOCATE",
		.fop = FSAL_URING_FOP_FALLOCATE,
		.prep_sqe = nfs4_op_allocate_uring_prep,
		.finish_cqe = nfs4_op_allocate_uring_finish,
		.abort_prep = nfs4_op_allocate_uring_abort,
		.max_slots = 1,
	},
	[NFS4_OP_DEALLOCATE] = {
		.name = "OP_DEALLOCATE",
		.fop = FSAL_URING_FOP_FALLOCATE,
		.prep_sqe = nfs4_op_deallocate_uring_prep,
		.finish_cqe = nfs4_op_deallocate_uring_finish,
		.abort_prep = nfs4_op_deallocate_uring_abort,
		.max_slots = 1,
	},

	/* --- Category C, deliberately never batched -----------------------
	 *
	 * Listed explicitly rather than left implicit so the reason is on
	 * the record.
	 */
	[NFS4_OP_OPEN] = {
		.name = "OP_OPEN",
		.fop = FSAL_URING_FOP_LAST,
		/* Drives the SAL open-state machine and may create state; it
		 * has to run on a worker with op_ctx.
		 */
		.prep_sqe = NULL,
		.max_slots = 0,
	},
	[NFS4_OP_LOOKUP] = {
		.name = "OP_LOOKUP",
		.fop = FSAL_URING_FOP_LAST,
		/* obj_ops->lookup() allocates a new fsal_obj_handle and an
		 * MDCACHE entry, needing op_ctx and export references that do
		 * not exist on a reaper thread. Also moves current_obj, which
		 * would invalidate the rest of a planned run.
		 */
		.prep_sqe = NULL,
		.max_slots = 0,
	},

	/* Terminator so the array is at least as long as the largest opcode
	 * the engine will index. nfs4_uring_op_batchable() is only ever
	 * called with an opcode already clamped to LastOpcode[minorversion].
	 */
	[NFS4_OP_REMOVEXATTR] = {
		.name = "OP_REMOVEXATTR",
		.fop = FSAL_URING_FOP_LAST,
		.prep_sqe = NULL,
		.max_slots = 0,
	},
};

#endif /* USE_FSAL_URING */
