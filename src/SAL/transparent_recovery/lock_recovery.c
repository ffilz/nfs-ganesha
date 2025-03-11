#include "transparent_recovery.h"

char *lock_attribute =
	"LOCK"; /* attribute to be concatenated with key to form lock key */
uint64_t unique_serial_no = 0; /* serial no. to be used as primary key */
static int num_documents =
	0; /* file local varaible to store number of documents */
static int num_fields = 0; /* file local variable to store number of fields */
static struct lock_info **lock_obj =
	NULL; /* file local variable to store the locks */

/**
 * Function to persist the lock information in the redis instance.
 * Given the bucket, lock information, state owner and file handle 
 * persists the lock information in the redis instance.
 */
void persist_lock_info(const char *bucket, fsal_lock_param_t *lock,
		       state_owner_t *owner, struct fsal_obj_handle *sle_obj)
{
	/* Local variable for casting & store lock information
	 as form of document in the redis instance */

	int num_fields;
	char lock_bucket[MAX_LEN];
	char primary_key[MAX_LEN];
	char rhdlstr[MAX_LEN];
	char lock_type_str[MAX_LEN];
	char lock_start_str[MAX_LEN];
	char lock_length_str[MAX_LEN];
	char fsid_maj_str[MAX_LEN];
	char fsid_min_str[MAX_LEN];
	char fileid_str[MAX_LEN];

	/* Preparing the docuement fields for setting the lock information */

	sprintf(lock_bucket, "%s-%s", bucket, lock_attribute);
	base64url_encode(owner->so_owner_val, owner->so_owner_len, rhdlstr,
			 sizeof(rhdlstr));
	sprintf(lock_type_str, "%d", lock->lock_type);
	sprintf(lock_start_str, "%lu", lock->lock_start);
	sprintf(lock_length_str, "%lu", lock->lock_length);
	sprintf(fsid_maj_str, "%lu", sle_obj->fsid.major);
	sprintf(fsid_min_str, "%lu", sle_obj->fsid.minor);
	sprintf(fileid_str, "%lu", sle_obj->fileid);
	sprintf(primary_key, "%lu", unique_serial_no++);

	/* Creating the document for setting the lock information in the
	 given bucket in the redis instance */

	char *document[] = { "lock_owner",  rhdlstr,	     "lock_type",
			     lock_type_str, "lock_start",    lock_start_str,
			     "lock_length", lock_length_str, "fsid_major",
			     fsid_maj_str,  "fsid_minor",    fsid_min_str,
			     "fileid",	    fileid_str };

	num_fields = 7;
	set_document(lock_bucket, primary_key, document,
		     num_fields); /* setting the document */

	LogAlways(COMPONENT_ALL, "Successfully persisted lock info for key %s",
		  primary_key);
}

/**
 * Function to fetch the lock information from the redis instance.
 * Given the bucket, fetches all the locks information from the redis instance
 * in that bucket.
 */
void fetch_persisting_locks(const char *bucket)
{
	char ***document_list;
	char lock_bucket[256];
	num_documents = 0;
	num_fields = 0;

	/* Creating the bucket name for fetching the lock information */
	sprintf(lock_bucket, "%s-%s", bucket, lock_attribute);
	get_bucket(lock_bucket, &document_list, &num_documents,
		   &num_fields); /* fetching the lock information */
	lock_obj = malloc(sizeof(struct db_entry *) * num_documents);

	/* Setting the fetched data in the lock_obj */
	for (int i = 0; i < num_documents; i++) {
		lock_obj[i] = malloc(sizeof(struct lock_info));

		lock_obj[i]->lock_owner = document_list[i][1];
		lock_obj[i]->lock_type = atoi(document_list[i][3]);
		lock_obj[i]->lock_start = atoll(document_list[i][5]);
		lock_obj[i]->lock_length = atoll(document_list[i][7]);
		lock_obj[i]->fsid_major = atoi(document_list[i][9]);
		lock_obj[i]->fsid_minor = atoi(document_list[i][11]);
		lock_obj[i]->fileid = atoll(document_list[i][13]);
	}

	LogAlways(COMPONENT_ALL, "Successfully fetched %i locks",
		  num_documents);
}

/**
 * Function to check if the lock information is conflicting.
 * Given the lock information, state owner and file handle
 * checks if the lock information is conflicting or not.
 */
bool is_conflict_persisting_locks(fsal_lock_param_t *lock, state_owner_t *owner,
				  struct fsal_obj_handle *sle_obj)
{
	char rhdlstr[MAX_LEN];

	base64url_encode(owner->so_owner_val, owner->so_owner_len, rhdlstr,
			 sizeof(rhdlstr));

	/* Iterating through the list of lock objects */
	for (int i = 0; i < num_documents; i++) {
		uint64_t lock_end =
			lock_obj[i]->lock_start + lock_obj[i]->lock_length - 1;

		/* If file system & fileid doesn't match continue */
		if (lock_obj[i]->fsid_major != sle_obj->fsid.major ||
		    lock_obj[i]->fsid_minor != sle_obj->fsid.minor ||
		    lock_obj[i]->fileid != sle_obj->fileid)
			continue;

		/* If the requested lock range is overalapping */
		if (lock_end >= lock->lock_start &&
		    lock_obj[i]->lock_start <= lock->lock_start) {
			/* If the lock owner doesn't match */
			if ((lock_obj[i]->lock_type == FSAL_LOCK_W ||
			     lock->lock_type == FSAL_LOCK_W) &&
			    strcmp(lock_obj[i]->lock_owner, rhdlstr) != 0) {
				LogEvent(
					COMPONENT_ALL,
					"DB lock owner vs lock owner: %s and %s",
					lock_obj[i]->lock_owner, rhdlstr);
				return true;
			}
		}
	}

	return false;
}