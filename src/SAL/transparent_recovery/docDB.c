#include <hiredis/hiredis.h>
#include "docDB.h"

/* Pointer to the database bucket for storing state information*/
char *bucket = NULL;
char *grace_bucket =
	NULL; /* Pointer to the bucket to be used during grace period */
static redisContext *redis_context = NULL; /* Pointer to the redis context */

/**
 * Function to connect to the redis instance.
 * Given the IP address and port of the running redis instance,
 * connects to the redis instance.
 * Returns 0 on success, -1 on failure
*/
int connect_doc_db(const char *const ip_addr, int port)
{
	/* Connecting to the redis instance */
	redis_context = redisConnect(ip_addr, port);

	if (redis_context == NULL) {
		LogAlways(COMPONENT_MIGRATION,
			  "Failed to allocate redisContext");
		return -1;
	} else if (redis_context->err) {
		LogAlways(COMPONENT_MIGRATION, "Connection error: %s",
			  redis_context->errstr);
		return -1;
	}
	LogAlways(COMPONENT_MIGRATION, "Connected to redis successfully");
	return 0;
}

/**
 * Function to set a key-value pair in the redis instance.
 * Given the key and value, sets the key-value pair in the redis instance.
 * Returns 0 on success, -1 on failure
*/
int set_key(const char *key, const char *value)
{
	redisReply *reply;

	LogFullDebug(COMPONENT_MIGRATION, "Setting key %s", key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	/* Set the key-value pair in the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "SET %s %s", key,
					   value);

	if (reply == NULL) {
		LogAlways(COMPONENT_MIGRATION, "Command failed: %s",
			  redis_context->errstr);
		return -1;
	} else if (!(reply->type == REDIS_REPLY_STATUS &&
		     strcmp(reply->str, "OK") == 0)) {
		freeReplyObject(reply);
		return -1;
	}

	freeReplyObject(reply);
	return 0;
}

/**
 * Function to get a value for a key from the redis instance.
 * Given the key, gets the key-value pair from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int get_key(const char *key, char **value)
{
	redisReply *reply;

	LogFullDebug(COMPONENT_MIGRATION, "Getting key %s", key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogCrit(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	/* Get the key-value pair from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "GET %s", key);

	if (reply == NULL) {
		LogAlways(COMPONENT_MIGRATION, "Failed to execute command: %s",
			  redis_context->errstr);
		return -1;
	}
	if (reply->type == REDIS_REPLY_NIL) {
		LogAlways(COMPONENT_MIGRATION, "Key not found");
		return -1;
	}
	if (reply->type != REDIS_REPLY_STRING) {
		LogAlways(COMPONENT_MIGRATION, "Unexpected error");
		return -1;
	}

	*value = (char *)malloc(sizeof(reply->str) + 1);
	strcpy(*value, reply->str);
	freeReplyObject(reply);

	return 0;
}

/**
 * Function to delete a key from the redis instance.
 * Given the key, deletes the key-value pair from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int del_key(const char *key)
{
	redisReply *reply;
	int status;

	LogFullDebug(COMPONENT_MIGRATION, "Deleting key-value pair for key %s",
		     key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		status = -1;
		goto out;
	}

	/* Delete the key-value pair from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "DEL %s", key);
	status = -1;
	if (reply == NULL) {
		LogAlways(COMPONENT_MIGRATION, "Redis command failed: %s",
			  redis_context->errstr);
		return status;
	}
	if (reply->type == REDIS_REPLY_ERROR) {
		LogAlways(COMPONENT_MIGRATION, "Redis DEL error: %s",
			  reply->str);
		goto out;
	}
	if (reply->type != REDIS_REPLY_INTEGER) {
		LogAlways(COMPONENT_MIGRATION, "Unexpected error");
		goto out;
	}
	if (reply->integer == 0) {
		LogAlways(COMPONENT_MIGRATION, "Key not found");
		goto out;
	}
	LogAlways(COMPONENT_MIGRATION, "Key deleted successfully");
	status = 0;
out:
	freeReplyObject(reply);
	return status;
}

/* Setting key-val pair in a bucket in redis */
static int set_document_field(const char *bucket, const char *key,
			      const char *field, const char *value)
{
	char *key_with_bucket;
	redisReply *reply;
	int status;

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Set the key-value pair in the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HSET %s %s %s",
					   key_with_bucket, field, value);
	status = -1;
	if (reply == NULL) {
		LogAlways(COMPONENT_MIGRATION, "Redis HSET failed: %s",
			  redis_context->errstr);
		return status;
	}
	if (reply->type == REDIS_REPLY_INTEGER) {
		status = 0;
		if (reply->integer == 1)
			LogAlways(COMPONENT_ALL, "Field added to hash");
		else
			LogAlways(COMPONENT_MIGRATION, "Field updated in hash");
		goto out;
	} else if (reply->type == REDIS_REPLY_ERROR) {
		LogAlways(COMPONENT_ALL, "Redis HSET error: %s", reply->str);
	}
out:
	freeReplyObject(reply);
	return 0;
}

/**
 * Function to set a document in the redis instance.
 * Given the bucket, primary-key and field-value pairs, sets the document in the redis instance.
 * Returns 0 on success, -1 on failure
*/
int set_document(const char *bucket, const char *primary_key, char *arr[],
		 int num_fields)
{
	/* Set the document in the redis instance */
	for (int i = 0; i < num_fields; i++) {
		if (set_document_field(bucket, primary_key, arr[2 * i],
				       arr[2 * i + 1]) < 0) {
			return -1;
		}
	}
	return 0;
}

/**
 * Function to get a document from the redis instance.
 * Given the bucket and primary-key, gets the document from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int get_document(const char *bucket, const char *key, char ***document,
		 int *num_fields)
{
	char *key_with_bucket;
	redisReply *reply;
	int status;

	LogFullDebug(COMPONENT_MIGRATION,
		     "Getting document for key %s in bucket %s", key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Get the document corresponding to the key from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HGETALL %s",
					   key_with_bucket);
	status = -1;
	if (reply == NULL) {
		LogAlways(COMPONENT_ALL, "Redis HGETALL failed: %s",
			  redis_context->errstr);
		return status;
	}
	if (reply->type == REDIS_REPLY_ERROR) {
		LogAlways(COMPONENT_ALL, "Redis HGETALL error: %s", reply->str);
		goto out;
	}
	if (reply->type != REDIS_REPLY_ARRAY) {
		LogAlways(COMPONENT_ALL, "Unexpected error");
		goto out;
	}

	*num_fields = reply->elements / 2;
	*document = (char **)malloc(reply->elements * sizeof(char *));
	/* Storing the retrieved information in the document */
	for (size_t i = 0; i < reply->elements; i++) {
		(*document)[i] = strdup(reply->element[i]->str);
	}

out:
	freeReplyObject(reply);
	return 0;
}

int get_document_without_bucket(const char *key, char ***document,
				int *num_fields)
{
	redisReply *reply;
	int status;

	LogFullDebug(COMPONENT_MIGRATION, "Getting document for key %s", key);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	/* Get the document corresponding to the key from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HGETALL %s", key);
	status = -1;
	if (reply == NULL) {
		LogAlways(COMPONENT_ALL, "Redis HGETALL failed: %s",
			  redis_context->errstr);
		return status;
	}
	if (reply->type == REDIS_REPLY_ERROR) {
		LogAlways(COMPONENT_ALL, "Redis HGETALL error: %s", reply->str);
		goto out;
	}
	if (reply->type != REDIS_REPLY_ARRAY) {
		LogAlways(COMPONENT_ALL, "Unexpected error");
		goto out;
	}

	*num_fields = reply->elements / 2;
	*document = (char **)malloc(reply->elements * sizeof(char *));
	/* Storing the retrieved information in the document */
	for (size_t i = 0; i < reply->elements; i++) {
		(*document)[i] = strdup(reply->element[i]->str);
	}

out:
	freeReplyObject(reply);
	return 0;
}

/**
 * Function to update a document in the redis instance.
 * Given the bucket, primary-key and field-value pairs, updates the document in the redis instance.
 * Returns 0 on success, -1 on failure
 */
int update_document(const char *bucket, const char *key, const char *field,
		    const char *new_value)
{
	LogFullDebug(COMPONENT_MIGRATION,
		     "Updating document for key %s in bucket %s", key, bucket);

	return set_document_field(bucket, key, field, new_value);
}

/**
 * Function to delete a document from the redis instance.
 * Given the bucket and primary-key, deletes the document from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int del_document(const char *bucket, const char *key)
{
	char *key_with_bucket;
	redisReply *reply;
	int status;

	LogFullDebug(COMPONENT_MIGRATION,
		     "Deleting document for key %s in bucket %s", key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Delete the document corresponding to the key from the redis instance */
	reply = redisCommand(redis_context, "DEL %s", key_with_bucket);

	status = -1;
	if (reply == NULL) {
		LogAlways(COMPONENT_ALL, "Redis DEL failed: %s",
			  redis_context->errstr);
		return status;
	}
	if (reply->type == REDIS_REPLY_ERROR) {
		LogAlways(COMPONENT_ALL, "Redis DEL error: %s", reply->str);
		goto out;
	}
	if (reply->type != REDIS_REPLY_INTEGER) {
		LogAlways(COMPONENT_ALL, "Unexpected error");
		goto out;
	}
	if (reply->integer == 1) {
		LogAlways(COMPONENT_ALL, "Key deleted successfully");
		status = 0;
	} else
		LogAlways(COMPONENT_ALL, "Key not found");

out:
	freeReplyObject(reply);
	return 0;
}

int get_document_with_prefix(const char *bucket, const char *prefix,
			     char ****documents, int *num_document)
{
	redisReply *reply, *keys;
	char pattern[1024];
	char cursor[16] = "0";
	int num_fields;
	int status;

	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}
	status = 0;
	sprintf(pattern, "%s:%s:*", bucket, prefix);
	do {
		reply = redisCommand(redis_context,
				     "SCAN %s MATCH %s COUNT 100", cursor,
				     pattern);
		if (reply == NULL) {
			LogAlways(COMPONENT_ALL, "Redis DEL failed: %s",
				  redis_context->errstr);
			status = -1;
			goto out;
		}
		if (reply->type == REDIS_REPLY_ERROR) {
			LogAlways(COMPONENT_ALL, "Redis DEL error: %s",
				  reply->str);
			status = -1;
			goto out;
		}

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
			LogAlways(COMPONENT_MIGRATION, "Unexpected data");
			status = -1;
			goto out;
		}

		snprintf(cursor, sizeof(cursor), "%s", reply->element[0]->str);
		keys = reply->element[1];
		*documents = (char ***)realloc(
			*documents,
			(*num_document + keys->elements) * sizeof(char **));

		if (!(*documents)) {
			LogDebug(COMPONENT_MIGRATION,
				 "Memory allocation failed");
			status = -1;
			goto out;
		}

		for (size_t i = 0; i < keys->elements; i++) {
			char *key = keys->element[i]->str;
			status = get_document_without_bucket(
				key, &(*documents)[i], &num_fields);
			if (status != 0)
				goto out;
			(*num_document)++;
		}

		freeReplyObject(reply);

	} while (strcmp(cursor, "0") != 0);

out:
	if (reply)
		freeReplyObject(reply);
	return status;
}

/**
 * Function to get a bucket from the redis instance.
 * Given the bucket name, gets the bucket from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int get_bucket(const char *bucket, char ****documents, int *num_document,
	       int *num_fields)
{
	redisReply *reply, *keys;
	char pattern[256];
	char cursor[16] = "0";
	int status;

	*documents = NULL;
	*num_document = 0;
	*num_fields = 0;

	LogFullDebug(COMPONENT_MIGRATION,
		     "Getting entire bucket %s stored in redis", bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "%s:*", bucket);
	status = 0;

	do {
		reply = redisCommand(redis_context,
				     "SCAN %s MATCH %s COUNT 100", cursor,
				     pattern);
		if (reply == NULL) {
			LogAlways(COMPONENT_ALL, "Redis DEL failed: %s",
				  redis_context->errstr);
			status = -1;
			goto out;
		}
		if (reply->type == REDIS_REPLY_ERROR) {
			LogAlways(COMPONENT_ALL, "Redis DEL error: %s",
				  reply->str);
			status = -1;
			goto out;
		}

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
			LogAlways(COMPONENT_MIGRATION, "Unexpected data");
			status = -1;
			goto out;
		}

		snprintf(cursor, sizeof(cursor), "%s", reply->element[0]->str);
		keys = reply->element[1];
		*documents = (char ***)realloc(
			*documents,
			(*num_document + keys->elements) * sizeof(char **));

		if (!(*documents)) {
			LogDebug(COMPONENT_MIGRATION,
				 "Memory allocation failed");
			status = -1;
			goto out;
		}

		for (size_t i = 0; i < keys->elements; i++) {
			char *key = keys->element[i]->str;
			status = get_document_without_bucket(
				key, &(*documents)[i], num_fields);
			if (status != 0)
				goto out;
			(*num_document)++;
		}

		freeReplyObject(reply);

	} while (strcmp(cursor, "0") != 0);

out:
	if (reply)
		freeReplyObject(reply);
	return status;
}

/**
 * Function to delete an entire bucket from the redis instance.
 * Given the bucket name, deletes the bucket from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int delete_bucket(const char *bucket)
{
	redisReply *reply;
	char cursor[16] = "0";
	char pattern[256];
	int status;

	LogFullDebug(COMPONENT_MIGRATION,
		     "Deleting entire bucket %s stored in redis", bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "%s*", bucket);
	status = 0;

	do {
		reply = redisCommand(redis_context,
				     "SCAN %s MATCH %s COUNT 100", cursor,
				     pattern);

		if (reply == NULL) {
			LogAlways(COMPONENT_ALL, "Redis DEL failed: %s",
				  redis_context->errstr);
			status = -1;
			goto out;
		}
		if (reply->type == REDIS_REPLY_ERROR) {
			LogAlways(COMPONENT_ALL, "Redis DEL error: %s",
				  reply->str);
			status = -1;
			goto out;
		}

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
			LogAlways(COMPONENT_MIGRATION, "Unexpected data");
			status = -1;
			goto out;
		}

		snprintf(cursor, sizeof(cursor), "%s", reply->element[0]->str);
		redisReply *keys = reply->element[1];

		for (size_t i = 0; i < keys->elements; i++) {
			const char *key = keys->element[i]->str;
			redisReply *delReply;

			delReply = redisCommand(redis_context, "DEL %s", key);

			if (delReply) {
				freeReplyObject(delReply);
			} else {
				status = -1;
				goto out;
			}
		}
		freeReplyObject(reply);

	} while (strcmp(cursor, "0") != 0);

out:
	if (reply)
		freeReplyObject(reply);
	return status;
}

/**
 * Function to flush the redis instance.
 * Returns 0 on success, -1 on failure
*/
int flush_doc_db()
{
	redisReply *reply;

	LogEvent(COMPONENT_MIGRATION, "Flushing entire database");

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_MIGRATION, "Redis not connected");
		return -1;
	}

	reply = (redisReply *)redisCommand(redis_context, "FLUSHDB");

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_MIGRATION, "Failed to flush database");
		return -1;
	}

	freeReplyObject(reply);
	return 0;
}