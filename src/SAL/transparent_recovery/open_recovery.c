#include "transparent_recovery.h"
#include "log.h"

/**
 * Persist the open info to the redis database.
 * Given the bucket, state owner, share access and deny values, persists the 
 * open info to the redis database.
 */

void persist_open_info(const char *bucket, const struct file_metadata *file,
		       const struct open_info *open)
{
	char fileid[MAX_LEN];
	char share_access[MAX_LEN];
	char share_deny[MAX_LEN];

	sprintf(fileid, "%lu", open->fileid);
	sprintf(share_access, "%u", open->share_access);
	sprintf(share_deny, "%u", open->share_deny);

	char *document[] = { "key",	     open->key,	   "fileid",
			     fileid,	     "open_owner", open->open_owner,
			     "share_access", share_access, "share_deny",
			     share_deny };

	set_document(bucket, open->key, document, 5);
}

/**
 * Fetch the open info from the redis database.
 * Given the bucket and state owner, fetches the open info from the redis database.
 */

void fetch_persisting_opens(const char *bucket,
			    const struct file_metadata *file,
			    struct open_info *opens)
{
	char ***documents;
	int num_document;

	get_document_with_prefix(bucket, file->open_key_prefix, &documents,
				 &num_document);
	opens = (struct open_info *)malloc(sizeof(struct open_info) *
					   num_document);

	for (int i = 0; i < num_document; i++) {
		opens[i].key = documents[i][1];
		opens[i].fileid = atoll(documents[i][3]);
		opens[i].open_owner = documents[i][5];
		opens[i].share_access = atoll(documents[i][7]);
		opens[i].share_deny = atoll(documents[i][9]);
	}
}

void delete_persisting_open(const char *bucket, const struct open_info *open)
{
	del_document(bucket, open->key);
}

/**
 * Checks if the new open request conflicts with the existing open request.
 * Given the existing access and deny values, new access and deny values,
 * returns true if the new open request conflicts with the existing open request.
 */

bool is_conflicting_open(const struct open_info *opens,
			 const struct open_info *open)
{
	int num_open = sizeof(*opens) / sizeof(struct open_info);

	for (int i = 0; i < num_open; i++) {
		if (opens[i].fileid != open->fileid)
			continue;

		if ((opens[i].share_deny & open->share_access) != 0 ||
		    (open->share_deny & opens[i].share_access) != 0)
			return true;
	}

	return false;
}