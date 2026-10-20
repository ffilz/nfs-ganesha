// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Darwin (macOS) OS layer.
 *
 * The Linux and FreeBSD versions of this file provide two unrelated groups of
 * functions:
 *
 *  1. vfs_readents / to_vfs_dirent / vfs_utimesat / vfs_utimes -- raw local
 *     filesystem access used by FSAL_VFS.
 *  2. setuser / setgroup / set_threadgroups -- per-thread credential switching
 *     used by fsal_set_credentials().
 *
 * Neither group is reachable on Darwin, where GSH_CAN_HOST_LOCAL_FS defaults
 * to OFF (see src/CMakeLists.txt):
 *
 *  - FSAL_VFS, FSAL_GPFS, FSAL_LUSTRE and FSAL_XFS are not built without it.
 *  - fsal_set_credentials() and fsal_restore_ganesha_credentials() are both
 *    inside #if GSH_CAN_HOST_LOCAL_FS in FSAL/access_check.c. Their only
 *    callers are FSAL_VFS and FSAL_GPFS.
 *
 * Rather than ship stubs that would fail silently (or, worse, trip the
 * LogFatal() in fsal_set_credentials), this file refuses to compile if a
 * future change enables local-filesystem hosting on macOS without providing
 * real implementations. Darwin's per-thread credential primitive is
 * pthread_setugid_np(2), which sets uid and gid together and therefore cannot
 * implement setuser() and setgroup() as separate operations -- porting this
 * properly needs a design decision, not a stub.
 */

#include "config.h"

#if GSH_CAN_HOST_LOCAL_FS
#error "os/darwin: GSH_CAN_HOST_LOCAL_FS is enabled, but Darwin has no \
implementation of setuser/setgroup/set_threadgroups. See the comment at the \
top of this file: pthread_setugid_np() sets uid and gid together, so this \
needs a design decision rather than a stub."
#endif

/*
 * Keep the translation unit non-empty: `ar`/`ranlib` on macOS warn about
 * object files with no symbols.
 */
const char gsh_os_darwin_subr_placeholder;
