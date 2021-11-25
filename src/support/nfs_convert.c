/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 *
 * ---------------------------------------
 */

/**
 * @file  nfs_convert.c
 * @brief NFS conversion tools.
 */

#include <string.h>
#include "nfs_convert.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"

struct op_name {
	char *name;
};

#ifdef _USE_NFS3
static const struct op_name op_names_v3[] = {
	[NFSPROC3_NULL] = {.name = "NULL", },
	[NFSPROC3_GETATTR] = {.name = "GETATTR", },
	[NFSPROC3_SETATTR] = {.name = "SETATTR", },
	[NFSPROC3_LOOKUP] = {.name = "LOOKUP", },
	[NFSPROC3_ACCESS] = {.name = "ACCESS", },
	[NFSPROC3_READLINK] = {.name = "READLINK", },
	[NFSPROC3_READ] = {.name = "READ", },
	[NFSPROC3_WRITE] = {.name = "WRITE", },
	[NFSPROC3_CREATE] = {.name = "CREATE", },
	[NFSPROC3_MKDIR] = {.name = "MKDIR", },
	[NFSPROC3_SYMLINK] = {.name = "SYMLINK", },
	[NFSPROC3_MKNOD] = {.name = "MKNOD", },
	[NFSPROC3_REMOVE] = {.name = "REMOVE", },
	[NFSPROC3_RMDIR] = {.name = "RMDIR", },
	[NFSPROC3_RENAME] = {.name = "RENAME", },
	[NFSPROC3_LINK] = {.name = "LINK", },
	[NFSPROC3_READDIR] = {.name = "READDIR", },
	[NFSPROC3_READDIRPLUS] = {.name = "READDIRPLUS", },
	[NFSPROC3_FSSTAT] = {.name = "FSSTAT", },
	[NFSPROC3_FSINFO] = {.name = "FSINFO", },
	[NFSPROC3_PATHCONF] = {.name = "PATHCONF", },
	[NFSPROC3_COMMIT] = {.name = "COMMIT", },
};

static const int MAX_NFS3_PROC = NFSPROC3_COMMIT;

const char *nfsproc3_to_str(int nfsproc3)
{
	if (nfsproc3 < 0 || nfsproc3 > MAX_NFS3_PROC) {
		return op_names_v3[0].name;
	}
	return op_names_v3[nfsproc3].name;

}

#endif


static const struct op_name op_names_v4[] = {
	[0] = {.name = "ILLEGAL", },
	[1] = {.name = "ILLEGAL",},
	[2] = {.name = "ILLEGAL",},
	[NFS4_OP_ACCESS] = {.name = "ACCESS",},
	[NFS4_OP_CLOSE] = {.name = "CLOSE",},
	[NFS4_OP_COMMIT] = {.name = "COMMIT",},
	[NFS4_OP_CREATE] = {.name = "CREATE",},
	[NFS4_OP_DELEGPURGE] = {.name = "DELEGPURGE",},
	[NFS4_OP_DELEGRETURN] = {.name = "DELEGRETURN",},
	[NFS4_OP_GETATTR] = {.name = "GETATTR",},
	[NFS4_OP_GETFH] = {.name = "GETFH",},
	[NFS4_OP_LINK] = {.name = "LINK",},
	[NFS4_OP_LOCK] = {.name = "LOCK",},
	[NFS4_OP_LOCKT] = {.name = "LOCKT",},
	[NFS4_OP_LOCKU] = {.name = "LOCKU",},
	[NFS4_OP_LOOKUP] = {.name = "LOOKUP",},
	[NFS4_OP_LOOKUPP] = {.name = "LOOKUPP",},
	[NFS4_OP_NVERIFY] = {.name = "NVERIFY",},
	[NFS4_OP_OPEN] = {.name = "OPEN",},
	[NFS4_OP_OPENATTR] = {.name = "OPENATTR",},
	[NFS4_OP_OPEN_CONFIRM] = {.name = "OPEN_CONFIRM",},
	[NFS4_OP_OPEN_DOWNGRADE] = {.name = "OPEN_DOWNGRADE",},
	[NFS4_OP_PUTFH] = {.name = "PUTFH",},
	[NFS4_OP_PUTPUBFH] = {.name = "PUTPUBFH",},
	[NFS4_OP_PUTROOTFH] = {.name = "PUTROOTFH",},
	[NFS4_OP_READ] = {.name = "READ",},
	[NFS4_OP_READDIR] = {.name = "READDIR",},
	[NFS4_OP_READLINK] = {.name = "READLINK",},
	[NFS4_OP_REMOVE] = {.name = "REMOVE",},
	[NFS4_OP_RENAME] = {.name = "RENAME",},
	[NFS4_OP_RENEW] = {.name = "RENEW",},
	[NFS4_OP_RESTOREFH] = {.name = "RESTOREFH",},
	[NFS4_OP_SAVEFH] = {.name = "SAVEFH",},
	[NFS4_OP_SECINFO] = {.name = "SECINFO",},
	[NFS4_OP_SETATTR] = {.name = "SETATTR",},
	[NFS4_OP_SETCLIENTID] = {.name = "SETCLIENTID",},
	[NFS4_OP_SETCLIENTID_CONFIRM] = {.name = "SETCLIENTID_CONFIRM",},
	[NFS4_OP_VERIFY] = {.name = "VERIFY",},
	[NFS4_OP_WRITE] = {.name = "WRITE",},
	[NFS4_OP_RELEASE_LOCKOWNER] = {.name = "RELEASE_LOCKOWNER",},
	[NFS4_OP_BACKCHANNEL_CTL] = {.name = "BACKCHANNEL_CTL",},
	[NFS4_OP_BIND_CONN_TO_SESSION] = {.name = "BIND_CONN_TO_SESSION",},
	[NFS4_OP_EXCHANGE_ID] = {.name = "EXCHANGE_ID",},
	[NFS4_OP_CREATE_SESSION] = {.name = "CREATE_SESSION",},
	[NFS4_OP_DESTROY_SESSION] = {.name = "DESTROY_SESSION",},
	[NFS4_OP_FREE_STATEID] = {.name = "FREE_STATEID",},
	[NFS4_OP_GET_DIR_DELEGATION] = {.name = "GET_DIR_DELEGATION",},
	[NFS4_OP_GETDEVICEINFO] = {.name = "GETDEVICEINFO",},
	[NFS4_OP_GETDEVICELIST] = {.name = "GETDEVICELIST",},
	[NFS4_OP_LAYOUTCOMMIT] = {.name = "LAYOUTCOMMIT",},
	[NFS4_OP_LAYOUTGET] = {.name = "LAYOUTGET",},
	[NFS4_OP_LAYOUTRETURN] = {.name = "LAYOUTRETURN",},
	[NFS4_OP_SECINFO_NO_NAME] = {.name = "SECINFO_NO_NAME",},
	[NFS4_OP_SEQUENCE] = {.name = "SEQUENCE",},
	[NFS4_OP_SET_SSV] = {.name = "SET_SSV",},
	[NFS4_OP_TEST_STATEID] = {.name = "TEST_STATEID",},
	[NFS4_OP_WANT_DELEGATION] = {.name = "WANT_DELEGATION",},
	[NFS4_OP_DESTROY_CLIENTID] = {.name = "DESTROY_CLIENTID",},
	[NFS4_OP_RECLAIM_COMPLETE] = {.name = "RECLAIM_COMPLETE",},
	/* NFSv4.2 */
	[NFS4_OP_ALLOCATE] = {.name = "ALLOCATE",},
	[NFS4_OP_COPY] = {.name = "COPY",},
	[NFS4_OP_COPY_NOTIFY] = {.name = "COPY_NOTIFY",},
	[NFS4_OP_DEALLOCATE] = {.name = "DEALLOCATE",},
	[NFS4_OP_IO_ADVISE] = {.name = "IO_ADVISE",},
	[NFS4_OP_LAYOUTERROR] = {.name = "LAYOUTERROR",},
	[NFS4_OP_OFFLOAD_CANCEL] = {.name = "OFFLOAD_CANCEL",},
	[NFS4_OP_OFFLOAD_STATUS] = {.name = "OFFLOAD_STATUS",},
	[NFS4_OP_READ_PLUS] = {.name = "READ_PLUS",},
	[NFS4_OP_SEEK] = {.name = "SEEK",},
	[NFS4_OP_WRITE_SAME] = {.name = "WRITE_SAME",},
	[NFS4_OP_CLONE] = {.name = "OP_CLONE",},
	/* NFSv4.3 */
	[NFS4_OP_GETXATTR] = {.name = "OP_GETXATTR",},
	[NFS4_OP_SETXATTR] = {.name = "OP_SETXATTR",},
	[NFS4_OP_LISTXATTR] = {.name = "OP_LISTXATTR",},
	[NFS4_OP_REMOVEXATTR] = {.name = "OP_REMOVEXATTR",},
};

static const int MAX_NFS4_OP = NFS4_OP_REMOVEXATTR;

const char *nfsop4_to_str(int nfsop4)
{
	if (nfsop4 < 0 || MAX_NFS4_OP) {
		return op_names_v4[0].name;
	}
	return op_names_v4[nfsop4].name;
}

char *nfsstat3_to_str(nfsstat3 code)
{
	switch (code) {
		/* no nead for break statments,
		 * because we "return".
		 */
	case NFS3_OK:
		return "NFS3_OK";
	case NFS3ERR_PERM:
		return "NFS3ERR_PERM";
	case NFS3ERR_NOENT:
		return "NFS3ERR_NOENT";
	case NFS3ERR_IO:
		return "NFS3ERR_IO";
	case NFS3ERR_NXIO:
		return "NFS3ERR_NXIO";
	case NFS3ERR_ACCES:
		return "NFS3ERR_ACCES";
	case NFS3ERR_EXIST:
		return "NFS3ERR_EXIST";
	case NFS3ERR_XDEV:
		return "NFS3ERR_XDEV";
	case NFS3ERR_NODEV:
		return "NFS3ERR_NODEV";
	case NFS3ERR_NOTDIR:
		return "NFS3ERR_NOTDIR";
	case NFS3ERR_ISDIR:
		return "NFS3ERR_ISDIR";
	case NFS3ERR_INVAL:
		return "NFS3ERR_INVAL";
	case NFS3ERR_FBIG:
		return "NFS3ERR_FBIG";
	case NFS3ERR_NOSPC:
		return "NFS3ERR_NOSPC";
	case NFS3ERR_ROFS:
		return "NFS3ERR_ROFS";
	case NFS3ERR_MLINK:
		return "NFS3ERR_MLINK";
	case NFS3ERR_NAMETOOLONG:
		return "NFS3ERR_NAMETOOLONG";
	case NFS3ERR_NOTEMPTY:
		return "NFS3ERR_NOTEMPTY";
	case NFS3ERR_DQUOT:
		return "NFS3ERR_DQUOT";
	case NFS3ERR_STALE:
		return "NFS3ERR_STALE";
	case NFS3ERR_REMOTE:
		return "NFS3ERR_REMOTE";
	case NFS3ERR_BADHANDLE:
		return "NFS3ERR_BADHANDLE";
	case NFS3ERR_NOT_SYNC:
		return "NFS3ERR_NOT_SYNC";
	case NFS3ERR_BAD_COOKIE:
		return "NFS3ERR_BAD_COOKIE";
	case NFS3ERR_NOTSUPP:
		return "NFS3ERR_NOTSUPP";
	case NFS3ERR_TOOSMALL:
		return "NFS3ERR_TOOSMALL";
	case NFS3ERR_SERVERFAULT:
		return "NFS3ERR_SERVERFAULT";
	case NFS3ERR_BADTYPE:
		return "NFS3ERR_BADTYPE";
	case NFS3ERR_JUKEBOX:
		return "NFS3ERR_JUKEBOX";
	}
	return "UNKNOWN NFSv3 ERROR CODE";
}

char *nfsstat4_to_str(nfsstat4 code)
{
	switch (code) {
	case NFS4_OK:
		return "NFS4_OK";
	case NFS4ERR_PERM:
		return "NFS4ERR_PERM";
	case NFS4ERR_NOENT:
		return "NFS4ERR_NOENT";
	case NFS4ERR_IO:
		return "NFS4ERR_IO";
	case NFS4ERR_NXIO:
		return "NFS4ERR_NXIO";
	case NFS4ERR_ACCESS:
		return "NFS4ERR_ACCESS";
	case NFS4ERR_EXIST:
		return "NFS4ERR_EXIST";
	case NFS4ERR_XDEV:
		return "NFS4ERR_XDEV";
	case NFS4ERR_NOTDIR:
		return "NFS4ERR_NOTDIR";
	case NFS4ERR_ISDIR:
		return "NFS4ERR_ISDIR";
	case NFS4ERR_INVAL:
		return "NFS4ERR_INVAL";
	case NFS4ERR_FBIG:
		return "NFS4ERR_FBIG";
	case NFS4ERR_NOSPC:
		return "NFS4ERR_NOSPC";
	case NFS4ERR_ROFS:
		return "NFS4ERR_ROFS";
	case NFS4ERR_MLINK:
		return "NFS4ERR_MLINK";
	case NFS4ERR_NAMETOOLONG:
		return "NFS4ERR_NAMETOOLONG";
	case NFS4ERR_NOTEMPTY:
		return "NFS4ERR_NOTEMPTY";
	case NFS4ERR_DQUOT:
		return "NFS4ERR_DQUOT";
	case NFS4ERR_STALE:
		return "NFS4ERR_STALE";
	case NFS4ERR_BADHANDLE:
		return "NFS4ERR_BADHANDLE";
	case NFS4ERR_BAD_COOKIE:
		return "NFS4ERR_BAD_COOKIE";
	case NFS4ERR_NOTSUPP:
		return "NFS4ERR_NOTSUPP";
	case NFS4ERR_TOOSMALL:
		return "NFS4ERR_TOOSMALL";
	case NFS4ERR_SERVERFAULT:
		return "NFS4ERR_SERVERFAULT";
	case NFS4ERR_BADTYPE:
		return "NFS4ERR_BADTYPE";
	case NFS4ERR_DELAY:
		return "NFS4ERR_DELAY";
	case NFS4ERR_SAME:
		return "NFS4ERR_SAME";
	case NFS4ERR_DENIED:
		return "NFS4ERR_DENIED";
	case NFS4ERR_EXPIRED:
		return "NFS4ERR_EXPIRED";
	case NFS4ERR_LOCKED:
		return "NFS4ERR_LOCKED";
	case NFS4ERR_GRACE:
		return "NFS4ERR_GRACE";
	case NFS4ERR_FHEXPIRED:
		return "NFS4ERR_FHEXPIRED";
	case NFS4ERR_SHARE_DENIED:
		return "NFS4ERR_SHARE_DENIED";
	case NFS4ERR_WRONGSEC:
		return "NFS4ERR_WRONGSEC";
	case NFS4ERR_CLID_INUSE:
		return "NFS4ERR_CLID_INUSE";
	case NFS4ERR_RESOURCE:
		return "NFS4ERR_RESOURCE";
	case NFS4ERR_MOVED:
		return "NFS4ERR_MOVED";
	case NFS4ERR_NOFILEHANDLE:
		return "NFS4ERR_NOFILEHANDLE";
	case NFS4ERR_MINOR_VERS_MISMATCH:
		return "NFS4ERR_MINOR_VERS_MISMATCH";
	case NFS4ERR_STALE_CLIENTID:
		return "NFS4ERR_STALE_CLIENTID";
	case NFS4ERR_STALE_STATEID:
		return "NFS4ERR_STALE_STATEID";
	case NFS4ERR_OLD_STATEID:
		return "NFS4ERR_OLD_STATEID";
	case NFS4ERR_BAD_STATEID:
		return "NFS4ERR_BAD_STATEID";
	case NFS4ERR_BAD_SEQID:
		return "NFS4ERR_BAD_SEQID";
	case NFS4ERR_NOT_SAME:
		return "NFS4ERR_NOT_SAME";
	case NFS4ERR_LOCK_RANGE:
		return "NFS4ERR_LOCK_RANGE";
	case NFS4ERR_SYMLINK:
		return "NFS4ERR_SYMLINK";
	case NFS4ERR_RESTOREFH:
		return "NFS4ERR_RESTOREFH";
	case NFS4ERR_LEASE_MOVED:
		return "NFS4ERR_LEASE_MOVED";
	case NFS4ERR_ATTRNOTSUPP:
		return "NFS4ERR_ATTRNOTSUPP";
	case NFS4ERR_NO_GRACE:
		return "NFS4ERR_NO_GRACE";
	case NFS4ERR_RECLAIM_BAD:
		return "NFS4ERR_RECLAIM_BAD";
	case NFS4ERR_RECLAIM_CONFLICT:
		return "NFS4ERR_RECLAIM_CONFLICT";
	case NFS4ERR_BADXDR:
		return "NFS4ERR_BADXDR";
	case NFS4ERR_LOCKS_HELD:
		return "NFS4ERR_LOCKS_HELD";
	case NFS4ERR_OPENMODE:
		return "NFS4ERR_OPENMODE";
	case NFS4ERR_BADOWNER:
		return "NFS4ERR_BADOWNER";
	case NFS4ERR_BADCHAR:
		return "NFS4ERR_BADCHAR";
	case NFS4ERR_BADNAME:
		return "NFS4ERR_BADNAME";
	case NFS4ERR_BAD_RANGE:
		return "NFS4ERR_BAD_RANGE";
	case NFS4ERR_LOCK_NOTSUPP:
		return "NFS4ERR_LOCK_NOTSUPP";
	case NFS4ERR_OP_ILLEGAL:
		return "NFS4ERR_OP_ILLEGAL";
	case NFS4ERR_DEADLOCK:
		return "NFS4ERR_DEADLOCK";
	case NFS4ERR_FILE_OPEN:
		return "NFS4ERR_FILE_OPEN";
	case NFS4ERR_ADMIN_REVOKED:
		return "NFS4ERR_ADMIN_REVOKED";
	case NFS4ERR_CB_PATH_DOWN:
		return "NFS4ERR_CB_PATH_DOWN";
	case NFS4ERR_BADIOMODE:
		return "NFS4ERR_BADIOMODE";
	case NFS4ERR_BADLAYOUT:
		return "NFS4ERR_BADLAYOUT";
	case NFS4ERR_BAD_SESSION_DIGEST:
		return "NFS4ERR_BAD_SESSION_DIGEST";
	case NFS4ERR_BADSESSION:
		return "NFS4ERR_BADSESSION";
	case NFS4ERR_BADSLOT:
		return "NFS4ERR_BADSLOT";
	case NFS4ERR_COMPLETE_ALREADY:
		return "NFS4ERR_COMPLETE_ALREADY";
	case NFS4ERR_CONN_NOT_BOUND_TO_SESSION:
		return "NFS4ERR_CONN_NOT_BOUND_TO_SESSION";
	case NFS4ERR_DELEG_ALREADY_WANTED:
		return "NFS4ERR_DELEG_ALREADY_WANTED";
	case NFS4ERR_BACK_CHAN_BUSY:
		return "NFS4ERR_BACK_CHAN_BUSY";
	case NFS4ERR_LAYOUTTRYLATER:
		return "NFS4ERR_LAYOUTTRYLATER";
	case NFS4ERR_LAYOUTUNAVAILABLE:
		return "NFS4ERR_LAYOUTUNAVAILABLE";
	case NFS4ERR_NOMATCHING_LAYOUT:
		return "NFS4ERR_NOMATCHING_LAYOUT";
	case NFS4ERR_RECALLCONFLICT:
		return "NFS4ERR_RECALLCONFLICT";
	case NFS4ERR_UNKNOWN_LAYOUTTYPE:
		return "NFS4ERR_UNKNOWN_LAYOUTTYPE";
	case NFS4ERR_SEQ_MISORDERED:
		return "NFS4ERR_SEQ_MISORDERED";
	case NFS4ERR_SEQUENCE_POS:
		return "NFS4ERR_SEQUENCE_POS";
	case NFS4ERR_REQ_TOO_BIG:
		return "NFS4ERR_REQ_TOO_BIG";
	case NFS4ERR_REP_TOO_BIG:
		return "NFS4ERR_REP_TOO_BIG";
	case NFS4ERR_REP_TOO_BIG_TO_CACHE:
		return "NFS4ERR_REP_TOO_BIG_TO_CACHE";
	case NFS4ERR_RETRY_UNCACHED_REP:
		return "NFS4ERR_RETRY_UNCACHED_REP";
	case NFS4ERR_UNSAFE_COMPOUND:
		return "NFS4ERR_UNSAFE_COMPOUND";
	case NFS4ERR_TOO_MANY_OPS:
		return "NFS4ERR_TOO_MANY_OPS";
	case NFS4ERR_OP_NOT_IN_SESSION:
		return "NFS4ERR_OP_NOT_IN_SESSION";
	case NFS4ERR_HASH_ALG_UNSUPP:
		return "NFS4ERR_HASH_ALG_UNSUPP";
	case NFS4ERR_CLIENTID_BUSY:
		return "NFS4ERR_CLIENTID_BUSY";
	case NFS4ERR_PNFS_IO_HOLE:
		return "NFS4ERR_PNFS_IO_HOLE";
	case NFS4ERR_SEQ_FALSE_RETRY:
		return "NFS4ERR_SEQ_FALSE_RETRY";
	case NFS4ERR_BAD_HIGH_SLOT:
		return "NFS4ERR_BAD_HIGH_SLOT";
	case NFS4ERR_DEADSESSION:
		return "NFS4ERR_DEADSESSION";
	case NFS4ERR_ENCR_ALG_UNSUPP:
		return "NFS4ERR_ENCR_ALG_UNSUPP";
	case NFS4ERR_PNFS_NO_LAYOUT:
		return "NFS4ERR_PNFS_NO_LAYOUT";
	case NFS4ERR_NOT_ONLY_OP:
		return "NFS4ERR_NOT_ONLY_OP";
	case NFS4ERR_WRONG_CRED:
		return "NFS4ERR_WRONG_CRED";
	case NFS4ERR_WRONG_TYPE:
		return "NFS4ERR_WRONG_TYPE";
	case NFS4ERR_DIRDELEG_UNAVAIL:
		return "NFS4ERR_DIRDELEG_UNAVAIL";
	case NFS4ERR_REJECT_DELEG:
		return "NFS4ERR_REJECT_DELEG";
	case NFS4ERR_RETURNCONFLICT:
		return "NFS4ERR_RETURNCONFLICT";
	case NFS4ERR_DELEG_REVOKED:
		return "NFS4ERR_DELEG_REVOKED";

	/* NFSv4.2 */
	case NFS4ERR_PARTNER_NOTSUPP:
		return "NFS4ERR_PARTNER_NOTSUPP";
	case NFS4ERR_PARTNER_NO_AUTH:
		return "NFS4ERR_PARTNER_NO_AUTH";
	case NFS4ERR_OFFLOAD_DENIED:
		return "NFS4ERR_OFFLOAD_DENIED";
	case NFS4ERR_WRONG_LFS:
		return "NFS4ERR_WRONG_LFS";
	case NFS4ERR_BADLABEL:
		return "NFS4ERR_BADLABEL";
	case NFS4ERR_OFFLOAD_NO_REQS:
		return "NFS4ERR_OFFLOAD_NO_REQS";
	case NFS4ERR_UNION_NOTSUPP:
		return "NFS4ERR_UNION_NOTSUPP";
	case NFS4ERR_REPLAY:
		return "NFS4ERR_REPLAY";

	/* NFSv4.3 */
	case NFS4ERR_NOXATTR:
		return "NFS4ERR_NOXATTR";
	case NFS4ERR_XATTR2BIG:
		return "NFS4ERR_XATTR2BIG";
	}
	return "UNKNOWN NFSv4 ERROR CODE";
}

char *nfstype3_to_str(ftype3 code)
{
	switch (code) {
		/* no nead for break statments,
		 * because we "return".
		 */
	case NF3REG:
		return "NF3REG";
	case NF3DIR:
		return "NF3DIR";
	case NF3BLK:
		return "NF3BLK";
	case NF3CHR:
		return "NF3CHR";
	case NF3LNK:
		return "NF3LNK";
	case NF3SOCK:
		return "NF3SOCK";
	case NF3FIFO:
		return "NF3FIFO";
	}
	return "UNKNOWN NFSv3 TYPE";
}

/**
 * @brief Same as htonl, but on 64 bits.
 *
 * @param[in] arg64 Value in host byte order
 *
 * @return Value in network byte order
 */
uint64_t nfs_htonl64(uint64_t arg64)
{
	uint64_t res64;

#ifdef LITTLEEND
	uint32_t low = (uint32_t) (arg64 & 0x00000000FFFFFFFFLL);
	uint32_t high = (uint32_t) ((arg64 & 0xFFFFFFFF00000000LL) >> 32);

	low = htonl(low);
	high = htonl(high);

	res64 = (uint64_t) high + (((uint64_t) low) << 32);
#else
	res64 = arg64;
#endif

	return res64;
}

/**
 * @brief Same as ntohl, but on 64 bits.
 *
 * @param[in] arg64 Value in network byte order
 *
 * @return value in host byte order.
 */
uint64_t nfs_ntohl64(uint64_t arg64)
{
	uint64_t res64;

#ifdef LITTLEEND
	uint32_t low = (uint32_t) (arg64 & 0x00000000FFFFFFFFLL);
	uint32_t high = (uint32_t) ((arg64 & 0xFFFFFFFF00000000LL) >> 32);

	low = ntohl(low);
	high = ntohl(high);

	res64 = (uint64_t) high + (((uint64_t) low) << 32);
#else
	res64 = arg64;
#endif

	return res64;
}

/**
 * @brief Converts an auth_stat enum to a string
 *
 * @param[in] why The stat to convert
 *
 * @return String describing the status
 */

const char *auth_stat2str(enum auth_stat why)
{
	switch (why) {
	case AUTH_OK:
		return "AUTH_OK";

	case AUTH_BADCRED:
		return "AUTH_BADCRED";

	case AUTH_REJECTEDCRED:
		return "AUTH_REJECTEDCRED";

	case AUTH_BADVERF:
		return "AUTH_BADVERF";

	case AUTH_REJECTEDVERF:
		return "AUTH_REJECTEDVERF";

	case AUTH_TOOWEAK:
		return "AUTH_TOOWEAK";

	case AUTH_INVALIDRESP:
		return "AUTH_INVALIDRESP";

	case AUTH_FAILED:
		return "AUTH_FAILED";

	case RPCSEC_GSS_CREDPROBLEM:
		return "RPCSEC_GSS_CREDPROBLEM";

	case RPCSEC_GSS_CTXPROBLEM:
		return "RPCSEC_GSS_CTXPROBLEM";
	}

	return "UNKNOWN AUTH";
}

/* Error conversion routines */

/**
 * @brief Convert a FSAL status.major error to a nfsv4 status.
 *
 * @param[in] status The FSAL status
 * @param[in] where String identifying the caller
 *
 * @return the converted NFSv4 status.
 *
 */
nfsstat4 nfs4_Errno_verbose(fsal_status_t status, const char *where)
{
	nfsstat4 nfserror = NFS4ERR_INVAL;

	switch (status.major) {
	case ERR_FSAL_NO_ERROR:
		nfserror = NFS4_OK;
		break;

	case ERR_FSAL_NOMEM:
		nfserror = NFS4ERR_SERVERFAULT;
		break;

	case ERR_FSAL_SYMLINK:
		nfserror = NFS4ERR_SYMLINK;
		break;

	case ERR_FSAL_BADTYPE:
	case ERR_FSAL_INVAL:
	case ERR_FSAL_OVERFLOW:
		nfserror = NFS4ERR_INVAL;
		break;

	case ERR_FSAL_NOTDIR:
		nfserror = NFS4ERR_NOTDIR;
		break;

	case ERR_FSAL_EXIST:
		nfserror = NFS4ERR_EXIST;
		break;

	case ERR_FSAL_NOTEMPTY:
		nfserror = NFS4ERR_NOTEMPTY;
		break;

	case ERR_FSAL_NOENT:
		nfserror = NFS4ERR_NOENT;
		break;

	case ERR_FSAL_NOT_OPENED:
	case ERR_FSAL_BLOCKED:
	case ERR_FSAL_INTERRUPT:
	case ERR_FSAL_NOT_INIT:
	case ERR_FSAL_ALREADY_INIT:
	case ERR_FSAL_BAD_INIT:
	case ERR_FSAL_TIMEOUT:
	case ERR_FSAL_IO:
		if (status.major == ERR_FSAL_IO && status.minor != 0)
			LogCrit(COMPONENT_NFS_V4,
				"Error %s with error code %d in %s converted "
				"to NFS4ERR_IO but was set non-retryable",
				msg_fsal_err(status.major), status.minor,
				where);
		else
			LogCrit(COMPONENT_NFS_V4,
				"Error %s in %s converted to NFS4ERR_IO but "
				"was set non-retryable",
				msg_fsal_err(status.major), where);
		nfserror = NFS4ERR_IO;
		break;

	case ERR_FSAL_NXIO:
		nfserror = NFS4ERR_NXIO;
		break;

	case ERR_FSAL_ACCESS:
		nfserror = NFS4ERR_ACCESS;
		break;

	case ERR_FSAL_PERM:
	case ERR_FSAL_SEC:
		nfserror = NFS4ERR_PERM;
		break;

	case ERR_FSAL_NOSPC:
		nfserror = NFS4ERR_NOSPC;
		break;

	case ERR_FSAL_ISDIR:
		nfserror = NFS4ERR_ISDIR;
		break;

	case ERR_FSAL_ROFS:
		nfserror = NFS4ERR_ROFS;
		break;

	case ERR_FSAL_NAMETOOLONG:
		nfserror = NFS4ERR_NAMETOOLONG;
		break;

	case ERR_FSAL_STALE:
	case ERR_FSAL_FHEXPIRED:
		nfserror = NFS4ERR_STALE;
		break;

	case ERR_FSAL_DQUOT:
	case ERR_FSAL_NO_QUOTA:
		nfserror = NFS4ERR_DQUOT;
		break;

	case ERR_FSAL_NOTSUPP:
		nfserror = NFS4ERR_NOTSUPP;
		break;

	case ERR_FSAL_ATTRNOTSUPP:
		nfserror = NFS4ERR_ATTRNOTSUPP;
		break;

	case ERR_FSAL_UNION_NOTSUPP:
		nfserror = NFS4ERR_UNION_NOTSUPP;
		break;

	case ERR_FSAL_DELAY:
		nfserror = NFS4ERR_DELAY;
		break;

	case ERR_FSAL_FBIG:
		nfserror = NFS4ERR_FBIG;
		break;

	case ERR_FSAL_FILE_OPEN:
		nfserror = NFS4ERR_FILE_OPEN;
		break;

	case ERR_FSAL_BADCOOKIE:
		nfserror = NFS4ERR_BAD_COOKIE;
		break;

	case ERR_FSAL_TOOSMALL:
		nfserror = NFS4ERR_TOOSMALL;
		break;

	case ERR_FSAL_NO_DATA:
	case ERR_FSAL_FAULT:
	case ERR_FSAL_SERVERFAULT:
		nfserror = NFS4ERR_SERVERFAULT;
		break;

	case ERR_FSAL_DEADLOCK:
		nfserror = NFS4ERR_DEADLOCK;
		break;

	case ERR_FSAL_XDEV:
		nfserror = NFS4ERR_XDEV;
		break;

	case ERR_FSAL_BADHANDLE:
		nfserror = NFS4ERR_BADHANDLE;
		break;

	case ERR_FSAL_MLINK:
		nfserror = NFS4ERR_MLINK;
		break;

	case ERR_FSAL_SHARE_DENIED:
		nfserror = NFS4ERR_SHARE_DENIED;
		break;

	case ERR_FSAL_LOCKED:
		nfserror = NFS4ERR_LOCKED;
		break;

	case ERR_FSAL_IN_GRACE:
		nfserror = NFS4ERR_GRACE;
		break;

	case ERR_FSAL_BAD_RANGE:
		nfserror = NFS4ERR_BAD_RANGE;
		break;

	case ERR_FSAL_BADNAME:
		nfserror = NFS4ERR_BADNAME;
		break;

	case ERR_FSAL_NOXATTR:
		nfserror = NFS4ERR_NOXATTR;
		break;

	case ERR_FSAL_XATTR2BIG:
		nfserror = NFS4ERR_XATTR2BIG;
		break;

	case ERR_FSAL_CROSS_JUNCTION:
	case ERR_FSAL_NO_ACE:
	case ERR_FSAL_STILL_IN_USE:
		/* Should not occur */
		LogDebug(COMPONENT_NFS_V4,
			 "Line %u should never be reached in nfs4_Errno from %s for cache_status=%u",
			 __LINE__, where, status.major);
		nfserror = NFS4ERR_INVAL;
		break;
	}

	return nfserror;
}

/**
 *
 * @brief Convert a FSAL status.major error to a nfsv3 status.
 *
 * @param[in] status Input FSAL status
 * @param[in] where String identifying caller
 *
 * @return the converted NFSv3 status.
 *
 */
#ifdef _USE_NFS3
nfsstat3 nfs3_Errno_verbose(fsal_status_t status, const char *where)
{
	nfsstat3 nfserror = NFS3ERR_INVAL;

	switch (status.major) {
	case ERR_FSAL_NO_ERROR:
		nfserror = NFS3_OK;
		break;

	case ERR_FSAL_NOMEM:
	case ERR_FSAL_FILE_OPEN:
	case ERR_FSAL_NOT_OPENED:
	case ERR_FSAL_IO:
		if (status.major == ERR_FSAL_IO && status.minor != 0)
			LogCrit(COMPONENT_NFSPROTO,
				"Error %s with error code %d in %s converted "
				"to NFS3ERR_IO but was set non-retryable",
				msg_fsal_err(status.major), status.minor,
				where);
		else
			LogCrit(COMPONENT_NFSPROTO,
				"Error %s in %s converted to NFS3ERR_IO but "
				"was set non-retryable",
				msg_fsal_err(status.major), where);
		nfserror = NFS3ERR_IO;
		break;

	case ERR_FSAL_NXIO:
		nfserror = NFS3ERR_NXIO;
		break;

	case ERR_FSAL_INVAL:
	case ERR_FSAL_OVERFLOW:
		nfserror = NFS3ERR_INVAL;
		break;

	case ERR_FSAL_NOTDIR:
		nfserror = NFS3ERR_NOTDIR;
		break;

	case ERR_FSAL_EXIST:
		nfserror = NFS3ERR_EXIST;
		break;

	case ERR_FSAL_NOTEMPTY:
		nfserror = NFS3ERR_NOTEMPTY;
		break;

	case ERR_FSAL_NOENT:
		nfserror = NFS3ERR_NOENT;
		break;

	case ERR_FSAL_ACCESS:
		nfserror = NFS3ERR_ACCES;
		break;

	case ERR_FSAL_PERM:
	case ERR_FSAL_SEC:
		nfserror = NFS3ERR_PERM;
		break;

	case ERR_FSAL_NOSPC:
		nfserror = NFS3ERR_NOSPC;
		break;

	case ERR_FSAL_ISDIR:
		nfserror = NFS3ERR_ISDIR;
		break;

	case ERR_FSAL_ROFS:
		nfserror = NFS3ERR_ROFS;
		break;

	case ERR_FSAL_STALE:
	case ERR_FSAL_FHEXPIRED:
		nfserror = NFS3ERR_STALE;
		break;

	case ERR_FSAL_DQUOT:
	case ERR_FSAL_NO_QUOTA:
		nfserror = NFS3ERR_DQUOT;
		break;

	case ERR_FSAL_SYMLINK:
	case ERR_FSAL_BADTYPE:
		nfserror = NFS3ERR_BADTYPE;
		break;

	case ERR_FSAL_NOTSUPP:
	case ERR_FSAL_ATTRNOTSUPP:
	case ERR_FSAL_UNION_NOTSUPP:
		nfserror = NFS3ERR_NOTSUPP;
		break;

	case ERR_FSAL_DELAY:
	case ERR_FSAL_SHARE_DENIED:
	case ERR_FSAL_LOCKED:
		nfserror = NFS3ERR_JUKEBOX;
		break;

	case ERR_FSAL_NAMETOOLONG:
		nfserror = NFS3ERR_NAMETOOLONG;
		break;

	case ERR_FSAL_FBIG:
		nfserror = NFS3ERR_FBIG;
		break;

	case ERR_FSAL_BADCOOKIE:
		nfserror = NFS3ERR_BAD_COOKIE;
		break;

	case ERR_FSAL_TOOSMALL:
		nfserror = NFS3ERR_TOOSMALL;
		break;

	case ERR_FSAL_NO_DATA:
	case ERR_FSAL_FAULT:
	case ERR_FSAL_SERVERFAULT:
	case ERR_FSAL_DEADLOCK:
		nfserror = NFS3ERR_SERVERFAULT;
		break;

	case ERR_FSAL_XDEV:
		nfserror = NFS3ERR_XDEV;
		break;

	case ERR_FSAL_BADNAME:
		nfserror = NFS3ERR_INVAL;
		break;

	case ERR_FSAL_BADHANDLE:
		nfserror = NFS3ERR_BADHANDLE;
		break;

	case ERR_FSAL_MLINK:
		nfserror = NFS3ERR_MLINK;
		break;

	case ERR_FSAL_IN_GRACE:
		nfserror = NFS3ERR_JUKEBOX;
		break;

	case ERR_FSAL_CROSS_JUNCTION:
	case ERR_FSAL_BLOCKED:
	case ERR_FSAL_INTERRUPT:
	case ERR_FSAL_NOT_INIT:
	case ERR_FSAL_ALREADY_INIT:
	case ERR_FSAL_BAD_INIT:
	case ERR_FSAL_TIMEOUT:
	case ERR_FSAL_NO_ACE:
	case ERR_FSAL_BAD_RANGE:
	case ERR_FSAL_STILL_IN_USE:
	case ERR_FSAL_NOXATTR:
	case ERR_FSAL_XATTR2BIG:
		/* Should not occur */
		LogDebug(COMPONENT_NFSPROTO,
			 "Line %u should never be reached in nfs3_Errno from %s for FSAL error=%s",
			 __LINE__, where, msg_fsal_err(status.major));
		nfserror = NFS3ERR_INVAL;
		break;
	}

	return nfserror;
}				/* nfs3_Errno */
#endif
