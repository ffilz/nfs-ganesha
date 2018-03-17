#include <errno.h>
#include <string.h>

#include "nfs4_fs_locations.h"
#include "fsal_types.h"
#include "common_utils.h"

fsal_fs_locations_t *nfs4_fs_locations_alloc()
{
	fsal_fs_locations_t *fs_locations;

	fs_locations = gsh_calloc(1, sizeof(fsal_fs_locations_t));
	if (pthread_rwlock_init(&(fs_locations->lock), NULL) != 0) {
		nfs4_fs_locations_free(fs_locations);
		LogCrit(COMPONENT_NFS_V4,
			"New fs locations RW lock init returned %d (%s)", errno,
			strerror(errno));
		return NULL;
	}

	return fs_locations;
}

void nfs4_fs_locations_free(fsal_fs_locations_t *fs_locations)
{
	if (!fs_locations)
		return;

	if (fs_locations->path)
		gsh_free(fs_locations->path);

	if (fs_locations->locations)
		gsh_free(fs_locations->locations);

	gsh_free(fs_locations);
}

void nfs4_fs_locations_get_ref(fsal_fs_locations_t *fs_locations)
{
	PTHREAD_RWLOCK_wrlock(&fs_locations->lock);
	fs_locations->ref++;
	LogFullDebug(COMPONENT_NFS_V4, "(fs_locations, ref) = (%p, %u)",
		     fs_locations, fs_locations->ref);
	PTHREAD_RWLOCK_unlock(&fs_locations->lock);
}

/* Must be called with lock held */
static void nfs4_fs_locations_put_ref(fsal_fs_locations_t *fs_locations)
{
	fs_locations->ref--;
	LogFullDebug(COMPONENT_NFS_V4, "(fs_locations, ref) = (%p, %u)",
		     fs_locations, fs_locations->ref);
}

void nfs4_fs_locations_release(fsal_fs_locations_t *fs_locations)
{
	if (fs_locations == NULL)
		return;

	PTHREAD_RWLOCK_wrlock(&fs_locations->lock);
	if (fs_locations->ref > 1) {
		nfs4_fs_locations_put_ref(fs_locations);
		PTHREAD_RWLOCK_unlock(&fs_locations->lock);
		return;
	} else {
		LogFullDebug(COMPONENT_NFS_V4, "Free fs_locations: %p",
			     fs_locations);
	}

	PTHREAD_RWLOCK_unlock(&fs_locations->lock);

	// Releasing fs_locations
	nfs4_fs_locations_free(fs_locations);
}

fsal_fs_locations_t *nfs4_fs_locations_new(const char *path,
					   const char *locations)
{
	fsal_fs_locations_t *fs_locations;

	fs_locations = nfs4_fs_locations_alloc();
	if (fs_locations == NULL) {
		LogCrit(COMPONENT_NFS_V4, "Could not allocate fs_locations");
		return NULL;
	}

	fs_locations->path = gsh_strdup(path);
	fs_locations->locations = gsh_strdup(locations);
	fs_locations->ref = 1;

	return fs_locations;
}
