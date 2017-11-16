#include "config.h"
#include "log.h"
#include "nfs_core.h"
#include "nfs4.h"
#include "sal_functions.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#include "bsd-base64.h"
#include "client_mgr.h"
#include "fsal.h"

#define NFS_V4_RECOV_DIR "v4recov"
#define NFS_V4_OLD_DIR "v4old"

char v4_recov_dir[PATH_MAX];
char v4_recov_link[PATH_MAX];
char recov_root[PATH_MAX];

/**
 * @brief convert clientid opaque bytes as a hex string for mkdir purpose.
 *
 * @param[in,out] dspbuf The buffer.
 * @param[in]     value  The bytes to display
 * @param[in]     len    The number of bytes to display
 *
 * @return the bytes remaining in the buffer.
 *
 */
static int fs_convert_opaque_value_max_for_dir(struct display_buffer *dspbuf,
					       void *value,
					       int len,
					       int max)
{
	unsigned int i = 0;
	int          b_left = display_start(dspbuf);
	int          cpy = len;

	if (b_left <= 0)
		return 0;

	/* Check that the length is ok
	 * If the value is empty, display EMPTY value. */
	if (len <= 0 || len > max)
		return 0;

	/* If the value is NULL, display NULL value. */
	if (value == NULL)
		return 0;

	/* Determine if the value is entirely printable characters, */
	/* and it contains no slash character (reserved for filename) */
	for (i = 0; i < len; i++)
		if ((!isprint(((char *)value)[i])) ||
		    (((char *)value)[i] == '/'))
			break;

	if (i == len) {
		/* Entirely printable character, so we will just copy the
		 * characters into the buffer (to the extent there is room
		 * for them).
		 */
		b_left = display_len_cat(dspbuf, value, cpy);
	} else {
		b_left = display_opaque_bytes(dspbuf, value, cpy);
	}

	if (b_left <= 0)
		return 0;

	return b_left;
}

/**
 * @brief generate a name that identifies this client
 *
 * This name will be used to know that a client was talking to the
 * server before a restart so that it will be allowed to do reclaims
 * during grace period.
 *
 * @param[in] clientid Client record
 */
static void fs_create_clid_name(nfs_client_id_t *clientid)
{
	nfs_client_record_t *cl_rec = clientid->cid_client_record;
	const char *str_client_addr = "(unknown)";
	char cidstr[PATH_MAX] = { 0, };
	struct display_buffer dspbuf = {sizeof(cidstr), cidstr, cidstr};
	char cidstr_len[20];
	int total_len;

	/* get the caller's IP addr */
	if (clientid->gsh_client != NULL)
		str_client_addr = clientid->gsh_client->hostaddr_str;

	if (fs_convert_opaque_value_max_for_dir(&dspbuf,
					     cl_rec->cr_client_val,
					     cl_rec->cr_client_val_len,
					     PATH_MAX) > 0) {
		/* fs_convert_opaque_value_max_for_dir does not prefix
		 * the "(<length>:". So we need to do it here */
		snprintf(cidstr_len, sizeof(cidstr_len), "%zd", strlen(cidstr));
		total_len = strlen(cidstr) + strlen(str_client_addr) + 5 +
			    strlen(cidstr_len);
		/* hold both long form clientid and IP */
		clientid->cid_recov_tag = gsh_malloc(total_len);

		(void) snprintf(clientid->cid_recov_tag, total_len,
				"%s-(%s:%s)",
				str_client_addr, cidstr_len, cidstr);
	}

	LogDebug(COMPONENT_CLIENTID, "Created client name [%s]",
		 clientid->cid_recov_tag);
}

void fs_create_recov_dir(void)
{
	int err;
	char *newdir;

	snprintf(recov_root, PATH_MAX, "%s", NFS_V4_RECOV_ROOT);

	err = mkdir(recov_root, 0700);
	if (err == -1 && errno != EEXIST) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to create v4 recovery dir (%s), errno=%d",
			 recov_root, errno);
	}

	snprintf(v4_recov_dir, sizeof(v4_recov_dir), "%s/%s", recov_root,
		 NFS_V4_RECOV_DIR);
	err = mkdir(v4_recov_dir, 0700);
	if (err == -1 && errno != EEXIST) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to create v4 recovery dir(%s), errno=%d",
			 v4_recov_dir, errno);
	}

	/* Populate link path string, but don't try to create it yet */
	snprintf(v4_recov_link, sizeof(v4_recov_link), "%s/%s/node%d",
		 recov_root, NFS_V4_RECOV_DIR, g_nodeid);

	snprintf(v4_recov_dir, sizeof(v4_recov_dir), "%s.XXXXXX",
		 v4_recov_link);

	newdir = mkdtemp(v4_recov_dir);
	if (newdir != v4_recov_dir) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to create v4 recovery dir(%s), errno=%d",
			 v4_recov_dir, errno);
	}
}

void fs_add_clid(nfs_client_id_t *clientid)
{
	int err = 0;
	char path[PATH_MAX] = {0}, segment[NAME_MAX + 1] = {0};
	int length, position = 0;

	fs_create_clid_name(clientid);

	/* break clientid down if it is greater than max dir name */
	/* and create a directory hierarchy to represent the clientid. */
	snprintf(path, sizeof(path), "%s", v4_recov_dir);

	length = strlen(clientid->cid_recov_tag);
	while (position < length) {
		/* if the (remaining) clientid is shorter than 255 */
		/* create the last level of dir and break out */
		int len = strlen(&clientid->cid_recov_tag[position]);

		if (len <= NAME_MAX) {
			strcat(path, "/");
			strncat(path, &clientid->cid_recov_tag[position], len);
			err = mkdir(path, 0700);
			break;
		}
		/* if (remaining) clientid is longer than 255, */
		/* get the next 255 bytes and create a subdir */
		strncpy(segment, &clientid->cid_recov_tag[position], NAME_MAX);
		strcat(path, "/");
		strncat(path, segment, NAME_MAX);
		err = mkdir(path, 0700);
		if (err == -1 && errno != EEXIST)
			break;
		position += NAME_MAX;
	}

	if (err == -1 && errno != EEXIST) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to create client in recovery dir (%s), errno=%d",
			 path, errno);
	} else {
		LogDebug(COMPONENT_CLIENTID, "Created client dir [%s]", path);
	}
}

/**
 * @brief Remove the revoked file handles created under a specific
 * client-id path on the stable storage.
 *
 * @param[in] path Path of the client-id on the stable storage.
 */
static void fs_rm_revoked_handles(char *path)
{
	DIR *dp;
	struct dirent *dentp;
	char del_path[PATH_MAX];

	dp = opendir(path);
	if (dp == NULL) {
		LogEvent(COMPONENT_CLIENTID, "opendir %s failed errno=%d",
			path, errno);
		return;
	}
	for (dentp = readdir(dp); dentp != NULL; dentp = readdir(dp)) {
		if (!strcmp(dentp->d_name, ".") ||
				!strcmp(dentp->d_name, "..") ||
				dentp->d_name[0] != '\x1') {
			continue;
		}

		snprintf(del_path, sizeof(del_path), "%s/%s",
			 path, dentp->d_name);

		if (unlink(del_path) < 0) {
			LogEvent(COMPONENT_CLIENTID,
					"unlink of %s failed errno: %d",
					del_path,
					errno);
		}
	}
	(void)closedir(dp);
}

static void fs_rm_clid_impl(char *recov_dir, char *parent_path, int position)
{
	int err;
	char *path;
	char *segment;
	int len, segment_len;
	int total_len;

	if (recov_dir == NULL)
		return;

	len = strlen(recov_dir);
	if (position == len) {
		/* We are at the tail directory of the clid,
		* remove revoked handles, if any.
		*/
		fs_rm_revoked_handles(parent_path);
		return;
	}
	segment = gsh_malloc(NAME_MAX+1);

	memset(segment, 0, NAME_MAX+1);
	strncpy(segment, &recov_dir[position], NAME_MAX);
	segment_len = strlen(segment);

	/* allocate enough memory for the new part of the string */
	/* which is parent path + '/' + new segment */
	total_len = strlen(parent_path) + segment_len + 2;
	path = gsh_malloc(total_len);

	memset(path, 0, total_len);
	(void) snprintf(path, total_len, "%s/%s",
			parent_path, segment);
	/* free setment as it has no use now */
	gsh_free(segment);

	/* recursively remove the directory hirerchy which represent the
	 *clientid
	 */
	fs_rm_clid_impl(recov_dir, path, position + segment_len);

	err = rmdir(path);
	if (err == -1) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to remove client recovery dir (%s), errno=%d",
			 path, errno);
	} else {
		LogDebug(COMPONENT_CLIENTID, "Removed client dir [%s]", path);
	}
	gsh_free(path);
}

void fs_rm_clid(nfs_client_id_t *clientid)
{
	char *recov_tag = clientid->cid_recov_tag;

	clientid->cid_recov_tag = NULL;
	fs_rm_clid_impl(recov_tag, v4_recov_dir, 0);
	gsh_free(recov_tag);
}

/**
 * @brief Create the client reclaim list
 *
 * When not doing a take over, first open the old state dir and read
 * in those entries.  The reason for the two directories is in case of
 * a reboot/restart during grace period.  Next, read in entries from
 * the recovery directory and then move them into the old state
 * directory.  if called due to a take over, nodeid will be nonzero.
 * in this case, add that node's clientids to the existing list.  Then
 * move those entries into the old state directory.
 *
 * @param[in] dp       Recovery directory
 * @param[in] srcdir   Path to the source directory on failover
 *
 * @return POSIX error codes.
 */
static int fs_read_recov_clids_impl(const char *parent_path,
				    char *clid_str,
				    add_clid_entry_hook add_clid_entry,
				    add_rfh_entry_hook add_rfh_entry)
{
	struct dirent *dentp;
	DIR *dp;
	clid_entry_t *new_ent;
	char *sub_path = NULL;
	char *build_clid = NULL;
	int rc = 0;
	int num = 0;
	char *ptr, *ptr2;
	char temp[10];
	int cid_len, len;
	int segment_len;
	int total_len;
	int total_clid_len;

	dp = opendir(parent_path);
	if (dp == NULL) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to open v4 recovery dir (%s), errno=%d",
			 parent_path, errno);
		return -1;
	}

	for (dentp = readdir(dp); dentp != NULL; dentp = readdir(dp)) {
		/* don't add '.' and '..' entry */
		if (!strcmp(dentp->d_name, ".") || !strcmp(dentp->d_name, ".."))
			continue;

		/* Skip names that start with '\x1' as they are files
		 * representing revoked file handles
		 */
		if (dentp->d_name[0] == '\x1')
			continue;

		num++;

		/* construct the path by appending the subdir for the
		 * next readdir. This recursion keeps reading the
		 * subdirectory until reaching the end.
		 */
		segment_len = strlen(dentp->d_name);
		total_len = segment_len + 2 + strlen(parent_path);
		sub_path = gsh_malloc(total_len);

		memset(sub_path, 0, total_len);

		strcpy(sub_path, parent_path);
		strcat(sub_path, "/");
		strncat(sub_path, dentp->d_name, segment_len);
		/* keep building the clientid str by recursively */
		/* reading the directory structure */
		total_clid_len = segment_len + 1;
		if (clid_str)
			total_clid_len += strlen(clid_str);
		build_clid = gsh_calloc(1, total_clid_len);
		if (clid_str)
			strcpy(build_clid, clid_str);
		strncat(build_clid, dentp->d_name, segment_len);

		rc = fs_read_recov_clids_impl(sub_path,
					      build_clid,
					      add_clid_entry,
					      add_rfh_entry);

		/* after recursion, if the subdir has no non-hidden
		 * directory this is the end of this clientid str. Add
		 * the clientstr to the list.
		 */
		if (rc == 0) {
			/* the clid format is
			 * <IP>-(clid-len:long-form-clid-in-string-form)
			 * make sure this reconstructed string is valid
			 * by comparing clid-len and the actual
			 * long-form-clid length in the string. This is
			 * to prevent getting incompleted strings that
			 * might exist due to program crash.
			 */
			if (strlen(build_clid) >= PATH_MAX) {
				LogEvent(COMPONENT_CLIENTID,
					"invalid clid format: %s, too long",
					build_clid);
				gsh_free(sub_path);
				gsh_free(build_clid);
				continue;
			}
			ptr = strchr(build_clid, '(');
			if (ptr == NULL) {
				LogEvent(COMPONENT_CLIENTID,
					 "invalid clid format: %s",
					 build_clid);
				gsh_free(sub_path);
				gsh_free(build_clid);
				continue;
			}
			ptr2 = strchr(ptr, ':');
			if (ptr2 == NULL) {
				LogEvent(COMPONENT_CLIENTID,
					 "invalid clid format: %s",
					 build_clid);
				gsh_free(sub_path);
				gsh_free(build_clid);
				continue;
			}
			len = ptr2-ptr-1;
			if (len >= 9) {
				LogEvent(COMPONENT_CLIENTID,
					 "invalid clid format: %s",
					 build_clid);
				gsh_free(sub_path);
				gsh_free(build_clid);
				continue;
			}
			strncpy(temp, ptr+1, len);
			temp[len] = 0;
			cid_len = atoi(temp);
			len = strlen(ptr2);
			if ((len == (cid_len+2)) && (ptr2[len-1] == ')')) {
				new_ent = add_clid_entry(build_clid);
				LogDebug(COMPONENT_CLIENTID,
					 "added %s to clid list",
					 new_ent->cl_name);
			}
		}
		gsh_free(build_clid);
		gsh_free(sub_path);
	}

	(void)closedir(dp);

	return num;
}

static void fs_read_recov_clids_recover(add_clid_entry_hook add_clid_entry,
					add_rfh_entry_hook add_rfh_entry)
{
	int rc;

	rc = fs_read_recov_clids_impl(v4_recov_link, NULL,
				      add_clid_entry,
				      add_rfh_entry);
	if (rc == -1) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to read v4 recovery dir (%s)",
			 v4_recov_link);
		return;
	}
}

/**
 * @brief Load clients for recovery, with no lock
 *
 * @param[in] nodeid Node, on takeover
 */
void fs_read_recov_clids_takeover(nfs_grace_start_t *gsp,
				  add_clid_entry_hook add_clid_entry,
				  add_rfh_entry_hook add_rfh_entry)
{
	int rc;
	char path[PATH_MAX];

	if (!gsp) {
		fs_read_recov_clids_recover(add_clid_entry, add_rfh_entry);
		return;
	}

	switch (gsp->event) {
	case EVENT_TAKE_NODEID:
		snprintf(path, sizeof(path), "%s/%s/node%d",
			 recov_root, NFS_V4_RECOV_DIR,
			 gsp->nodeid);
		break;
	default:
		LogWarn(COMPONENT_STATE, "Recovery unknown event: %d",
				gsp->event);
		return;
	}

	LogEvent(COMPONENT_CLIENTID, "Recovery for nodeid %d dir (%s)",
		 gsp->nodeid, path);

	rc = fs_read_recov_clids_impl(path, NULL,
				      add_clid_entry,
				      add_rfh_entry);
	if (rc == -1) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to read v4 recovery dir (%s)", path);
		return;
	}
}

static void fs_clean_old_recov_dir_impl(char *parent_path)
{
	DIR *dp;
	struct dirent *dentp;
	char *path = NULL;
	int rc;
	int total_len;

	dp = opendir(parent_path);
	if (dp == NULL) {
		LogEvent(COMPONENT_CLIENTID,
			 "Failed to open old v4 recovery dir (%s), errno=%d",
			 parent_path, errno);
		return;
	}

	for (dentp = readdir(dp); dentp != NULL; dentp = readdir(dp)) {
		/* don't remove '.' and '..' entry */
		if (!strcmp(dentp->d_name, ".") || !strcmp(dentp->d_name, ".."))
			continue;

		/* If there is a filename starting with '\x1', then it is
		 * a revoked handle, go ahead and remove it.
		 */
		if (dentp->d_name[0] == '\x1') {
			char del_path[PATH_MAX];

			snprintf(del_path, sizeof(del_path), "%s/%s",
				 parent_path, dentp->d_name);

			if (unlink(del_path) < 0) {
				LogEvent(COMPONENT_CLIENTID,
						"unlink of %s failed errno: %d",
						del_path,
						errno);
			}

			continue;
		}

		/* This is a directory, we need process files in it! */
		total_len = strlen(parent_path) + strlen(dentp->d_name) + 2;
		path = gsh_malloc(total_len);

		snprintf(path, total_len, "%s/%s", parent_path, dentp->d_name);

		fs_clean_old_recov_dir_impl(path);
		rc = rmdir(path);
		if (rc == -1) {
			LogEvent(COMPONENT_CLIENTID,
				 "Failed to remove %s, errno=%d", path, errno);
		}
		gsh_free(path);
	}
	(void)closedir(dp);
	rc = rmdir(parent_path);
	if (rc != 0)
		LogEvent(COMPONENT_CLIENTID, "Failed to remove %s, errno=%d",
				parent_path, errno);
}

void fs_swap_recov_dir(void)
{
	int ret;
	char old_pathbuf[PATH_MAX];
	char tmp_link[PATH_MAX];
	char *old_path;

	/* save off the old link path so we can do some cleanup afterward */
	old_path = realpath(v4_recov_link, old_pathbuf);

	/* Make a new symlink at a temporary location, pointing to new dir */
	snprintf(tmp_link, PATH_MAX, "%s.tmp", v4_recov_link);

	/* unlink old symlink, if any */
	ret = unlink(tmp_link);
	if (ret != 0 && errno != ENOENT) {
		LogEvent(COMPONENT_CLIENTID,
			 "Unable to remove recoverydir symlink: %d", errno);
		return;
	}

	/* make a new symlink in a temporary spot */
	ret = symlink(v4_recov_dir, tmp_link);
	if (ret != 0) {
		LogEvent(COMPONENT_CLIENTID,
			 "Unable to create recoverydir symlink: %d", errno);
		return;
	}

	/* rename tmp link into place */
	ret = rename(tmp_link, v4_recov_link);
	if (ret != 0) {
		LogEvent(COMPONENT_CLIENTID,
			 "Unable to rename recoverydir symlink: %d", errno);
		return;
	}

	/* now clean up old path, if any */
	if (old_path)
		fs_clean_old_recov_dir_impl(old_path);
}

void fs_add_revoke_fh(nfs_client_id_t *delr_clid, nfs_fh4 *delr_handle)
{
	char rhdlstr[NAME_MAX];
	char path[PATH_MAX] = {0}, segment[NAME_MAX + 1] = {0};
	int length, position = 0;
	int fd;
	int retval;

	/* Convert nfs_fh4_val into base64 encoded string */
	retval = base64url_encode(delr_handle->nfs_fh4_val,
				  delr_handle->nfs_fh4_len,
				  rhdlstr, sizeof(rhdlstr));
	assert(retval != -1);

	/* Parse through the clientid directory structure */
	assert(delr_clid->cid_recov_tag != NULL);

	snprintf(path, sizeof(path), "%s", v4_recov_dir);
	length = strlen(delr_clid->cid_recov_tag);
	while (position < length) {
		int len = strlen(&delr_clid->cid_recov_tag[position]);

		if (len <= NAME_MAX) {
			strcat(path, "/");
			strncat(path, &delr_clid->cid_recov_tag[position], len);
			strcat(path, "/\x1"); /* Prefix 1 to converted fh */
			strncat(path, rhdlstr, strlen(rhdlstr));
			fd = creat(path, 0700);
			if (fd < 0) {
				LogEvent(COMPONENT_CLIENTID,
					"Failed to record revoke errno:%d\n",
					errno);
			} else {
				close(fd);
			}
			return;
		}
		strncpy(segment, &delr_clid->cid_recov_tag[position], NAME_MAX);
		strcat(path, "/");
		strncat(path, segment, NAME_MAX);
		position += NAME_MAX;
	}
}

struct nfs4_recovery_backend fs_backend = {
	.recovery_init = fs_create_recov_dir,
	.recovery_cleanup = fs_swap_recov_dir,
	.recovery_read_clids = fs_read_recov_clids_takeover,
	.add_clid = fs_add_clid,
	.rm_clid = fs_rm_clid,
	.add_revoke_fh = fs_add_revoke_fh,
};

void fs_backend_init(struct nfs4_recovery_backend **backend)
{
	*backend = &fs_backend;
}
