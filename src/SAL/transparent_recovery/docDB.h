#ifndef _DOC_DB
#define _DOC_DB

#include "log.h"

/* Pointer to global variable representing bucket name */
extern char *bucket;
extern char *grace_bucket; /* bucket to be used during grace period*/

/*
 * Interface for interaction with the redis instance(s)
 * acting as database running in-memory 
 */
int connect_doc_db(const char *const ip_addr, int port);

/* Interface for raw key-value operations */
int set_key(const char *key, const char *value);
int get_key(const char *key, char **value);
int del_key(const char *key);

/* Interface for bucketwise key-document operations */
int set_document(const char *bucket, const char *key, char *arr[],
		 int num_fields);
int get_document(const char *bucket, const char *key, char ***document,
		 int *num_fields);
int del_document(const char *bucket, const char *key);
int update_document(const char *bucket, const char *key, const char *field,
		    const char *new_value);
int get_bucket(const char *bucket, char ****doc_list, int *num_documents,
	       int *num_fields);
int delete_bucket(const char *bucket);

/* Interface for flushing the redis instance */
int flush_doc_db();

#endif