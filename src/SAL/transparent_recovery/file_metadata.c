#include "transparent_recovery.h"

// store file metadata in the database
bool persist_file_md(char *bucket, uint32_t fsid_maj, uint32_t fsid_min,
		     struct file_metadata *file)
{
	char fileid[MAX_LEN], open_counter[MAX_LEN], lock_counter[MAX_LEN];
	char *primary_key, *open_prefix, *lock_prefix;
	bool status;

	file->open_counter = 0;
	file->lock_counter = 0;

	sprintf(fileid, "%lu", file->fileid);
	sprintf(open_counter, "%lu", file->open_counter);
	sprintf(lock_counter, "%lu", file->lock_counter);

	primary_key = malloc(MAX_LEN);
	open_prefix = malloc(MAX_LEN);
	lock_prefix = malloc(MAX_LEN);

	sprintf(primary_key, "file:%u:%u:%lu", fsid_maj, fsid_min,
		file->fileid);
	sprintf(open_prefix, "file:%u:%u:%lu:open", fsid_maj, fsid_min,
		file->fileid);
	sprintf(lock_prefix, "file:%u:%u:%lu:lock", fsid_maj, fsid_min,
		file->fileid);

	file->key = primary_key;
	file->open_key_prefix = open_prefix;
	file->lock_key_prefix = lock_prefix;

	char *document[] = { "key",
			     file->key,
			     "fileid",
			     fileid,
			     "open_key_prefix",
			     file->open_key_prefix,
			     "lock_key_prefix",
			     file->lock_key_prefix,
			     "open_counter",
			     open_counter,
			     "lock_counter",
			     lock_counter };
	status = true;
	if (set_document(bucket, file->key, document, 6) < 0) {
		status = false;
	}

	LogAlways(COMPONENT_MIGRATION, "persist file md");
	return status;
}

// fetch file metadata from the database
bool fetch_file_md(char *bucket, uint32_t fsid_maj, uint32_t fsid_min,
		   uint64_t fileid, struct file_metadata *file)
{
	char primary_key[MAX_LEN];
	char **document;
	int num_fields;

	sprintf(primary_key, "file:%u:%u:%lu", fsid_maj, fsid_min, fileid);

	if (get_document(bucket, primary_key, &document, &num_fields) < 0) {
		return false;
	}

	file = (struct file_metadata *)malloc(sizeof(struct file_metadata));

	file->key = document[1];
	file->fileid = atoll(document[3]);
	file->open_key_prefix = document[5];
	file->lock_key_prefix = document[7];
	file->open_counter = atoll(document[9]);
	file->lock_counter = atoll(document[11]);

	for (int i = 0; i < 2 * num_fields; i++) {
		free(document[i]);
	}
	free(document);
	LogAlways(COMPONENT_MIGRATION, "Fetched FILE MD");
	return true;
}