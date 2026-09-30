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
 * @file  fsal_uring_fops.h
 * @brief Pluggable per-operation io_uring vector for FSALs.
 *
 * The compound engine does not know how any particular filesystem expresses
 * an operation as an SQE.  A POSIX backend writes IORING_OP_READV against a
 * file descriptor; GPFS writes IORING_OP_URING_CMD carrying an OPENHANDLE_*
 * command against its device fd.  Both must be drivable from the same
 * planner, so each FSAL publishes a vector of per-operation handlers and the
 * planner only ever asks abstract questions:
 *
 *     "can you batch a READ?"   -> fsal_uring_fop_can_batch()
 *     "fill this chain slot"    -> desc->prep()
 *     "turn this CQE into a result" -> desc->finish()
 *
 * Two properties this buys us:
 *
 *  1. Adding an operation is a new table entry.  The set of IORING_OP_*
 *     opcodes grows every kernel release; supporting a new one means adding
 *     an enum value and one row per FSAL, with no change to the engine, the
 *     planner, or any other operation.
 *
 *  2. Each operation owns its own completion.  Unlike the original
 *     read2/write2 integration, where one shared done callback had to know
 *     about reads, writes, stable-write fsyncs and deferred fd closes all at
 *     once, every operation here has a finish() that handles only itself.
 *
 * Support is decided in two layers, because neither alone is sufficient:
 *   - the kernel probe (uring_engine_op_supported) answers "does this kernel
 *     implement the opcode", which a compile-time #ifdef cannot;
 *   - the FSAL override answers "do I use that opcode at all", which the
 *     probe cannot, since an FSAL driving IORING_OP_URING_CMD does not care
 *     whether the generic opcode exists.
 */

#ifndef FSAL_URING_FOPS_H
#define FSAL_URING_FOPS_H

#include "config.h"

#ifdef USE_FSAL_URING

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>
#include <linux/stat.h>

#include "fsal_types.h"
#include "io_uring/core/uring_engine.h"

struct fsal_export;
struct fsal_obj_handle;
struct state_t;
struct uring_pers;
/* Defined in FSAL_URING/uring_pers.h; forward-declared so this header does
 * not drag the registry's internals into every FSAL.
 */
struct uring_pers_cred_ops;

/**
 * @brief Abstract operations an FSAL may implement through io_uring.
 *
 * These are storage operations, not NFS opcodes: several NFS operations can
 * map onto one entry (GETATTR and ACCESS both ride FSAL_URING_FOP_STATX,
 * ALLOCATE and DEALLOCATE both ride FSAL_URING_FOP_FALLOCATE), and the
 * mapping lives in the protocol layer.
 *
 * Append-only.  FSAL_URING_FOP_LAST sizes the vectors, so inserting in the
 * middle would silently renumber every FSAL's table.
 */
enum fsal_uring_fop {
	FSAL_URING_FOP_READ = 0,
	FSAL_URING_FOP_WRITE,
	FSAL_URING_FOP_SYNC, /**< COMMIT: sync_file_range / fsync */
	FSAL_URING_FOP_STATX, /**< GETATTR, ACCESS */
	FSAL_URING_FOP_CLOSE,
	FSAL_URING_FOP_FALLOCATE, /**< ALLOCATE, DEALLOCATE */
	FSAL_URING_FOP_LAST
};

/**
 * Maximum SQEs a single FSAL operation may emit.
 *
 * 2 today: a stable WRITE is a WRITEV plus a linked sync. Raising it is a
 * matter of widening fsal_uring_fop_req::pers[] and checking that
 * nfs4_uring_op_desc::max_slots agrees.
 */
#define FSAL_URING_FOP_MAX_SQE 2

/**
 * @brief Everything one operation needs, for both prep and finish.
 *
 * Allocated by the protocol layer, one per operation in a chain, and kept
 * alive until that operation's finish() has run.  prep() stores a pointer to
 * it as the chain slot's sqe_pre_arg, so the SQE callback can reach it from
 * inside submit_lock without another lookup.
 */
struct fsal_uring_fop_req {
	/** Which operation this is; selects the descriptor. */
	enum fsal_uring_fop fop;

	/**
	 * Storage handle, already unstacked past MDCACHE.  The protocol layer
	 * resolves this before calling prep(), because walking a stackable
	 * handle needs locks that must not be taken from a reaper thread.
	 */
	struct fsal_obj_handle *obj;

	/** Open state the operation runs under, or NULL. */
	struct state_t *state;

	/**
	 * Descriptor the SQE acts on. Resolved by the FSAL's prep(), which
	 * is the only layer that knows how to get one: fsal_start_io() needs
	 * the handle's private fd struct and its share reservation.
	 *
	 * MUST be -1 until resolved, never 0. Callers that allocate this
	 * struct by zeroing it have to set it explicitly, because 0 is a
	 * perfectly valid descriptor — the daemon's stdin — and every
	 * prep() validates with "fd < 0". A zeroed field would therefore
	 * pass validation and build the SQE against stdin.
	 */
	int fd;

	/**
	 * FSAL-owned state that has to survive the async gap, typically the
	 * struct fsal_fd * returned by fsal_start_io() plus the heap copy of
	 * the temporary fd it may have opened into.
	 *
	 * Allocated by prep(), released by finish() on the normal path, or
	 * by the descriptor's release() when the chain is abandoned before
	 * submission. The protocol layer never looks inside it.
	 */
	void *fsal_priv;

	/** Open mode prep() should acquire the fd with. */
	fsal_openflags_t openflags;

	/** Bypass share reservation checks (an anonymous/special stateid). */
	bool bypass;

	/**
	 * Per-SQE credentials, indexed by the SQE's position within this
	 * operation.  pers[n] is stamped into the n'th SQE the operation
	 * emits; NULL means sqe->personality = 0, i.e. run as the daemon.
	 *
	 * This is an array, not a single value, because credentials are a
	 * property of the individual SQE rather than of the request: a
	 * stable WRITE emits a WRITEV that must run as the client and a
	 * following sync that need not, and two operations sharing one
	 * chain can legitimately carry different identities.
	 *
	 * Filled by fsal_uring_fop_resolve_creds() after prep() has
	 * declared nr_sqe.  Released by fsal_uring_fop_release_creds().
	 */
	struct uring_pers *pers[FSAL_URING_FOP_MAX_SQE];

	/**
	 * How many SQEs this operation emits.  Set by the FSAL's prep();
	 * defaults to 1 and must not exceed FSAL_URING_FOP_MAX_SQE.
	 */
	unsigned int nr_sqe;

	/** Per-operation parameters and results. */
	union {
		struct {
			struct iovec *iov;
			int iovcnt;
			uint64_t offset;
			/** Registered buffer backing iov[0], or NULL. */
			struct uring_buf *buf;
			/** Set by finish(): bytes transferred. */
			uint64_t amount;
			/** Set by finish(): read hit end of file. */
			bool eof;
		} rw;

		struct {
			uint64_t offset;
			uint64_t len;
			unsigned int flags;
		} sync;

		struct {
			/** STATX_* mask requested. */
			unsigned int mask;
			/** Caller-owned buffer the kernel fills. */
			struct statx *stx;
		} statx;

		struct {
			/** FALLOC_FL_* flags; 0 for a plain allocate. */
			int mode;
			uint64_t offset;
			uint64_t len;
		} falloc;
	} u;

	/** Raw CQE result, copied in before finish() runs. */
	int32_t res;

	/** Translated result; finish() sets this. */
	fsal_status_t status;
};

/**
 * @brief Resolve the credential for one SQE of one operation.
 *
 * Called once per SQE the operation emits, after prep() has set nr_sqe and
 * before the chain is submitted. Runs on a worker thread with a valid
 * op_ctx, so it may register a new personality (which impersonates briefly
 * and must never happen on a reaper thread).
 *
 * @param[in]  req      The operation; req->obj and req->fop identify it
 * @param[in]  sqe_idx  Which SQE of this operation, 0-based
 * @param[out] pers_out Resolved personality, or NULL for daemon credentials
 *
 * Returning an error aborts planning for this operation and falls it back
 * to the synchronous handler. Returning success with *pers_out == NULL is
 * an explicit, legitimate "run this SQE as the daemon".
 */
typedef fsal_status_t (*fsal_uring_pers_fn)(struct fsal_uring_fop_req *req,
					    unsigned int sqe_idx,
					    struct uring_pers **pers_out);

/**
 * @brief One operation's handlers for one FSAL.
 */
struct fsal_uring_fop_desc {
	/** Human readable, for logs. */
	const char *name;

	/**
	 * Per-SQE credential policy for this operation, on this FSAL.
	 *
	 * NULL means fsal_uring_pers_caller(): run as the NFS client, which
	 * is what an ordinary data or metadata operation wants.
	 *
	 * This lives on the FSAL descriptor rather than in the protocol
	 * layer because the right answer is a property of how the FSAL
	 * expresses the operation, not of which NFS opcode asked for it.
	 * A backend whose command payload already carries the caller's
	 * identity must not also stamp a personality; one operating on a
	 * control descriptor it opened itself has no client identity to
	 * apply. Both set fsal_uring_pers_none() here, and neither is
	 * expressible from the NFSv4 op table.
	 */
	fsal_uring_pers_fn pers_resolve;

	/**
	 * Generic IORING_OP_* this maps to, used to gate on the kernel probe.
	 * Set to FSAL_URING_OPCODE_NONE when the FSAL drives the operation
	 * through something the generic probe cannot describe, such as
	 * IORING_OP_URING_CMD with a private command; op_supported() then
	 * decides on its own.
	 */
	uint16_t kernel_opcode;

	/**
	 * Fill in @slot so the engine can place this operation's SQE.
	 * Runs on a worker thread with a valid op_ctx, before submission.
	 *
	 * Must set slot->sqe_pre and slot->sqe_pre_arg; normally
	 * sqe_pre_arg is @req itself.
	 *
	 * Returning an error removes the operation from the batch and makes
	 * the planner fall back to the synchronous handler for it.
	 */
	fsal_status_t (*prep)(struct fsal_uring_fop_req *req,
			      struct uring_chain_op *slot);

	/**
	 * Turn @req->res into @req->status and any per-operation output.
	 * Runs on a worker thread after the chain completes, so it may take
	 * locks and call back into the FSAL.
	 */
	fsal_status_t (*finish)(struct fsal_uring_fop_req *req);

	/**
	 * Release whatever prep() acquired, for an operation that was
	 * planned but will never complete because the chain was abandoned
	 * before submission. finish() does this on the normal path, so
	 * release() exists only to cover the path where no CQE arrives.
	 *
	 * Must be idempotent and safe on a req whose prep() failed.
	 */
	void (*release)(struct fsal_uring_fop_req *req);
};

/** Sentinel for fsal_uring_fop_desc::kernel_opcode. */
#define FSAL_URING_OPCODE_NONE ((uint16_t)0xffff)

/**
 * @brief An FSAL's complete io_uring operation vector.
 *
 * Published on struct fsal_export.  A NULL vector, a NULL entry, or a NULL
 * prep means "I cannot batch that", and the planner uses the synchronous
 * handler.
 */
struct fsal_uring_fops {
	/** Indexed by enum fsal_uring_fop. */
	const struct fsal_uring_fop_desc *desc[FSAL_URING_FOP_LAST];

	/**
	 * Optional per-FSAL support override, consulted after the kernel
	 * probe.  Lets an FSAL claim an operation the generic probe cannot
	 * see (its own IORING_OP_URING_CMD command set), or refuse one the
	 * kernel offers but the backing filesystem cannot honour.
	 *
	 * NULL means "the kernel probe decides".
	 */
	bool (*op_supported)(struct fsal_export *exp, enum fsal_uring_fop fop);

	/** Engine these operations are submitted through. */
	struct fsal_uring_engine *engine;

	/**
	 * How this FSAL assumes a client identity when a personality has to
	 * be registered. NULL uses the generic fsal_set_credentials() pair.
	 *
	 * FSAL_VFS must supply its own: vfs_set_credentials() honours the
	 * export's only_one_user mode and can refuse, and its restore is a
	 * no-op in that mode. Snapshotting through the generic pair would
	 * record an identity VFS would never have assumed synchronously.
	 */
	const struct uring_pers_cred_ops *cred_ops;
};

/* -------------------------------------------------------------------------
 * Stock credential resolvers
 * -------------------------------------------------------------------------
 * Point a descriptor's pers_resolve at one of these, or supply your own.
 */

/**
 * @brief Run the SQE as the calling NFS client. The default.
 *
 * Resolves op_ctx->creds through the export's personality registry, using
 * the FSAL's own cred_ops to take the snapshot. Fails if no personality
 * could be registered, because submitting without one would silently run
 * the operation as the Ganesha daemon — more privilege than the client has.
 */
fsal_status_t fsal_uring_pers_caller(struct fsal_uring_fop_req *req,
				     unsigned int sqe_idx,
				     struct uring_pers **pers_out);

/**
 * @brief Run the SQE as the Ganesha daemon, with no credential switch.
 *
 * For operations where the client's identity is either already accounted
 * for or meaningless: a descriptor close, which the kernel authorised at
 * open time; or a backend whose own command payload carries the caller.
 */
fsal_status_t fsal_uring_pers_none(struct fsal_uring_fop_req *req,
				   unsigned int sqe_idx,
				   struct uring_pers **pers_out);

/**
 * @brief Fill req->pers[0 .. req->nr_sqe-1] for a planned operation.
 *
 * Call after the FSAL's prep() has set nr_sqe and before submission.
 * On failure nothing is left referenced and the operation must not be
 * submitted.
 */
fsal_status_t fsal_uring_fop_resolve_creds(struct fsal_export *exp,
					   struct fsal_uring_fop_req *req);

/** Drop every reference taken by fsal_uring_fop_resolve_creds(). */
void fsal_uring_fop_release_creds(struct fsal_uring_fop_req *req);

/**
 * @brief May @fop be batched into a chain for this export?
 *
 * Applies, in order: the FSAL publishes a handler at all, the kernel
 * implements the opcode behind it, and the FSAL's own override agrees.
 *
 * @param exp  Export whose FSAL vector to consult.
 * @param fop  Operation to test.
 *
 * @return true when the operation can be planned into an SQE chain.
 */
bool fsal_uring_fop_can_batch(struct fsal_export *exp,
			      enum fsal_uring_fop fop);

/**
 * @brief Look up the handler pair for @fop, or NULL if unavailable.
 *
 * Returns NULL in exactly the cases where fsal_uring_fop_can_batch() is
 * false, so a caller that already has the descriptor need not test both.
 */
const struct fsal_uring_fop_desc *fsal_uring_fop_get(struct fsal_export *exp,
						     enum fsal_uring_fop fop);

/**
 * @brief Submit a planned chain and arrange for @done_cb on completion.
 *
 * Thin wrapper over uring_engine_submit_chain() that is exported to FSAL
 * modules, which cannot link the engine directly.
 *
 * @return 0 on success, negative errno otherwise.  done_cb only fires on
 *         success.
 */
int fsal_uring_submit_chain(struct fsal_uring_engine *engine,
			    struct uring_chain_req *chain);

/**
 * @brief Does the running kernel implement @opcode?
 *
 * Exported wrapper over uring_engine_op_supported() for FSAL modules.
 */
bool fsal_uring_opcode_supported(struct fsal_uring_engine *engine,
				 unsigned int opcode);

/**
 * @brief Resolve the calling client's io_uring personality.
 *
 * io_uring runs an SQE under the credential of whichever thread (or SQPOLL
 * kernel thread) submits it, which is not the NFS client. The personality
 * registry pre-registers client credentials so an SQE can name one; this is
 * how a batched operation gets the right identity without the submitting
 * thread impersonating anyone.
 *
 * The registry lives inside the opaque struct fsal_uring_engine, so this is
 * the only way the protocol layer can reach it. The result is cached in
 * op_ctx->uring_pers for the rest of the request.
 *
 * @return A referenced personality the caller must release with
 *         fsal_uring_pers_release(), or NULL if one could not be resolved
 *         (registry full, or registration refused) — in which case the
 *         caller must NOT batch the operation, because submitting it would
 *         run under the daemon credential.
 */
struct uring_pers *fsal_uring_pers_get(struct fsal_uring_engine *engine);

/**
 * @brief As fsal_uring_pers_get(), but taking the snapshot through the
 *        FSAL's own impersonation primitives.
 */
struct uring_pers *fsal_uring_pers_get_ops(struct fsal_uring_engine *engine,
					   const struct uring_pers_cred_ops
						   *cred_ops);

/** Drop a reference taken by fsal_uring_pers_get(). NULL-safe. */
void fsal_uring_pers_release(struct uring_pers *pers);

#endif /* USE_FSAL_URING */

#endif /* FSAL_URING_FOPS_H */
