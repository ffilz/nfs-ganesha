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
	char *cowner;
	uint32_t share_access;
	uint32_t share_deny;
	char *filehandle;
};

void persist_open_info(const char *bucket, const char *rhdlstr, char *cowner,
		       char *filehandle, uint32_t share_access,
		       uint32_t share_deny);
void fetch_persisting_open(struct open_info **open, const char *bucket,
			   const char *rhdlstr);
bool is_conflict_persisting_opens(uint32_t existing_access,
				  uint32_t existing_deny, uint32_t new_access,
				  uint32_t new_deny);

void delete_persisting_open(const char *bucket, const char *rhdlstr);

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
	char *lock_owner;
	uint32_t lock_type;
	uint64_t lock_start;
	uint64_t lock_length;
	uint32_t fsid_major;
	uint32_t fsid_minor;
	uint64_t fileid;
	char *primary_key;
};

void persist_lock_info(const char *bucket, fsal_lock_param_t *lock,
		       state_owner_t *owner, struct fsal_obj_handle *sle_obj);
void fetch_persisting_locks(const char *bucket);
bool is_conflict_persisting_locks(fsal_lock_param_t *lock, state_owner_t *owner,
				  struct fsal_obj_handle *sle_obj);
void delete_persisting_lock(const char *bucket, fsal_lock_param_t *lock,
			    state_owner_t *owner,
			    struct fsal_obj_handle *sle_obj);

#endif