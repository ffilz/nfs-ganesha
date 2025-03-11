#include "transparent_recovery.h"
#include "log.h"

/**
 * Checks if the new open request conflicts with the existing open request.
 * Given the existing access and deny values, new access and deny values,
 * returns true if the new open request conflicts with the existing open request.
 */
bool is_conflict_persisting_opens(uint32_t existing_access,
				  uint32_t existing_deny, uint32_t new_access,
				  uint32_t new_deny)
{
	LogFullDebug(COMPONENT_MIGRATION,
		     "Checking for conflict for access in open requests");

	/* If the existing access and deny values conflict with the new access and deny values */
	return ((existing_deny & new_access) != 0) ||
	       ((new_deny & existing_access) != 0);
}

/**
 * Persist the open info to the redis database.
 * Given the bucket, state owner, share access and deny values, persists the 
 * open info to the redis database.
 */
void persist_open_info(const char *bucket, const char *rhdlstr, char *cowner,
		       char *filehandle, uint32_t share_access,
		       uint32_t share_deny)
{
	int num_fields;
	char *share_access_str, *share_deny_str;

	/* Preparing the data points for setting the document */
	share_access_str = (char *)malloc(MAX_LEN);
	share_deny_str = (char *)malloc(MAX_LEN);
	sprintf(share_access_str, "%d", share_access);
	sprintf(share_deny_str, "%d", share_deny);
	num_fields = 4;

	/* Creating a document object */
	char *document_object[] = { "cowner",	    cowner,
				    "share_access", share_access_str,
				    "share_deny",   share_deny_str,
				    "filehandle",   filehandle };

	/* Setting the document in the redis instance */
	set_document(bucket, rhdlstr, document_object, num_fields);

	free(share_access_str);
	free(share_deny_str);
	LogFullDebug(COMPONENT_MIGRATION,
		     "Successfully persisted open info for key %s", rhdlstr);
}

/**
 * Fetch the open info from the redis database.
 * Given the bucket and state owner, fetches the open info from the redis database.
 */
void fetch_persisting_open(struct open_info **open, const char *bucket,
			   const char *rhdlstr)
{
	int num_fields = 0;
	char **document;

	/* Getting the document from the redis instance */
	get_document(bucket, rhdlstr, &document, &num_fields);

	/* Creating an open info object & setting the fetched document */
	if (num_fields > 0) {
		*open = (struct open_info *)malloc(sizeof(struct open_info));
		(*open)->cowner = document[1];
		(*open)->share_access =
			(uint32_t)strtoul(document[3], NULL, 10);
		(*open)->share_deny = (uint32_t)strtoul(document[5], NULL, 10);
		(*open)->filehandle = document[7];
	}
	LogFullDebug(COMPONENT_MIGRATION,
		     "Successfully fetched open info for key %s", rhdlstr);
}

void delete_persisting_open(const char *bucket, const char *rhdlstr)
{
	LogAlways(COMPONENT_MIGRATION, "Deleting open info for key %s",
		  rhdlstr);
}