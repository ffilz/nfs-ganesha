/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 * 02110-1301 USA
 *
 * -------------
 */

/* file.c
 * File I/O methods for VFS module
 */

#include "config.h"

#include <assert.h>
#include "fsal.h"
#include "FSAL/access_check.h"
#include "fsal_convert.h"
#include <unistd.h>
#include <fcntl.h>
#include "FSAL/fsal_commonlib.h"
#include "vfs_methods.h"
#include "os/subr.h"
#include "sal_data.h"

fsal_status_t vfs_open_my_fd(struct vfs_fsal_obj_handle *myself,
			     fsal_openflags_t openflags,
			     int posix_flags,
			     struct vfs_fd *my_fd)
{
	int fd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	assert(my_fd->fd == -1
	       && my_fd->openflags == FSAL_O_CLOSED && openflags != 0);

	LogFullDebug(COMPONENT_FSAL, "open_by_handle_at flags from %x to %x",
		     openflags, posix_flags);

	fd = vfs_fsal_open(myself, posix_flags, &fsal_error);

	if (fd < 0) {
		retval = -fd;
	} else {
		/* Save the file descriptor, make sure we only save the
		 * open modes that actually represent the open file.
		 */
		my_fd->fd = fd;
		my_fd->openflags = openflags & (FSAL_O_RDWR | FSAL_O_SYNC);
	}

	return fsalstat(fsal_error, retval);
}

fsal_status_t vfs_close_my_fd(struct vfs_fd *my_fd)
{
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	if (my_fd->fd >= 0 && my_fd->openflags != FSAL_O_CLOSED) {
		retval = close(my_fd->fd);
		if (retval < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
		}
		my_fd->fd = -1;
		my_fd->openflags = FSAL_O_CLOSED;
	}

	return fsalstat(fsal_error, retval);
}

/**
 * @brief Reopen the fd associated with the object handle.
 *
 * This function assures that the fd is open in the mode requested. If
 * the fd was already open, it closes it and reopens with the OR of the
 * requested modes.
 *
 * This function will return with the object handle lock held even if
 * an error occurred.
 *
 * @param[in] myself           File on which to operate
 * @param[in] openflags        Mode for open
 *
 * @return FSAL status.
 */

fsal_status_t vfs_reopen_obj(struct fsal_obj_handle *obj_hdl,
			     fsal_openflags_t openflags)
{
	struct vfs_fsal_obj_handle *myself;
	int posix_flags = 0;
	struct vfs_fd *my_fd;
	fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};

	/* Use the global file descriptor. */
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	/* Take read lock on object to protect file descriptor.
	 * We only take a read lock because we are not changing the
	 * state of the file descriptor.
	 */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	if ((my_fd->openflags & openflags) != openflags) {
		/* Switch to write lock on object to protect file descriptor. */
		PTHREAD_RWLOCK_unlock(&obj_hdl->lock);
		PTHREAD_RWLOCK_wrlock(&obj_hdl->lock);

		if ((my_fd->openflags & openflags) != openflags) {
			if (my_fd->openflags != FSAL_O_CLOSED) {
				/* Add whatevever mode file was in to
				 * FSAL_O_WRITE.
				 */
				openflags |= my_fd->openflags;

				/* Now close the already open descriptor. */
				status = vfs_close_my_fd(my_fd);

				if (FSAL_IS_ERROR(status))
					goto out;
			}

			fsal2posix_openflags(openflags, &posix_flags);

			/* Actually open the file */
			status = vfs_open_my_fd(myself, openflags,
						posix_flags, my_fd);

		}
	}

 out:

	return status;
}

/** vfs_open
 * called with appropriate locks taken at the cache inode level
 */

fsal_status_t vfs_open(struct fsal_obj_handle *obj_hdl,
		       fsal_openflags_t openflags)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	fsal_status_t status;
	int posix_flags = 0;

	fsal2posix_openflags(openflags, &posix_flags);

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	/* Take write lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->lock);

	status = vfs_open_my_fd(myself, openflags, posix_flags, my_fd);

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return status;
}

/* vfs_status
 * Let the caller peek into the file's open/close state.
 */

fsal_openflags_t vfs_status(struct fsal_obj_handle *obj_hdl)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	return my_fd->openflags;
}

/* vfs_read
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t vfs_read(struct fsal_obj_handle *obj_hdl,
		       uint64_t offset,
		       size_t buffer_size, void *buffer, size_t *read_amount,
		       bool *end_of_file)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	ssize_t nb_read;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(my_fd->fd >= 0 && my_fd->openflags != FSAL_O_CLOSED);

	nb_read = pread(my_fd->fd, buffer, buffer_size, offset);

	if (offset == -1 || nb_read == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	*read_amount = nb_read;

	/* dual eof condition */
	*end_of_file = ((nb_read == 0) /* most clients */ ||	/* ESXi */
			(((offset + nb_read) >= myself->attributes.filesize)))
	    ? true : false;

 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_write
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t vfs_write(struct fsal_obj_handle *obj_hdl,
			uint64_t offset,
			size_t buffer_size, void *buffer, size_t *write_amount,
			bool *fsal_stable)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	ssize_t nb_written;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(my_fd->fd >= 0 && my_fd->openflags != FSAL_O_CLOSED);

	fsal_set_credentials(op_ctx->creds);
	nb_written = pwrite(my_fd->fd, buffer, buffer_size, offset);

	if (offset == -1 || nb_written == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	*write_amount = nb_written;

	/* attempt stability */
	if (fsal_stable != NULL && *fsal_stable) {
		retval = fsync(my_fd->fd);
		if (retval == -1) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
		}
		*fsal_stable = true;
	}

 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	fsal_restore_ganesha_credentials();
	return fsalstat(fsal_error, retval);
}

/* vfs_commit
 * Commit a file range to storage.
 * for right now, fsync will have to do.
 */

fsal_status_t vfs_commit(struct fsal_obj_handle *obj_hdl,	/* sync */
			 off_t offset, size_t len)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(my_fd->fd >= 0 && my_fd->openflags != FSAL_O_CLOSED);

	retval = fsync(my_fd->fd);
	if (retval == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_lock_op
 * lock a region of the file
 * throw an error if the fd is not open.  The old fsal didn't
 * check this.
 */

fsal_status_t vfs_lock_op(struct fsal_obj_handle *obj_hdl,
			  void *p_owner,
			  fsal_lock_op_t lock_op,
			  fsal_lock_param_t *request_lock,
			  fsal_lock_param_t *conflicting_lock)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	struct flock lock_args;
	int fcntl_comm;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	if (my_fd->fd < 0 || my_fd->openflags == FSAL_O_CLOSED) {
		LogDebug(COMPONENT_FSAL,
			 "Attempting to lock with no file descriptor open");
		fsal_error = ERR_FSAL_FAULT;
		goto out;
	}
	if (p_owner != NULL) {
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}
	LogFullDebug(COMPONENT_FSAL,
		     "Locking: op:%d type:%d start:%" PRIu64 " length:%lu ",
		     lock_op, request_lock->lock_type, request_lock->lock_start,
		     request_lock->lock_length);
	if (lock_op == FSAL_OP_LOCKT) {
		fcntl_comm = F_GETLK;
	} else if (lock_op == FSAL_OP_LOCK || lock_op == FSAL_OP_UNLOCK) {
		fcntl_comm = F_SETLK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: Lock operation requested was not TEST, READ, or WRITE.");
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}

	if (request_lock->lock_type == FSAL_LOCK_R) {
		lock_args.l_type = F_RDLCK;
	} else if (request_lock->lock_type == FSAL_LOCK_W) {
		lock_args.l_type = F_WRLCK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: The requested lock type was not read or write.");
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}

	if (lock_op == FSAL_OP_UNLOCK)
		lock_args.l_type = F_UNLCK;

	lock_args.l_len = request_lock->lock_length;
	lock_args.l_start = request_lock->lock_start;
	lock_args.l_whence = SEEK_SET;

	/* flock.l_len being signed long integer, larger lock ranges may
	 * get mapped to negative values. As per 'man 3 fcntl', posix
	 * locks can accept negative l_len values which may lead to
	 * unlocking an unintended range. Better bail out to prevent that.
	 */
	if (lock_args.l_len < 0) {
		LogCrit(COMPONENT_FSAL,
			"The requested lock length is out of range- lock_args.l_len(%ld), request_lock_length(%lu)",
			lock_args.l_len, request_lock->lock_length);
		fsal_error = ERR_FSAL_BAD_RANGE;
		goto out;
	}

	errno = 0;
	retval = fcntl(my_fd->fd, fcntl_comm, &lock_args);
	if (retval && lock_op == FSAL_OP_LOCK) {
		retval = errno;
		if (conflicting_lock != NULL) {
			fcntl_comm = F_GETLK;
			if (fcntl(my_fd->fd, fcntl_comm, &lock_args)) {
				retval = errno;	/* we lose the inital error */
				LogCrit(COMPONENT_FSAL,
					"After failing a lock request, I couldn't even get the details of who owns the lock.");
				fsal_error = posix2fsal_error(retval);
				goto out;
			}
			if (conflicting_lock != NULL) {
				conflicting_lock->lock_length = lock_args.l_len;
				conflicting_lock->lock_start =
				    lock_args.l_start;
				conflicting_lock->lock_type = lock_args.l_type;
			}
		}
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	/* F_UNLCK is returned then the tested operation would be possible. */
	if (conflicting_lock != NULL) {
		if (lock_op == FSAL_OP_LOCKT && lock_args.l_type != F_UNLCK) {
			conflicting_lock->lock_length = lock_args.l_len;
			conflicting_lock->lock_start = lock_args.l_start;
			conflicting_lock->lock_type = lock_args.l_type;
		} else {
			conflicting_lock->lock_length = 0;
			conflicting_lock->lock_start = 0;
			conflicting_lock->lock_type = FSAL_NO_LOCK;
		}
	}
 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_close
 * Close the file if it is still open.
 * Yes, we ignor lock status.  Closing a file in POSIX
 * releases all locks but that is state and cache inode's problem.
 */

fsal_status_t vfs_close(struct fsal_obj_handle *obj_hdl)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_status_t status;

	assert(obj_hdl->type == REGULAR_FILE);
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	/* Take write lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->lock);

	status = vfs_close_my_fd(&myself->u.file);

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return status;
}

/* vfs_lru_cleanup
 * free non-essential resources at the request of cache inode's
 * LRU processing identifying this handle as stale enough for resource
 * trimming.
 */

fsal_status_t vfs_lru_cleanup(struct fsal_obj_handle *obj_hdl,
			      lru_actions_t requests)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	if (obj_hdl->type == REGULAR_FILE && my_fd->fd >= 0) {
		retval = close(my_fd->fd);
		my_fd->fd = -1;
		my_fd->openflags = FSAL_O_CLOSED;
	}
	if (retval == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/**
 * @brief Allocate a state_t structure
 *
 * @param[in] obj_hdl               File to open or parent directory
 * @param[in] state_type            Type of state to allocate
 *
 * @returns NULL on failure otherwise a state structure.
 */

struct state_t *vfs_alloc_state(struct fsal_obj_handle *obj_hdl,
				enum state_type state_type,
				struct state_t *related_state)
{
	struct state_t *state;
	size_t extra = sizeof(struct vfs_fd);

	state = gsh_calloc(1, sizeof(struct state_t) + extra);

	if (state != NULL) {
		state->state_obj = obj_hdl;
		state->state_type = state_type;
		if (state_type == STATE_TYPE_LOCK ||
		    state_type == STATE_TYPE_NLM_LOCK)
			state->state_data.lock.openstate = related_state;
	}

	return state;
}

/**
 * @brief Open a file descriptor for read or write
 *
 * This function opens a file for read or write.  The file should not
 * already be opened when this function is called.  The thread calling
 * this function will have hold the Cache inode content lock
 * exclusively and the FSAL may assume whatever private state it uses
 * to manage open/close status is protected.
 *
 * If Name is NULL, obj_hdl is the file itself, otherwise obj_hdl is the
 * parent directory.
 *
 * On an exclusive create, the upper layer may know the object handle
 * already, so it MAY call with name == NULL. In this case, the caller
 * expects just to check the verifier. The caller must hold the attr_lock
 * since the FSAL will update the attributes in checking the verifier.
 *
 * On a call with an existing object handle for an UNCHECKED create,
 * we can set the size to 0, because of this, the caller must hold the
 * attr_lock to update the attributes.
 *
 * If attributes are not set on create, the FSAL will set some minimal
 * attributes (for example, mode might be set to 0600).
 *
 * If an open by name succeeds and did not result in Ganesha creating a file,
 * the caller will need to do a subsequent permission check to confirm the
 * open. This is because the permission attributes were not available
 * beforehand.
 *
 * @param[in] obj_hdl               File to open or parent directory
 * @param[in,out] state             state_t to use for this operation
 * @param[in] openflags             Mode for open
 * @param[in] createmode            Mode for create
 * @param[in] name                  Name for file if being created or opened
 * @param[in] attrib_set            Attributes to set on created file
 * @param[in] verifier              Verifier to use for exclusive create
 * @param[in,out] new_obj           Newly created object
 * @param[in,out] caller_perm_check The caller must do a permission check
 *
 * @return FSAL status.
 */

fsal_status_t vfs_open2(struct fsal_obj_handle *obj_hdl,
			struct state_t *state,
			fsal_openflags_t openflags,
			enum fsal_create_mode createmode,
			const char *name,
			struct attrlist *attrib_set,
			fsal_verifier_t verifier,
			struct fsal_obj_handle **new_obj,
			bool *caller_perm_check)
{
	int posix_flags = 0;
	int fd, dir_fd;
	int retval = 0;
	mode_t unix_mode;
	fsal_status_t status = {0, 0};
	struct vfs_fd *my_fd = (struct vfs_fd *)(state + 1);
	struct vfs_fsal_obj_handle *myself, *hdl;
	struct stat stat;
	vfs_file_handle_t *fh = NULL;
	bool truncated = false;
	bool setattrs = attrib_set != NULL;
	bool created = false;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	fsal2posix_openflags(openflags, &posix_flags);

	if (createmode != FSAL_NO_CREATE && setattrs) {
		/* We have a create, check for size == 0 */
		if (FSAL_TEST_MASK(attrib_set->mask, ATTR_SIZE) &&
		    attrib_set->filesize == 0) {
			/* Handle truncate to zero on open */
			posix_flags |= O_TRUNC;
			truncated = true;
			/* Don't set the size if we later set the attributes */
			FSAL_UNSET_MASK(attrib_set->mask, ATTR_SIZE);
		}
	}

	if (name == NULL) {
		/* This is an open by handle */
		struct vfs_fsal_obj_handle *myself;

		myself  = container_of(obj_hdl,
				       struct vfs_fsal_obj_handle,
				       obj_handle);

		if (obj_hdl->fsal != obj_hdl->fs->fsal) {
			LogDebug(COMPONENT_FSAL,
				 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
				 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
			return fsalstat(posix2fsal_error(EXDEV), EXDEV);
		}

		status = vfs_open_my_fd(myself, openflags, posix_flags, my_fd);

		if (FSAL_IS_ERROR(status))
			return status;

		if (createmode >= FSAL_EXCLUSIVE || truncated) {
			/* Refresh the attributes */
			struct stat stat;
			attrmask_t request_mask;

			retval = fstat(my_fd->fd, &stat);

			if (retval == 0) {
				request_mask = myself->attributes.mask;
				posix2fsal_attributes(&stat,
						      &myself->attributes);
				myself->attributes.fsid = obj_hdl->fs->fsid;
				if (myself->sub_ops &&
				    myself->sub_ops->getattrs) {
					status = myself->sub_ops->getattrs(
							myself, my_fd->fd,
							request_mask);
					if (FSAL_IS_ERROR(status)) {
						FSAL_CLEAR_MASK(
						    myself->attributes.mask);
						FSAL_SET_MASK(
						    myself->attributes.mask,
						    ATTR_RDATTR_ERR);
						/** @todo: should handle this
						 * better.
						 */
					}
				}
			} else {
				if (errno == EBADF)
					errno = ESTALE;
				status = fsalstat(posix2fsal_error(errno),
						  errno);
			}

			/* Now check verifier for exclusive */
			if (!FSAL_IS_ERROR(status) &&
			    createmode >= FSAL_EXCLUSIVE &&
			    !obj_hdl->obj_ops.check_verifier(obj_hdl,
							     verifier)) {
				/* Verifier didn't match, return EEXIST */
				status =
				    fsalstat(posix2fsal_error(EEXIST), EEXIST);
			}
		}

		if (FSAL_IS_ERROR(status))
			(void) vfs_close_my_fd(my_fd);

		return status;
	}

	/* Now add in O_CREAT and O_EXCL.
	 * Even with FSAL_UNGUARDED we try exclusive create first so
	 * we can safely set attributes.
	 */
	if (createmode != FSAL_NO_CREATE) {
		posix_flags |= O_CREAT;

		if (createmode >= FSAL_GUARDED || setattrs)
			posix_flags |= O_EXCL;
	}

	if (setattrs && FSAL_TEST_MASK(attrib_set->mask, ATTR_MODE)) {
		unix_mode = fsal2unix_mode(attrib_set->mode) &
		    ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);
		/* Don't set the mode if we later set the attributes */
		FSAL_UNSET_MASK(attrib_set->mask, ATTR_MODE);
	} else {
		/* Default to mode 0600 */
		unix_mode = 0600;
	}

	dir_fd = vfs_fsal_open(myself, O_PATH | O_NOACCESS, &status.major);

	if (dir_fd < 0)
		return fsalstat(status.major, -dir_fd);

	/** @todo: not sure what this accomplishes... */
	retval = vfs_stat_by_handle(dir_fd, myself->handle, &stat,
				    O_PATH | O_NOACCESS);

	if (retval < 0) {
		retval = errno;
		goto direrr;
	}

	/* Become the user because we are creating an object in this dir.
	 */
	fsal_set_credentials(op_ctx->creds);

	if ((posix_flags & O_CREAT) != 0)
		fd = openat(dir_fd, name, posix_flags, unix_mode);
	else
		fd = openat(dir_fd, name, posix_flags);

	if (fd == -1 && errno == EEXIST && createmode == FSAL_UNCHECKED) {
		/* We tried to create O_EXCL to set attributes and failed.
		 * Remove O_EXCL and retry, also remember not to set attributes.
		 * We still try O_CREAT again just in case file disappears out
		 * from under us.
		 */
		posix_flags &= ~O_EXCL;
		setattrs = false;
		fd = openat(dir_fd, name, posix_flags, unix_mode);
	}

	if (fd < 0) {
		retval = errno;
		fsal_restore_ganesha_credentials();
		goto direrr;
	}

	/* Remember if we were responsible for creating the file.
	 * Note that in an UNCHECKED retry we MIGHT have re-created the
	 * file and won't remember that. Oh well, so in that rare case we
	 * leak a partially created file if we have a subsequent error in here.
	 * Also notify caller to do permission check if we DID NOT create the
	 * file. Note it IS possible in the case of a race between an UNCHECKED
	 * open and an external unlink, we did create the file, but we will
	 * still force a permission check. Of course that permission check
	 * SHOULD succeed since we also won't set the mode the caller requested
	 * and the default file create permissions SHOULD allow the owner
	 * read/write.
	 */
	created = (posix_flags & O_EXCL) != 0;
	*caller_perm_check = !created;

	fsal_restore_ganesha_credentials();

	vfs_alloc_handle(fh);

	retval = vfs_name_to_handle(dir_fd, obj_hdl->fs, name, fh);

	if (retval < 0) {
		retval = errno;
		goto fileerr;
	}

	retval = fstat(fd, &stat);

	if (retval < 0) {
		retval = errno;
		goto fileerr;
	}

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(dir_fd, fh, obj_hdl->fs, &stat, myself->handle, name,
			   op_ctx->fsal_export);

	if (hdl == NULL) {
		retval = ENOMEM;
		goto fileerr;
	}

	*new_obj = &hdl->obj_handle;

	close(dir_fd);

	my_fd->fd = fd;
	my_fd->openflags = openflags;

	if (setattrs && attrib_set->mask != 0) {
		/* Set attributes using our newly opened file descriptor as the
		 * share_fd if there are any left to set (mode and truncate
		 * have already been handled).
		 *
		 * Note that we only set the attributes if we were responsible
		 * for creating the file.
		 */
		status = (*new_obj)->obj_ops.setattr2(*new_obj,
						      state,
						      attrib_set);
	} else {
		status = fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	return status;

 fileerr:

	close(fd);

	/* Delete the file if we actually created it. */
	if (created)
		unlinkat(dir_fd, name, 0);

 direrr:

	close(dir_fd);
	status.major = posix2fsal_error(retval);
	return fsalstat(status.major, retval);
}

/**
 * @brief Re-open a file that may be already opened
 *
 * This function supports changing the access mode of a share reservation and
 * thus should only be called with a share state.
 *
 * @param[in] obj_hdl     File on which to operate
 * @param[in] state       state_t to use for this operation
 * @param[in] openflags   Mode for re-open
 *
 * @return FSAL status.
 */

fsal_status_t vfs_reopen2(struct fsal_obj_handle *obj_hdl,
			  struct state_t *state,
			  fsal_openflags_t openflags)
{
	struct vfs_fd fd, *my_fd = &fd, *my_share_fd;
	struct vfs_fsal_obj_handle *myself;
	fsal_status_t status = {0, 0};
	int posix_flags = 0;

	fsal2posix_openflags(openflags, &posix_flags);

	memset(my_fd, 0, sizeof(*my_fd));

	myself  = container_of(obj_hdl,
			       struct vfs_fsal_obj_handle,
			       obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	status = vfs_open_my_fd(myself, openflags, posix_flags, my_fd);

	if (!FSAL_IS_ERROR(status)) {
		/* Close the existing file descriptor and copy the new
		 * one over.
		 */
		my_share_fd = (struct vfs_fd *)(state + 1);
		vfs_close_my_fd(my_share_fd);
		*my_share_fd = fd;
	}

	return status;
}

fsal_status_t find_fd(struct vfs_fd **fd,
		      struct fsal_obj_handle *obj_hdl,
		      struct state_t *state,
		      fsal_openflags_t openflags,
		      bool *has_lock,
		      bool *need_fsync)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd = NULL;
	fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};

	/* Use the global file descriptor. */
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (state == NULL)
		goto global;

	/* State was valid, check it's fd */
	my_fd = (struct vfs_fd *)(state + 1);

	if ((my_fd->openflags & openflags & FSAL_O_RDWR)
	    == (openflags & FSAL_O_RDWR)) {
		/* It was valid, return it. */
		*fd = my_fd;
		*need_fsync = (openflags & FSAL_O_SYNC) != 0;
		return status;
	}

	if ((state->state_type == STATE_TYPE_LOCK ||
	     state->state_type == STATE_TYPE_NLM_LOCK) &&
	    state->state_data.lock.openstate != NULL) {
		my_fd = (struct vfs_fd *)(state->state_data.lock.openstate + 1);

		if ((my_fd->openflags & openflags & FSAL_O_RDWR)
		    == (openflags & FSAL_O_RDWR)) {
			/* It was valid, return it. */
			*fd = my_fd;
			 *need_fsync = (openflags & FSAL_O_SYNC) != 0;
			return status;
		}
	}

 global:

	/* No useable state_t so return the global file descriptor. */
	*fd = &myself->u.file;

	/* We will take the object handle lock in vfs_reopen_obj.
	 * And we won't have to fsync.
	 */
	*has_lock = true;
	*need_fsync = false;

	/* Make sure global is open as necessary. */
	return vfs_reopen_obj(obj_hdl, openflags);
}

/**
 * @brief Read data from a file
 *
 * This function reads data from the given file.
 *
 * @param[in]     obj_hdl     File on which to operate
 * @param[in]     state       state_t to use for this operation
 * @param[in]     offset      Position from which to read
 * @param[in]     buffer_size Amount of data to read
 * @param[out]    buffer      Buffer to which data are to be copied
 * @param[out]    read_amount Amount of data read
 * @param[out]    end_of_file true if the end of file has been reached
 * @param[in,out] info        more information about the data
 *
 * @return FSAL status.
 */
fsal_status_t vfs_read2(struct fsal_obj_handle *obj_hdl,
			struct state_t *state,
			uint64_t offset,
			size_t buffer_size,
			void *buffer,
			size_t *read_amount,
			bool *end_of_file,
			struct io_info *info)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd = NULL;
	ssize_t nb_read;
	fsal_status_t status;
	int retval = 0;
	bool has_lock = false;
	bool need_fsync = false;

	if (info != NULL) {
		/* Currently we don't support READ_PLUS */
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	/* Get a usable file descriptor */
	status = find_fd(&my_fd, obj_hdl, state, FSAL_O_READ,
			 &has_lock, &need_fsync);

	if (FSAL_IS_ERROR(status))
		goto out;

	nb_read = pread(my_fd->fd, buffer, buffer_size, offset);

	if (offset == -1 || nb_read == -1) {
		retval = errno;
		status = fsalstat(posix2fsal_error(retval), retval);
		goto out;
	}

	*read_amount = nb_read;

	/* dual eof condition */
	*end_of_file = ((nb_read == 0) /* most clients */ ||	/* ESXi */
			(((offset + nb_read) >= myself->attributes.filesize)));

#if 0
	/** @todo
	 *
	 * Is this all we really need to do to support READ_PLUS? Will anyone
	 * ever get upset that we don't return holes, even for blocks of all
	 * zeroes?
	 *
	 */
	if (info != NULL) {
		info->io_content.what = NFS4_CONTENT_DATA;
		info->io_content.data.d_offset = offset + nb_read;
		info->io_content.data.d_data.data_len = nb_read;
		info->io_content.data.d_data.data_val = buffer;
	}
#endif

 out:

	if (has_lock)
		PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return status;
}

/**
 * @brief Write data to a file
 *
 * This function writes data to a file.
 *
 * @param[in]     obj_hdl        File on which to operate
 * @param[in]     state          state_t to use for this operation
 * @param[in]     offset         Position at which to write
 * @param[in]     buffer         Data to be written
 * @param[in,out] fsal_stable    In, if on, the fsal is requested to write data
 *                               to stable store. Out, the fsal reports what
 *                               it did.
 * @param[in,out] info           more information about the data
 *
 * @return FSAL status.
 */
fsal_status_t vfs_write2(struct fsal_obj_handle *obj_hdl,
			 struct state_t *state,
			 uint64_t offset,
			 size_t buffer_size,
			 void *buffer,
			 size_t *wrote_amount,
			 bool *fsal_stable,
			 struct io_info *info)
{
	struct vfs_fd *my_fd;
	ssize_t nb_written;
	fsal_status_t status;
	int retval = 0;
	bool has_lock = false;
	bool need_fsync = false;
	fsal_openflags_t openflags = FSAL_O_WRITE;

	if (info != NULL) {
		/* Currently we don't support WRITE_PLUS */
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	if (*fsal_stable)
		openflags |= FSAL_O_SYNC;

	/* Get a usable file descriptor */
	status = find_fd(&my_fd, obj_hdl, state, openflags,
			 &has_lock, &need_fsync);

	if (FSAL_IS_ERROR(status))
		goto out;

	fsal_set_credentials(op_ctx->creds);

	nb_written = pwrite(my_fd->fd, buffer, buffer_size, offset);

	if (nb_written == -1) {
		retval = errno;
		status = fsalstat(posix2fsal_error(retval), retval);
		goto out;
	}

	*wrote_amount = nb_written;

	/* attempt stability if we aren't using an O_SYNC fd */
	if (need_fsync) {
		retval = fsync(my_fd->fd);
		if (retval == -1) {
			retval = errno;
			status = fsalstat(posix2fsal_error(retval), retval);
		}
	}

 out:

	if (has_lock)
		PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	fsal_restore_ganesha_credentials();
	return status;
}

/**
 * @brief Commit written data
 *
 * This function flushes possibly buffered data to a file.
 *
 * @param[in] obj_hdl          File on which to operate
 * @param[in] state            state_t to use for this operation
 * @param[in] offset           Start of range to commit
 * @param[in] len              Length of range to commit
 *
 * @return FSAL status.
 */

fsal_status_t vfs_commit2(struct fsal_obj_handle *obj_hdl,
			  off_t offset,
			  size_t len)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd;
	fsal_status_t status;
	int retval;

	/* Use the global file descriptor. */
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	my_fd = &myself->u.file;

	/* Make sure file is open in appropriate mode. Returns with the
	 * obj_hdl->lock held.
	 */
	status = vfs_reopen_obj(obj_hdl, FSAL_O_WRITE);

	if (!FSAL_IS_ERROR(status)) {

		fsal_set_credentials(op_ctx->creds);

		retval = fsync(my_fd->fd);

		if (retval == -1) {
			retval = errno;
			status = fsalstat(posix2fsal_error(retval), retval);
		}

		fsal_restore_ganesha_credentials();
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return status;
}

#ifdef F_OFD_GETLK
/**
 * @brief Perform a lock operation
 *
 * This function performs a lock operation (lock, unlock, test) on a
 * file.
 *
 * For FSAL_VFS etc. we ignore owner, implicitly we have a lock_fd per
 * lock owner.
 *
 * @param[in]  obj_hdl          File on which to operate
 * @param[in]  state            state_t to use for this operation
 * @param[in]  owner            Lock owner
 * @param[in]  lock_op          Operation to perform
 * @param[in]  request_lock     Lock to take/release/test
 * @param[out] conflicting_lock Conflicting lock
 *
 * @return FSAL status.
 */
fsal_status_t vfs_lock_op2(struct fsal_obj_handle *obj_hdl,
			   struct state_t *state,
			   void *owner,
			   fsal_lock_op_t lock_op,
			   fsal_lock_param_t *request_lock,
			   fsal_lock_param_t *conflicting_lock)
{
	struct vfs_fsal_obj_handle *myself;
	struct vfs_fd *my_fd = (struct vfs_fd *)(state + 1);
	struct flock lock_args;
	int fcntl_comm;
	fsal_status_t status = {0, 0};
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

	LogFullDebug(COMPONENT_FSAL,
		     "Locking: op:%d type:%d start:%" PRIu64 " length:%lu ",
		     lock_op, request_lock->lock_type, request_lock->lock_start,
		     request_lock->lock_length);

	if (lock_op == FSAL_OP_LOCKT) {
		fcntl_comm = F_OFD_GETLK;
	} else if (lock_op == FSAL_OP_LOCK || lock_op == FSAL_OP_UNLOCK) {
		fcntl_comm = F_OFD_SETLK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: Lock operation requested was not TEST, READ, or WRITE.");
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (request_lock->lock_type == FSAL_LOCK_R) {
		lock_args.l_type = F_RDLCK;
	} else if (request_lock->lock_type == FSAL_LOCK_W) {
		lock_args.l_type = F_WRLCK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: The requested lock type was not read or write.");
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (lock_op == FSAL_OP_UNLOCK)
		lock_args.l_type = F_UNLCK;

	lock_args.l_len = request_lock->lock_length;
	lock_args.l_start = request_lock->lock_start;
	lock_args.l_whence = SEEK_SET;

	/* flock.l_len being signed long integer, larger lock ranges may
	 * get mapped to negative values. As per 'man 3 fcntl', posix
	 * locks can accept negative l_len values which may lead to
	 * unlocking an unintended range. Better bail out to prevent that.
	 */
	if (lock_args.l_len < 0) {
		LogCrit(COMPONENT_FSAL,
			"The requested lock length is out of range- lock_args.l_len(%ld), request_lock_length(%lu)",
			lock_args.l_len, request_lock->lock_length);
		return fsalstat(ERR_FSAL_BAD_RANGE, 0);
	}

	if (my_fd->fd < 0 || my_fd->openflags == FSAL_O_CLOSED) {
		LogDebug(COMPONENT_FSAL,
			 "Attempting to open file descriptor open");

		status = vfs_open_my_fd(myself, FSAL_O_RDWR, O_RDWR, my_fd);

		if (FSAL_IS_ERROR(status)) {
			LogCrit(COMPONENT_FSAL,
				"Open for locking failed");
			return status;
		}
	}

	errno = 0;
	retval = fcntl(my_fd->fd, fcntl_comm, &lock_args);

	if (retval && lock_op == FSAL_OP_LOCK) {
		retval = errno;

		if (conflicting_lock != NULL) {
			/* Get the conflicting lock */
			retval = fcntl(my_fd->fd, F_GETLK, &lock_args);

			if (retval) {
				retval = errno;	/* we lose the initial error */
				LogCrit(COMPONENT_FSAL,
					"After failing a lock request, I couldn't even get the details of who owns the lock.");
				goto err;
			}

			if (conflicting_lock != NULL) {
				conflicting_lock->lock_length = lock_args.l_len;
				conflicting_lock->lock_start =
				    lock_args.l_start;
				conflicting_lock->lock_type = lock_args.l_type;
			}
		}

		goto err;
	}

	/* F_UNLCK is returned then the tested operation would be possible. */
	if (conflicting_lock != NULL) {
		if (lock_op == FSAL_OP_LOCKT && lock_args.l_type != F_UNLCK) {
			conflicting_lock->lock_length = lock_args.l_len;
			conflicting_lock->lock_start = lock_args.l_start;
			conflicting_lock->lock_type = lock_args.l_type;
		} else {
			conflicting_lock->lock_length = 0;
			conflicting_lock->lock_start = 0;
			conflicting_lock->lock_type = FSAL_NO_LOCK;
		}
	}

	/* Fall through (retval == 0) */

 err:

	return fsalstat(posix2fsal_error(retval), retval);
}
#endif

/**
 * @brief Set attributes on an object
 *
 * This function sets attributes on an object.  Which attributes are
 * set is determined by @c attrib_set->mask.
 *
 * @param[in] obj_hdl    File on which to operate
 * @param[in] state      state_t to use for this operation
 * @param[in] attrib_set Attributes to set
 *
 * @return FSAL status.
 */
fsal_status_t vfs_setattr2(struct fsal_obj_handle *obj_hdl,
			   struct state_t *state,
			   struct attrlist *attrib_set)
{
	struct vfs_fsal_obj_handle *myself;
	struct closefd cfd = { .fd = -1, .close_fd = false };
	struct stat stat;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	fsal_status_t status = {0, 0};
	int retval = 0;
	fsal_openflags_t open_flags = FSAL_O_ANY;
	bool has_lock = false;

	/* apply umask, if mode attribute is to be changed */
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_MODE))
		attrib_set->mode &=
		    ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name,
			 obj_hdl->fs->fsal != NULL
				? obj_hdl->fs->fsal->name
				: "(none)");
		return fsalstat(posix2fsal_error(EXDEV), EXDEV);
	}

#ifdef ENABLE_RFC_ACL
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_MODE) &&
	    !FSAL_TEST_MASK(attrib_set->mask, ATTR_ACL)) {
		/* Set ACL from MODE */
		status = fsal_mode_to_acl(attrib_set, myself->attributes.acl);
	} else {
		/* If ATTR_ACL is set, mode needs to be adjusted no matter what.
		 * See 7530 s 6.4.1.3 */
		if (!FSAL_TEST_MASK(attrib_set->mask, ATTR_MODE))
			attrib_set->mode = myself->attributes.mode;
		status = fsal_acl_to_mode(attrib_set);
	}

	if (FSAL_IS_ERROR(status))
		return status;
#endif /* ENABLE_RFC_ACL */


	/* This is yet another "you can't get there from here".  If this object
	 * is a socket (AF_UNIX), an fd on the socket s useless _period_.
	 * If it is for a symlink, without O_PATH, you will get an ELOOP error
	 * and (f)chmod doesn't work for a symlink anyway - not that it matters
	 * because access checking is not done on the symlink but the final
	 * target.
	 * AF_UNIX sockets are also ozone material.  If the socket is already
	 * active listeners et al, you can manipulate the mode etc.  If it is
	 * just sitting there as in you made it with a mknod.
	 * (one of those leaky abstractions...)
	 * or the listener forgot to unlink it, it is lame duck.
	 */

	/* Test if size is being set, make sure file is regular and if so,
	 * require a read/write file descriptor.
	 */
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_SIZE)) {
		if (obj_hdl->type != REGULAR_FILE)
			return fsalstat(ERR_FSAL_INVAL, EINVAL);
		open_flags = FSAL_O_RDWR;
	}

#if 0
	/** @todo find usable fd */
	if (share_fd != NULL &&
	    (share_fd->openflags & FSAL_O_RDWR) == FSAL_O_RDWR) {
		/* Use the share reservation's fd */
		struct vfs_fd *my_fd = container_of(share_fd,
						    struct vfs_fd,
						    fsal_fd);
		cfd.fd = my_fd->fd;
	} else if (lock_fd != NULL &&
		   (lock_fd->openflags & FSAL_O_RDWR) == FSAL_O_RDWR) {
		struct vfs_fd *my_fd = container_of(lock_fd,
						    struct vfs_fd,
						    fsal_fd);
		cfd.fd = my_fd->fd;
	} else {
#endif
	{
		/* Take read lock on object to protect file descriptor.
		 * We only take a read lock because we are not changing the
		 * state of the file descriptor. If the file is not open for
		 * read (or read/write in the case of setting size) we will
		 * use a temporary file descriptor.
		 */
		PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);
		has_lock = true;

		cfd = vfs_fsal_open_and_stat(op_ctx->fsal_export, myself, &stat,
					     open_flags, &fsal_error);
	}

	if (cfd.fd < 0) {
		if (obj_hdl->type == SYMBOLIC_LINK &&
		    cfd.fd == -EPERM) {
			/* You cannot open_by_handle (XFS) a symlink and it
			 * throws an EPERM error for it.  open_by_handle_at
			 * does not throw that error for symlinks so we play a
			 * game here.  Since there is not much we can do with
			 * symlinks anyway, say that we did it
			 * but don't actually do anything.
			 * If you *really* want to tweek things
			 * like owners, get a modern linux kernel...
			 */
			status = fsalstat(ERR_FSAL_NO_ERROR, 0);
		} else {
			status = fsalstat(fsal_error, -cfd.fd);
		}
		goto out_unlock;
	}

	/** TRUNCATE **/
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_SIZE)) {
		retval = ftruncate(cfd.fd, attrib_set->filesize);
		if (retval != 0) {
			/** @todo FSF: is this still necessary?
			 *
			 * XXX ESXi volume creation pattern reliably
			 * reached this point in the past, however now that we
			 * only use the already open file descriptor if it is
			 * open read/write, this may no longer fail.
			 * If there is some other error from ftruncate, then
			 * we will needlessly retry, but without more detail
			 * of the original failure, we can't be sure.
			 * Fortunately permission checking is done by
			 * Ganesha before calling here, so we won't get an
			 * EACCES since this call is done as root. We could
			 * get EFBIG, EPERM, or EINVAL.
			 */
			if (cfd.close_fd)
				close(cfd.fd);

			cfd = vfs_fsal_open_and_stat(op_ctx->fsal_export,
						     myself, &stat,
						     open_flags | FSAL_O_REOPEN,
						     &fsal_error);

			if (cfd.fd < 0) {
				status = fsalstat(fsal_error, -cfd.fd);
				goto out_unlock;
			}

			retval = ftruncate(cfd.fd, attrib_set->filesize);
			if (retval != 0)
				goto fileerr;
		}
	}

	/** CHMOD **/
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_MODE)) {
		/* The POSIX chmod call doesn't affect the symlink object, but
		 * the entry it points to. So we must ignore it.
		 */
		if (!S_ISLNK(stat.st_mode)) {
			if (vfs_unopenable_type(obj_hdl->type))
				retval = fchmodat(
					cfd.fd,
					myself->u.unopenable.name,
					fsal2unix_mode(attrib_set->mode),
					0);
			else
				retval = fchmod(
					cfd.fd,
					fsal2unix_mode(attrib_set->mode));

			if (retval != 0)
				goto fileerr;
		}
	}

	/**  CHOWN  **/
	if (FSAL_TEST_MASK(attrib_set->mask, ATTR_OWNER | ATTR_GROUP)) {
		uid_t user = FSAL_TEST_MASK(attrib_set->mask, ATTR_OWNER)
		    ? (int)attrib_set->owner : -1;
		gid_t group = FSAL_TEST_MASK(attrib_set->mask, ATTR_GROUP)
		    ? (int)attrib_set->group : -1;

		if (vfs_unopenable_type(obj_hdl->type))
			retval = fchownat(cfd.fd, myself->u.unopenable.name,
					  user, group, AT_SYMLINK_NOFOLLOW);
		else if (obj_hdl->type == SYMBOLIC_LINK)
			retval = fchownat(cfd.fd, "", user, group,
					  AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
		else
			retval = fchown(cfd.fd, user, group);

		if (retval)
			goto fileerr;
	}

	/**  UTIME  **/
	if (FSAL_TEST_MASK(attrib_set->mask, ATTRS_SET_TIME)) {
		struct timespec timebuf[2];

		if (obj_hdl->type == SYMBOLIC_LINK)
			goto out; /* Setting time on symlinks is illegal */
		/* Atime */
		if (FSAL_TEST_MASK(attrib_set->mask, ATTR_ATIME_SERVER)) {
			timebuf[0].tv_sec = 0;
			timebuf[0].tv_nsec = UTIME_NOW;
		} else if (FSAL_TEST_MASK(attrib_set->mask, ATTR_ATIME)) {
			timebuf[0] = attrib_set->atime;
		} else {
			timebuf[0].tv_sec = 0;
			timebuf[0].tv_nsec = UTIME_OMIT;
		}

		/* Mtime */
		if (FSAL_TEST_MASK(attrib_set->mask, ATTR_MTIME_SERVER)) {
			timebuf[1].tv_sec = 0;
			timebuf[1].tv_nsec = UTIME_NOW;
		} else if (FSAL_TEST_MASK(attrib_set->mask, ATTR_MTIME)) {
			timebuf[1] = attrib_set->mtime;
		} else {
			timebuf[1].tv_sec = 0;
			timebuf[1].tv_nsec = UTIME_OMIT;
		}
		if (vfs_unopenable_type(obj_hdl->type))
			retval = vfs_utimesat(cfd.fd, myself->u.unopenable.name,
					      timebuf, AT_SYMLINK_NOFOLLOW);
		else
			retval = vfs_utimes(cfd.fd, timebuf);
		if (retval != 0)
			goto fileerr;
	}

	/** SUBFSAL **/
	if (myself->sub_ops && myself->sub_ops->setattrs) {
		status = myself->sub_ops->setattrs(
					myself,
					cfd.fd,
					attrib_set->mask, attrib_set);
		if (FSAL_IS_ERROR(status))
			goto out;
	}

	errno = 0;

 fileerr:

	retval = errno;
	status = fsalstat(posix2fsal_error(retval), retval);

out:
	if (cfd.close_fd)
		close(cfd.fd);

 out_unlock:

	if (has_lock)
		PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return status;
}

/**
 * @brief Close a file
 *
 * This function closes a file.  It is protected by the Cache inode
 * content lock.
 *
 * @param[in] state      state_t to use for this operation
 *
 * @return FSAL status.
 */

fsal_status_t vfs_close2(struct state_t *state)
{
	struct vfs_fd *my_fd = (struct vfs_fd *)(state + 1);

	return vfs_close_my_fd(my_fd);
}
