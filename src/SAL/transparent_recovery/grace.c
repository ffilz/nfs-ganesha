#include "transparent_recovery.h"

/**
 * A List of possible bucket names, for creating storage buckets
 * in the redis storage when the server starts. 
 */
char *bucket_options[] = { "BUCKET0", "BUCKET1" };

/**
 * Function to exchange buckets when the grace is lifted.
 * - Swap the current bucket with the grace bucket 
 * - Delete the old bucket which is used for checking conflicts
 * 	in the grace period
 */
void grace_end_exchange_buckets()
{
	char *old_bucket = bucket; /* Create temporary pointer old bucket */
	bucket = grace_bucket; /* Set current bucket to grace bucket */
	grace_bucket = old_bucket;
	set_key("BUCKET_NAME",
		bucket); /* Set the bucket name in redis storage for future recovery */
	if (old_bucket) /* Delete old bucket, i.e., bucket used for checking conflicts during grace period */
		delete_bucket(old_bucket);
}

/**
 * Function to initialize transparent recovery on server startup
 * - Connect to the redis instance
 * - Get the current bucket name, which will be used for checking conflicts
 * 	during grace period
 * - Set the bucket name to be used during grace period
 */
void init_transparet_recovery()
{
	LogAlways(COMPONENT_ALL, "Initializing transparent recovery");

	if (connect_doc_db("127.0.0.1", 6379) <
	    0) /* Connecting to redis instance */
		LogFatal(COMPONENT_INIT,
			 "Could not connect to redis server on 127.0.0.1:6379");

	bucket = NULL;
	get_key("BUCKET_NAME",
		&bucket); /* Getting the bucket name used during last server life */
	if (!bucket)
		bucket = bucket_options[0];

	/* Setting the bucket to be used during grace period depending upon bucket name */
	if (!strcmp(bucket, bucket_options[0])) {
		grace_bucket = bucket_options[1];
	} else
		grace_bucket = bucket_options[0];
}