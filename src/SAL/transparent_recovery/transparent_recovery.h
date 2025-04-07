#ifndef _TRANSPARENT_RECOVERY
#define _TRANSPARENT_RECOVERY

#include "docDB.h"
#include "fsal.h"
#include "bsd-base64.h"

#define MAX_LEN 256

/**
 * Transparent Recovery Interface for Open & Byte Range Locks.
 * Will be extended to support delegations & layouts in future.
 * 
 * Plan of Action: Create interfaces for storing the open states in the
 * redis database.
 */

/**
 * Grace Handling Interface(s)
 * - Initializing transparent recovery
 * - Finishing transparent recovery
 */

bool init_transparet_recovery();
bool grace_end_exchange_buckets();

/**
 * Clid-Cowner Mapper Interface(s)
 * - For mapping clientids to cowners
 * - For updating cowners
 * - For getting cowners
 * - For deleting cowners
 */

struct clid_maps_cowner {
	uint64_t clientid;
	char *cowner;
	struct clid_maps_cowner *next;
};

int add_clid_maps_cowner(uint64_t clientid, char *cowner);
int update_clid_maps_cowner(uint64_t clientid, char *new_cowner);
int get_clid_maps_cowner(uint64_t clientid, char **cowner);
int del_clid_maps_cowner(uint64_t clientid);

/**
 * - Primary Key: Bucket + "filesystem" + FSID Major + FSID Minor
 * File System (Primay Key -> Serial Number) 
 * 
 * - Primary Key: Bucket + "file" + FSID Major + FSID Minor + FileID
 * File MetaData
 * {
 * 	Key
 * 	FileID
 * 	Open Key Prefix
 * 	Serial Number Open Counter
 * 	Lock Key Prefix
 * 	Serial Number Open Counter
 * }
 * 
 * - Primary Key: Open Key Prefix + "open" + Serial Number Open Counter
 * Open
 * {
 * 	Key
 * 	FileID
 * 	Open Owner
 * 	Share Access
 * 	Share Deny
 * }
 * 
 * - Primary Key: Lock Key Prefix + "lock" + Serial Number Lock Counter
 * Lock
 * {
 * 	Key
 * 	File ID
 * 	Lock Owner 
 * 	Lock Type 
 * 	Start 
 * 	Length
 * }
 * 
 * How to persist a open info?
 * - Get FSID Major, Minor & FileID
 * - Fetch File Metadata
 * - Fetch the list of opens for that file
 * - Check for a conflict against already existing opens
 * - If conflict return grace error otherwise pesist new open
 * 
 * How to close a file?
 * - Get FSID Major, Minor & FileID
 * - Fetch File Metadata
 * - Fetch the list of opens for that file
 * - Get the serial number for the matching file entry
 * - Delete that open entry from the DB for a successful close
 * 
 * How to update a particular open
 * - Fetch the list of opens for that file
 * - Iterate though the list & find the data for matching open
 * - Get the serial number for the matching open entry
 * - Call update API provided by docDB interface
 * 
 * How to persist a lock info?
 * - Get FSID Major, Minor & FileID
 * - Fetch File Metadata
 * - Fetch the list of locks for that file
 * - Chck for a conflicting lock in the list
 * - If conflict return grace error otherwise perisist new lock info
 * 
 * How to unlock?
 * - Fetch the list of locks for that file
 * - Iterate through the list to find overlapping locks
 * - Find the serial number for the same
 * - Delete or modify the ranges in the entry
 */

struct file_metadata {
	char *key;
	char *open_key_prefix;
	char *lock_key_prefix;
	uint64_t fileid;
	uint64_t open_counter;
	uint64_t lock_counter;
};

void persist_file_md(char *bucket, uint32_t fsid_maj, uint32_t fsid_min,
		     struct file_metadata *file);
void fetch_file_md(char *bucket, uint32_t fsid_maj, uint32_t fsid_min,
		   uint64_t fileid, struct file_metadata *file);

// delete a file metadata from the database
// static void delete_file_md(char *bucket, const struct file_metadata *file)
// {
// 	del_document(bucket, file->key);
// }

/**
 * Open State Recovery Interface(s)
 * - For persisting open states
 * - For fetching persisting open states
 * - For conflict detection
*/

/**
 * structure for encapsulating, important data for
 * an open request
 */
struct open_info {
	char *key;
	uint64_t fileid;
	char *open_owner;
	uint32_t share_access;
	uint32_t share_deny;
};

void persist_open_info(const char *bucket, const struct file_metadata *file,
		       const struct open_info *open);
void fetch_persisting_opens(const char *bucket,
			    const struct file_metadata *file,
			    struct open_info *opens);
void delete_open_info(const char *bucket, const struct file_metadata *file,
		      struct open_info *open);
bool is_conflicting_open(const struct open_info *opens,
			 const struct open_info *open);

/**
 * Byte Range Lock Recovery Interface(s)
 * - For persisting lock states
 * - For fetching persisting locks
 * - For conflict detection
 */

/*
 * structure for encapsulating, important data for  
 * a byte-range lock request
 */
struct lock_info {
	char *key;
	uint64_t fileid;
	char *lock_owner;
	uint32_t type;
	uint64_t start;
	uint64_t length;
};

void persist_lock_info(const char *bucket, struct file_metadata *file,
		       struct lock_info *lock);
void fetch_persisting_locks(const char *bucket,
			    const struct file_metadata *file,
			    struct lock_info *lock);
void delete_lock_info(const char *bucket, struct file_metadata *file,
		      struct lock_info *lock);
bool is_conflicting_lock(struct lock_info *locks, struct lock_info *lock);

#endif