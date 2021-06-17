#ifndef FSAL_LOCAL_FS_H
#define FSAL_LOCAL_FS_H

#include "fsal_api.h"

#if !GSH_CAN_HOST_LOCAL_FS

static inline void release_posix_file_systems(void) {}
#ifdef USE_DBUS
static inline void dbus_cache_init(void) {}
#endif

#else

int open_dir_by_path_walk(int first_fd, const char *path, struct stat *stat);

extern pthread_rwlock_t fs_lock;

int populate_posix_file_systems(const char *path);

int resolve_posix_filesystem(const char *path,
			     struct fsal_module *fsal,
			     struct fsal_export *exp,
			     claim_filesystem_cb claimfs,
			     unclaim_filesystem_cb unclaim,
			     struct fsal_filesystem **root_fs);

void release_posix_file_systems(void);

enum release_claims {
	UNCLAIM_WARN,
	UNCLAIM_SKIP,
};

bool release_posix_file_system(struct fsal_filesystem *fs,
			       enum release_claims release_claims);

int re_index_fs_fsid(struct fsal_filesystem *fs,
		     enum fsid_type fsid_type,
		     struct fsal_fsid__ *fsid);

int re_index_fs_dev(struct fsal_filesystem *fs,
		    struct fsal_dev__ *dev);

int change_fsid_type(struct fsal_filesystem *fs,
		     enum fsid_type fsid_type);

struct fsal_filesystem *lookup_fsid_locked(struct fsal_fsid__ *fsid,
					   enum fsid_type fsid_type);
struct fsal_filesystem *lookup_dev_locked(struct fsal_dev__ *dev);
struct fsal_filesystem *lookup_fsid(struct fsal_fsid__ *fsid,
				    enum fsid_type fsid_type);
struct fsal_filesystem *lookup_dev(struct fsal_dev__ *dev);

int claim_posix_filesystems(const char *path,
			    struct fsal_module *fsal,
			    struct fsal_export *exp,
			    claim_filesystem_cb claimfs,
			    unclaim_filesystem_cb unclaim,
			    struct fsal_filesystem **root_fs,
			    struct stat *statbuf);

bool is_filesystem_exported(struct fsal_filesystem *fs,
			    struct fsal_export *exp);

void unclaim_all_export_maps(struct fsal_export *exp);

#ifdef USE_DBUS
void dbus_cache_init(void);
#endif

#endif		/* GSH_CAN_HOST_LOCAL_FS */

#endif				/* FSAL_LOCAL_FS_H */
