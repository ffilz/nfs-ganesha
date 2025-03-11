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
	LogEvent(COMPONENT_ALL, "DATABASE: Connecting to %s:%d", ip_addr, port);

	/* Connecting to the redis instance */
	redis_context = redisConnect(ip_addr, port);

	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis Connection failed");
		return -1;
	}
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

	LogAlways(COMPONENT_ALL, "Setting key %s", key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Set the key-value pair in the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "SET %s %s", key,
					   value);

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to set k-v %s-%s in DB", key,
			 value);
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

	LogAlways(COMPONENT_ALL, "Getting key %s", key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Get the key-value pair from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "GET %s", key);
	if (reply->len == 0) {
		freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to get key %s", key);
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

	LogAlways(COMPONENT_ALL, "Deleting key-value pair for key %s", key);

	/* Check if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Delete the key-value pair from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "DEL %s", key);
	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Filed to delete key %s", key);
		return 0;
	}

	freeReplyObject(reply);
	return 0;
}

/* Setting key-val pair in a bucket in redis */
static int set_document_field(const char *bucket, const char *key,
			      const char *field, const char *value)
{
	char *key_with_bucket;
	redisReply *reply;

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Set the key-value pair in the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HSET %s %s %s",
					   key_with_bucket, field, value);

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		return -1;
	}

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
	LogAlways(COMPONENT_ALL, "Setting document for key %s in bucket %s",
		  primary_key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Set the document in the redis instance */
	for (int i = 0; i < num_fields; i++) {
		if (set_document_field(bucket, primary_key, arr[2 * i],
				       arr[2 * i + 1]) < 0) {
			LogDebug(COMPONENT_ALL,
				 "Failed to set the document for key %s",
				 primary_key);
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

	LogAlways(COMPONENT_ALL, "Getting document for key %s in bucket %s",
		  key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Get the document corresponding to the key from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HGETALL %s",
					   key_with_bucket);

	if (reply == NULL || reply->type != REDIS_REPLY_ARRAY ||
	    reply->elements == 0) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to get key %s in bucket %s",
			 key, bucket);
		return -1;
	}

	*num_fields = reply->elements / 2;
	*document = (char **)malloc(reply->elements * sizeof(char *));

	/* Storing the retrieved information in the document */
	for (size_t i = 0; i < reply->elements; i++) {
		(*document)[i] = strdup(reply->element[i]->str);
	}

	freeReplyObject(reply);
	return 0;
}

int get_document_without_bucket(const char *key, char ***document,
				int *num_fields)
{
	redisReply *reply;

	LogAlways(COMPONENT_ALL, "Getting document for key %s", key);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Get the document corresponding to the key from the redis instance */
	reply = (redisReply *)redisCommand(redis_context, "HGETALL %s", key);
	if (reply == NULL || reply->type != REDIS_REPLY_ARRAY ||
	    reply->elements == 0) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to get key %s", key);
		return -1;
	}

	*num_fields = reply->elements / 2;
	*document = (char **)malloc(reply->elements * sizeof(char *));

	for (size_t i = 0; i < reply->elements; i++) {
		(*document)[i] = strdup(reply->element[i]->str);
	}

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
	char *key_with_bucket;
	redisReply *reply;

	LogAlways(COMPONENT_ALL, "Updating document for key %s in bucket %s",
		  key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Update the document corresponding to the key in the redis instance */
	reply = redisCommand(redis_context, "HSET %s %s %s", key_with_bucket,
			     field, new_value);

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogAlways(COMPONENT_ALL, "Failed to update key %s in bucket %s",
			  key, bucket);
		return -1;
	}

	freeReplyObject(reply);
	return 0;
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

	LogAlways(COMPONENT_ALL, "Deleting document for key %s in bucket %s",
		  key, bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	/* Creating key with bucket prefix */
	key_with_bucket = (char *)malloc(strlen(key) + strlen(bucket) + 2);
	snprintf(key_with_bucket, strlen(key) + strlen(bucket) + 2, "%s:%s",
		 bucket, key);

	/* Delete the document corresponding to the key from the redis instance */
	reply = redisCommand(redis_context, "HDEL %s", key_with_bucket);

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to delete key %s in bucket %s",
			 key, bucket);
		return -1;
	}

	freeReplyObject(reply);
	return 0;
}

/**
 * Function to get a bucket from the redis instance.
 * Given the bucket name, gets the bucket from the redis instance.
 * Returns 0 on success, -1 on failure
*/
int get_bucket(const char *bucket, char ****doc_list, int *num_documents,
	       int *num_fields)
{
	redisReply *reply, *keys;
	char pattern[256];
	char cursor[16] = "0";

	*doc_list = NULL;
	*num_documents = 0;
	*num_fields = 0;

	LogAlways(COMPONENT_ALL, "Getting entire bucket %s stored in redis",
		  bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "%s:*", bucket);

	do {
		reply = redisCommand(redis_context,
				     "SCAN %s MATCH %s COUNT 100", cursor,
				     pattern);

		if (!reply || reply->type != REDIS_REPLY_ARRAY ||
		    reply->elements < 2) {
			if (reply)
				freeReplyObject(reply);
			LogDebug(COMPONENT_ALL,
				 "SCAN failed or returned unexpected data");
			return -1;
		}

		snprintf(cursor, sizeof(cursor), "%s", reply->element[0]->str);
		keys = reply->element[1];
		*doc_list = (char ***)realloc(
			*doc_list,
			(*num_documents + keys->elements) * sizeof(char **));

		if (!(*doc_list)) {
			LogDebug(COMPONENT_ALL, "Memory allocation failed");
			freeReplyObject(reply);
			return -1;
		}

		for (size_t i = 0; i < keys->elements; i++) {
			char *key = keys->element[i]->str;

			if (get_document_without_bucket(key, &(*doc_list)[i],
							num_fields) < 0) {
				freeReplyObject(reply);
				return -1;
			}
			(*num_documents)++;
		}

		freeReplyObject(reply);

	} while (strcmp(cursor, "0") != 0);

	return 0;
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

	LogAlways(COMPONENT_ALL, "Deleting entire bucket %s stored in redis",
		  bucket);

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "%s*", bucket);

	do {
		reply = redisCommand(redis_context,
				     "SCAN %s MATCH %s COUNT 100", cursor,
				     pattern);

		if (!reply || reply->type != REDIS_REPLY_ARRAY ||
		    reply->elements < 2) {
			LogDebug(COMPONENT_ALL,
				 "SCAN failed or returned unexpected data");
			freeReplyObject(reply);
			return -1;
		}

		snprintf(cursor, sizeof(cursor), "%s", reply->element[0]->str);
		redisReply *keys = reply->element[1];

		for (size_t i = 0; i < keys->elements; i++) {
			const char *key = keys->element[i]->str;
			redisReply *delReply =
				redisCommand(redis_context, "DEL %s", key);
			if (delReply)
				freeReplyObject(delReply);
		}
		freeReplyObject(reply);

	} while (strcmp(cursor, "0") != 0);

	return 0;
}

/**
 * Function to flush the redis instance.
 * Returns 0 on success, -1 on failure
*/
int flush_doc_db()
{
	redisReply *reply;

	LogAlways(COMPONENT_ALL, "Flushing entire database");

	/* Checking if redis is connected */
	if (redis_context == NULL || redis_context->err) {
		LogDebug(COMPONENT_ALL, "Redis not connected");
		return -1;
	}

	reply = (redisReply *)redisCommand(redis_context, "FLUSHDB");

	if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
		if (reply)
			freeReplyObject(reply);
		LogDebug(COMPONENT_ALL, "Failed to flush database");
		return -1;
	}

	freeReplyObject(reply);
	return 0;
}