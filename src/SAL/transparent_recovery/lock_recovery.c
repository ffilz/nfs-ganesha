#include "transparent_recovery.h"

/**
 * Function to persist the lock information in the redis instance.
 * Given the bucket, lock information, state owner and file handle 
 * persists the lock information in the redis instance.
 */

bool persist_lock_info(const char *bucket, struct file_metadata *file,
		       struct lock_info *lock)
{
	char primary_key[MAX_LEN];
	char fileid[MAX_LEN];
	char type[MAX_LEN];
	char start[MAX_LEN];
	char length[MAX_LEN];

	LogAlways(COMPONENT_ALL, "Lock Persisting %s %s", bucket,
		  file->open_key_prefix);

	sprintf(primary_key, "%s:%lu", file->open_key_prefix,
		file->lock_counter++);
	sprintf(fileid, "%lu", lock->fileid);
	sprintf(type, "%u", lock->type);
	sprintf(start, "%lu", lock->start);
	sprintf(length, "%lu", lock->length);

	lock->key = primary_key;

	char *document[] = { "key",	  lock->key,	"fileid",
			     fileid,	  "lock_owner", lock->lock_owner,
			     "lock_type", type,		"start",
			     start,	  "length",	length };

	return set_document(bucket, lock->key, document, 6) == 0;
}

/**
 * Function to fetch the lock information from the redis instance.
 * Given the bucket, fetches all the locks information from the redis instance
 * in that bucket.
 */

bool fetch_persisting_locks(const char *bucket,
			    const struct file_metadata *file,
			    struct lock_info *locks)
{
	char ***documents;
	int num_document;

	if (get_document_with_prefix(bucket, file->lock_key_prefix, &documents,
				     &num_document) < 0)
		return false;

	locks = (struct lock_info *)malloc(sizeof(struct lock_info) *
					   num_document);

	for (int i = 0; i < num_document; i++) {
		locks[i].key = documents[i][1];
		locks[i].fileid = strtoul(documents[i][3], NULL, 20);
		locks[i].lock_owner = documents[i][5];
		locks[i].type = strtoul(documents[i][7], NULL, 20);
		locks[i].start = strtoul(documents[i][9], NULL, 20);
		locks[i].length = strtoul(documents[i][11], NULL, 20);
	}

	for (int i = 0; i < num_document; i++) {
		for (int j = 0; j < 12; j++) {
			free(documents[i][j]);
		}
		free(documents[i]);
	}
	free(documents);

	return true;
}

/**
 * Function to check if the lock information is conflicting.
 * Given the lock information, state owner and file handle
 * checks if the lock information is conflicting or not.
 */

bool is_conflicting_lock(struct lock_info *locks, struct lock_info *lock)
{
	uint64_t num_locks;
	uint64_t found_lock_end, lock_end = lock->start + lock->length - 1;
	num_locks = sizeof(*locks) / sizeof(struct lock_info);
	for (int i = 0; i < num_locks; i++) {
		found_lock_end = locks[i].start + locks[i].length - 1;

		if (locks[i].fileid != lock->fileid)
			continue;

		if (found_lock_end >= lock->start &&
		    locks[i].start <= lock_end) {
			if ((locks[i].type == FSAL_LOCK_W ||
			     lock->type == FSAL_LOCK_W) &&
			    strcmp(locks[i].lock_owner, lock->lock_owner) != 0)
				return true;
		}
	}

	return false;
}

/**
 * Function to delete the lock information from the redis instance.
 * Given the bucket, lock information, state owner and file handle
 * deletes the lock information from the redis instance.
 */

bool delete_lock_info(const char *bucket, struct file_metadata *file,
		      struct lock_info *lock)
{
	struct lock_info *locks;
	uint64_t found_lock_end, lock_end = lock->start + lock->length - 1;
	char new_key[MAX_LEN];
	locks = NULL;

	if (!fetch_persisting_locks(bucket, file, locks))
		return false;

	int num_locks = sizeof(*locks) / sizeof(struct lock_info);

	for (int i = 0; i < num_locks; i++) {
		found_lock_end = locks[i].start + locks[i].length - 1;

		if (locks[i].fileid != lock->fileid ||
		    strcmp(locks[i].lock_owner, lock->lock_owner) != 0)
			continue;

		if (!(found_lock_end >= lock->start &&
		      locks[i].start <= lock_end))
			continue;

		uint64_t old_start = locks[i].start;
		uint64_t old_end = found_lock_end;

		if (del_document(bucket, locks[i].key) < 0)
			return false;

		struct lock_info new_lock;
		new_lock = *lock;
		LogAlways(COMPONENT_ALL, "%lu", new_lock.fileid);
		if (old_start < lock->start) {
			new_lock.start = old_start;
			new_lock.length = lock->start - old_start + 1;
			sprintf(new_key, "%s:%lu", file->key,
				file->lock_counter++);
			if (!persist_lock_info(bucket, file, lock))
				return false;
		}

		if (lock_end < old_end) {
			new_lock.start = lock->start + lock->length;
			new_lock.length = old_end - lock_end + 1;
			sprintf(new_key, "%s:%lu", file->key,
				file->lock_counter++);
			if (!persist_lock_info(bucket, file, lock))
				return false;
		}
	}

	return true;
}