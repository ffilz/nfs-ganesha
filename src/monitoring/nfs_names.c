/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2021
 * Author: Bjorn Leffler leffler@google.com
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
 * Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "nfs_names.h"
#include "log.h"
#include "nfs23.h"

const char *nfs3_proc_name(const uint32_t proc)
{
	switch (proc) {
	case NFSPROC3_NULL:
		return "null";
	case NFSPROC3_GETATTR:
		return "getattr";
	case NFSPROC3_SETATTR:
		return "setattr";
	case NFSPROC3_LOOKUP:
		return "lookup";
	case NFSPROC3_ACCESS:
		return "access";
	case NFSPROC3_READLINK:
		return "readlink";
	case NFSPROC3_READ:
		return "read";
	case NFSPROC3_WRITE:
		return "write";
	case NFSPROC3_CREATE:
		return "create";
	case NFSPROC3_MKDIR:
		return "mkdir";
	case NFSPROC3_SYMLINK:
		return "symlink";
	case NFSPROC3_MKNOD:
		return "mknod";
	case NFSPROC3_REMOVE:
		return "remove";
	case NFSPROC3_RMDIR:
		return "rmdir";
	case NFSPROC3_RENAME:
		return "rename";
	case NFSPROC3_LINK:
		return "link";
	case NFSPROC3_READDIR:
		return "readdir";
	case NFSPROC3_READDIRPLUS:
		return "readdirplus";
	case NFSPROC3_FSSTAT:
		return "fsstat";
	case NFSPROC3_FSINFO:
		return "fsinfo";
	case NFSPROC3_PATHCONF:
		return "pathconf";
	case NFSPROC3_COMMIT:
		return "commit";
	default:
		LogWarn(COMPONENT_MONITORING,
			"UNKNOWN NFSv3 PROC: %" PRIu32, proc);
		return "UNKNOWN NFSv3 PROC";
	}
}

const char *nfs4_proc_name(const uint32_t proc)
{
	switch (proc) {
	case NFS4_OP_ACCESS:
		return "access";
	case NFS4_OP_CLOSE:
		return "close";
	case NFS4_OP_COMMIT:
		return "commit";
	case NFS4_OP_CREATE:
		return "create";
	case NFS4_OP_DELEGPURGE:
		return "delegpurge";
	case NFS4_OP_DELEGRETURN:
		return "delegreturn";
	case NFS4_OP_GETATTR:
		return "getattr";
	case NFS4_OP_GETFH:
		return "getfh";
	case NFS4_OP_LINK:
		return "link";
	case NFS4_OP_LOCK:
		return "lock";
	case NFS4_OP_LOCKT:
		return "lockt";
	case NFS4_OP_LOCKU:
		return "locku";
	case NFS4_OP_LOOKUP:
		return "lookup";
	case NFS4_OP_LOOKUPP:
		return "lookupp";
	case NFS4_OP_NVERIFY:
		return "nverify";
	case NFS4_OP_OPEN:
		return "open";
	case NFS4_OP_OPENATTR:
		return "openattr";
	case NFS4_OP_OPEN_CONFIRM:
		return "open_confirm";
	case NFS4_OP_OPEN_DOWNGRADE:
		return "open_downgrade";
	case NFS4_OP_PUTFH:
		return "putfh";
	case NFS4_OP_PUTPUBFH:
		return "putpubfh";
	case NFS4_OP_PUTROOTFH:
		return "putrootfh";
	case NFS4_OP_READ:
		return "read";
	case NFS4_OP_READDIR:
		return "readdir";
	case NFS4_OP_READLINK:
		return "readlink";
	case NFS4_OP_REMOVE:
		return "remove";
	case NFS4_OP_RENAME:
		return "rename";
	case NFS4_OP_RENEW:
		return "renew";
	case NFS4_OP_RESTOREFH:
		return "restorefh";
	case NFS4_OP_SAVEFH:
		return "savefh";
	case NFS4_OP_SECINFO:
		return "secinfo";
	case NFS4_OP_SETATTR:
		return "setattr";
	case NFS4_OP_SETCLIENTID:
		return "setclientid";
	case NFS4_OP_SETCLIENTID_CONFIRM:
		return "setclientid_confirm";
	case NFS4_OP_VERIFY:
		return "verify";
	case NFS4_OP_WRITE:
		return "write";
	case NFS4_OP_RELEASE_LOCKOWNER:
		return "release_lockowner";

	/* NFSv4.1 */
	case NFS4_OP_BACKCHANNEL_CTL:
		return "backchannel_ctl";
	case NFS4_OP_BIND_CONN_TO_SESSION:
		return "bind_conn_to_session";
	case NFS4_OP_EXCHANGE_ID:
		return "exchange_id";
	case NFS4_OP_CREATE_SESSION:
		return "create_session";
	case NFS4_OP_DESTROY_SESSION:
		return "destroy_session";
	case NFS4_OP_FREE_STATEID:
		return "free_stateid";
	case NFS4_OP_GET_DIR_DELEGATION:
		return "get_dir_delegation";
	case NFS4_OP_GETDEVICEINFO:
		return "getdeviceinfo";
	case NFS4_OP_GETDEVICELIST:
		return "getdevicelist";
	case NFS4_OP_LAYOUTCOMMIT:
		return "layoutcommit";
	case NFS4_OP_LAYOUTGET:
		return "layoutget";
	case NFS4_OP_LAYOUTRETURN:
		return "layoutreturn";
	case NFS4_OP_SECINFO_NO_NAME:
		return "secinfo_no_name";
	case NFS4_OP_SEQUENCE:
		return "sequence";
	case NFS4_OP_SET_SSV:
		return "set_ssv";
	case NFS4_OP_TEST_STATEID:
		return "test_stateid";
	case NFS4_OP_WANT_DELEGATION:
		return "want_delegation";
	case NFS4_OP_DESTROY_CLIENTID:
		return "destroy_clientid";
	case NFS4_OP_RECLAIM_COMPLETE:
		return "reclaim_complete";

	/* NFSv4.2 */
	case NFS4_OP_ALLOCATE:
		return "allocate";
	case NFS4_OP_COPY:
		return "copy";
	case NFS4_OP_COPY_NOTIFY:
		return "copy_notify";
	case NFS4_OP_DEALLOCATE:
		return "deallocate";
	case NFS4_OP_IO_ADVISE:
		return "io_advise";
	case NFS4_OP_LAYOUTERROR:
		return "layouterror";
	case NFS4_OP_LAYOUTSTATS:
		return "layoutstats";
	case NFS4_OP_OFFLOAD_CANCEL:
		return "offload_cancel";
	case NFS4_OP_OFFLOAD_STATUS:
		return "offload_status";
	case NFS4_OP_READ_PLUS:
		return "read_plus";
	case NFS4_OP_SEEK:
		return "seek";
	case NFS4_OP_WRITE_SAME:
		return "write_same";
	case NFS4_OP_CLONE:
		return "clone";

	/* NFSv4.3 */
	case NFS4_OP_GETXATTR:
		return "getxattr";
	case NFS4_OP_SETXATTR:
		return "setxattr";
	case NFS4_OP_LISTXATTR:
		return "listxattr";
	case NFS4_OP_REMOVEXATTR:
		return "removexattr";

	case NFS4_OP_LAST_ONE:
		return "last_one";

	case NFS4_OP_ILLEGAL:
		return "illegal";

	default:
		LogWarn(COMPONENT_MONITORING,
			"UNKNOWN NFSv4 PROC: %" PRIu32, proc);
		return "UNKNOWN NFSv4 PROC";
	}
}

const char *nfsstat3_name(const nfsstat3 status)
{
	switch (status) {
	case NFS3_OK:
		return "nfs3_ok";
	case NFS3ERR_PERM:
		return "nfs3err_perm";
	case NFS3ERR_NOENT:
		return "nfs3err_noent";
	case NFS3ERR_IO:
		return "nfs3err_io";
	case NFS3ERR_NXIO:
		return "nfs3err_nxio";
	case NFS3ERR_ACCES:
		return "nfs3err_acces";
	case NFS3ERR_EXIST:
		return "nfs3err_exist";
	case NFS3ERR_XDEV:
		return "nfs3err_xdev";
	case NFS3ERR_NODEV:
		return "nfs3err_nodev";
	case NFS3ERR_NOTDIR:
		return "nfs3err_notdir";
	case NFS3ERR_ISDIR:
		return "nfs3err_isdir";
	case NFS3ERR_INVAL:
		return "nfs3err_inval";
	case NFS3ERR_FBIG:
		return "nfs3err_fbig";
	case NFS3ERR_NOSPC:
		return "nfs3err_nospc";
	case NFS3ERR_ROFS:
		return "nfs3err_rofs";
	case NFS3ERR_MLINK:
		return "nfs3err_mlink";
	case NFS3ERR_NAMETOOLONG:
		return "nfs3err_nametoolong";
	case NFS3ERR_NOTEMPTY:
		return "nfs3err_notempty";
	case NFS3ERR_DQUOT:
		return "nfs3err_dquot";
	case NFS3ERR_STALE:
		return "nfs3err_stale";
	case NFS3ERR_REMOTE:
		return "nfs3err_remote";
	case NFS3ERR_BADHANDLE:
		return "nfs3err_badhandle";
	case NFS3ERR_NOT_SYNC:
		return "nfs3err_not_sync";
	case NFS3ERR_BAD_COOKIE:
		return "nfs3err_bad_cookie";
	case NFS3ERR_NOTSUPP:
		return "nfs3err_notsupp";
	case NFS3ERR_TOOSMALL:
		return "nfs3err_toosmall";
	case NFS3ERR_SERVERFAULT:
		return "nfs3err_serverfault";
	case NFS3ERR_BADTYPE:
		return "nfs3err_badtype";
	case NFS3ERR_JUKEBOX:
		return "nfs3err_jukebox";
	default:
		LogWarn(COMPONENT_MONITORING,
			"UNKNOWN NFSv3 ERROR CODE: %" PRIu32, status);
		return "UNKNOWN NFSv3 ERROR CODE";
	}
}

const char *nfsstat4_name(const nfsstat4 status)
{
	switch (status) {
	case NFS4_OK:
		return "nfs4_ok";
	case NFS4ERR_PERM:
		return "nfs4err_perm";
	case NFS4ERR_NOENT:
		return "nfs4err_noent";
	case NFS4ERR_IO:
		return "nfs4err_io";
	case NFS4ERR_NXIO:
		return "nfs4err_nxio";
	case NFS4ERR_ACCESS:
		return "nfs4err_access";
	case NFS4ERR_EXIST:
		return "nfs4err_exist";
	case NFS4ERR_XDEV:
		return "nfs4err_xdev";
	case NFS4ERR_NOTDIR:
		return "nfs4err_notdir";
	case NFS4ERR_ISDIR:
		return "nfs4err_isdir";
	case NFS4ERR_INVAL:
		return "nfs4err_inval";
	case NFS4ERR_FBIG:
		return "nfs4err_fbig";
	case NFS4ERR_NOSPC:
		return "nfs4err_nospc";
	case NFS4ERR_ROFS:
		return "nfs4err_rofs";
	case NFS4ERR_MLINK:
		return "nfs4err_mlink";
	case NFS4ERR_NAMETOOLONG:
		return "nfs4err_nametoolong";
	case NFS4ERR_NOTEMPTY:
		return "nfs4err_notempty";
	case NFS4ERR_DQUOT:
		return "nfs4err_dquot";
	case NFS4ERR_STALE:
		return "nfs4err_stale";
	case NFS4ERR_BADHANDLE:
		return "nfs4err_badhandle";
	case NFS4ERR_BAD_COOKIE:
		return "nfs4err_bad_cookie";
	case NFS4ERR_NOTSUPP:
		return "nfs4err_notsupp";
	case NFS4ERR_TOOSMALL:
		return "nfs4err_toosmall";
	case NFS4ERR_SERVERFAULT:
		return "nfs4err_serverfault";
	case NFS4ERR_BADTYPE:
		return "nfs4err_badtype";
	case NFS4ERR_DELAY:
		return "nfs4err_delay";
	case NFS4ERR_SAME:
		return "nfs4err_same";
	case NFS4ERR_DENIED:
		return "nfs4err_denied";
	case NFS4ERR_EXPIRED:
		return "nfs4err_expired";
	case NFS4ERR_LOCKED:
		return "nfs4err_locked";
	case NFS4ERR_GRACE:
		return "nfs4err_grace";
	case NFS4ERR_FHEXPIRED:
		return "nfs4err_fhexpired";
	case NFS4ERR_SHARE_DENIED:
		return "nfs4err_share_denied";
	case NFS4ERR_WRONGSEC:
		return "nfs4err_wrongsec";
	case NFS4ERR_CLID_INUSE:
		return "nfs4err_clid_inuse";
	case NFS4ERR_RESOURCE:
		return "nfs4err_resource";
	case NFS4ERR_MOVED:
		return "nfs4err_moved";
	case NFS4ERR_NOFILEHANDLE:
		return "nfs4err_nofilehandle";
	case NFS4ERR_MINOR_VERS_MISMATCH:
		return "nfs4err_minor_vers_mismatch";
	case NFS4ERR_STALE_CLIENTID:
		return "nfs4err_stale_clientid";
	case NFS4ERR_STALE_STATEID:
		return "nfs4err_stale_stateid";
	case NFS4ERR_OLD_STATEID:
		return "nfs4err_old_stateid";
	case NFS4ERR_BAD_STATEID:
		return "nfs4err_bad_stateid";
	case NFS4ERR_BAD_SEQID:
		return "nfs4err_bad_seqid";
	case NFS4ERR_NOT_SAME:
		return "nfs4err_not_same";
	case NFS4ERR_LOCK_RANGE:
		return "nfs4err_lock_range";
	case NFS4ERR_SYMLINK:
		return "nfs4err_symlink";
	case NFS4ERR_RESTOREFH:
		return "nfs4err_restorefh";
	case NFS4ERR_LEASE_MOVED:
		return "nfs4err_lease_moved";
	case NFS4ERR_ATTRNOTSUPP:
		return "nfs4err_attrnotsupp";
	case NFS4ERR_NO_GRACE:
		return "nfs4err_no_grace";
	case NFS4ERR_RECLAIM_BAD:
		return "nfs4err_reclaim_bad";
	case NFS4ERR_RECLAIM_CONFLICT:
		return "nfs4err_reclaim_conflict";
	case NFS4ERR_BADXDR:
		return "nfs4err_badxdr";
	case NFS4ERR_LOCKS_HELD:
		return "nfs4err_locks_held";
	case NFS4ERR_OPENMODE:
		return "nfs4err_openmode";
	case NFS4ERR_BADOWNER:
		return "nfs4err_badowner";
	case NFS4ERR_BADCHAR:
		return "nfs4err_badchar";
	case NFS4ERR_BADNAME:
		return "nfs4err_badname";
	case NFS4ERR_BAD_RANGE:
		return "nfs4err_bad_range";
	case NFS4ERR_LOCK_NOTSUPP:
		return "nfs4err_lock_notsupp";
	case NFS4ERR_OP_ILLEGAL:
		return "nfs4err_op_illegal";
	case NFS4ERR_DEADLOCK:
		return "nfs4err_deadlock";
	case NFS4ERR_FILE_OPEN:
		return "nfs4err_file_open";
	case NFS4ERR_ADMIN_REVOKED:
		return "nfs4err_admin_revoked";
	case NFS4ERR_CB_PATH_DOWN:
		return "nfs4err_cb_path_down";
	case NFS4ERR_BADIOMODE:
		return "nfs4err_badiomode";
	case NFS4ERR_BADLAYOUT:
		return "nfs4err_badlayout";
	case NFS4ERR_BAD_SESSION_DIGEST:
		return "nfs4err_bad_session_digest";
	case NFS4ERR_BADSESSION:
		return "nfs4err_badsession";
	case NFS4ERR_BADSLOT:
		return "nfs4err_badslot";
	case NFS4ERR_COMPLETE_ALREADY:
		return "nfs4err_complete_already";
	case NFS4ERR_CONN_NOT_BOUND_TO_SESSION:
		return "nfs4err_conn_not_bound_to_session";
	case NFS4ERR_DELEG_ALREADY_WANTED:
		return "nfs4err_deleg_already_wanted";
	case NFS4ERR_BACK_CHAN_BUSY:
		return "nfs4err_back_chan_busy";
	case NFS4ERR_LAYOUTTRYLATER:
		return "nfs4err_layouttrylater";
	case NFS4ERR_LAYOUTUNAVAILABLE:
		return "nfs4err_layoutunavailable";
	case NFS4ERR_NOMATCHING_LAYOUT:
		return "nfs4err_nomatching_layout";
	case NFS4ERR_RECALLCONFLICT:
		return "nfs4err_recallconflict";
	case NFS4ERR_UNKNOWN_LAYOUTTYPE:
		return "nfs4err_unknown_layouttype";
	case NFS4ERR_SEQ_MISORDERED:
		return "nfs4err_seq_misordered";
	case NFS4ERR_SEQUENCE_POS:
		return "nfs4err_sequence_pos";
	case NFS4ERR_REQ_TOO_BIG:
		return "nfs4err_req_too_big";
	case NFS4ERR_REP_TOO_BIG:
		return "nfs4err_rep_too_big";
	case NFS4ERR_REP_TOO_BIG_TO_CACHE:
		return "nfs4err_rep_too_big_to_cache";
	case NFS4ERR_RETRY_UNCACHED_REP:
		return "nfs4err_retry_uncached_rep";
	case NFS4ERR_UNSAFE_COMPOUND:
		return "nfs4err_unsafe_compound";
	case NFS4ERR_TOO_MANY_OPS:
		return "nfs4err_too_many_ops";
	case NFS4ERR_OP_NOT_IN_SESSION:
		return "nfs4err_op_not_in_session";
	case NFS4ERR_HASH_ALG_UNSUPP:
		return "nfs4err_hash_alg_unsupp";
	case NFS4ERR_CLIENTID_BUSY:
		return "nfs4err_clientid_busy";
	case NFS4ERR_PNFS_IO_HOLE:
		return "nfs4err_pnfs_io_hole";
	case NFS4ERR_SEQ_FALSE_RETRY:
		return "nfs4err_seq_false_retry";
	case NFS4ERR_BAD_HIGH_SLOT:
		return "nfs4err_bad_high_slot";
	case NFS4ERR_DEADSESSION:
		return "nfs4err_deadsession";
	case NFS4ERR_ENCR_ALG_UNSUPP:
		return "nfs4err_encr_alg_unsupp";
	case NFS4ERR_PNFS_NO_LAYOUT:
		return "nfs4err_pnfs_no_layout";
	case NFS4ERR_NOT_ONLY_OP:
		return "nfs4err_not_only_op";
	case NFS4ERR_WRONG_CRED:
		return "nfs4err_wrong_cred";
	case NFS4ERR_WRONG_TYPE:
		return "nfs4err_wrong_type";
	case NFS4ERR_DIRDELEG_UNAVAIL:
		return "nfs4err_dirdeleg_unavail";
	case NFS4ERR_REJECT_DELEG:
		return "nfs4err_reject_deleg";
	case NFS4ERR_RETURNCONFLICT:
		return "nfs4err_returnconflict";
	case NFS4ERR_DELEG_REVOKED:
		return "nfs4err_deleg_revoked";
	case NFS4ERR_NOXATTR:
		return "nfs4err_noxattr";
	case NFS4ERR_XATTR2BIG:
		return "nfs4err_xattr2big";

	/* NFSv4.2 */
	case NFS4ERR_PARTNER_NOTSUPP:
		return "nfs4err_partner_notsupp";
	case NFS4ERR_PARTNER_NO_AUTH:
		return "nfs4err_partner_no_auth";
	case NFS4ERR_OFFLOAD_DENIED:
		return "nfs4err_offload_denied";
	case NFS4ERR_WRONG_LFS:
		return "nfs4err_wrong_lfs";
	case NFS4ERR_BADLABEL:
		return "nfs4err_badlabel";
	case NFS4ERR_OFFLOAD_NO_REQS:
		return "nfs4err_offload_no_reqs";
	case NFS4ERR_UNION_NOTSUPP:
		return "nfs4err_union_notsupp";
	case NFS4ERR_REPLAY:
		return "nfs4err_replay";
	default:
		LogWarn(COMPONENT_MONITORING,
			"UNKNOWN NFSv4 ERROR CODE: %" PRIu32, status);
		return "UNKNOWN NFSv4 ERROR CODE";
	}
}
