#include "transparent_recovery.h"
#include "log.h"

static struct clid_maps_cowner *clid_maps_cowner_head = NULL;
static struct clid_maps_cowner *clid_maps_cowner_tail = NULL;

/**
 * Add a new clientid-cowner pair to the list.
 * Returns 0 on success, -1 on failure
 */
int add_clid_maps_cowner(uint64_t clientid, char *cowner)
{
	LogAlways(COMPONENT_ALL,
		  "CLIDMAP: Updating cowner for clientid-cowner %lu - %s",
		  clientid, cowner);
	struct clid_maps_cowner *new_node = (struct clid_maps_cowner *)malloc(
		sizeof(struct clid_maps_cowner));

	new_node->clientid = clientid;
	new_node->cowner = strdup(cowner);
	if (clid_maps_cowner_head == NULL) {
		clid_maps_cowner_head = new_node;
	} else {
		clid_maps_cowner_tail->next = new_node;
	}
	clid_maps_cowner_tail = new_node;
	return 0;
}

/**
 * Update the cowner for a given clientid.
 * Returns 0 on success, -1 on failure
 */
int update_clid_maps_cowner(uint64_t clientid, char *new_cowner)
{
	LogAlways(COMPONENT_ALL, "CLIDMAP: Updating cowner for clientid %lu",
		  clientid);
	struct clid_maps_cowner *curr_node = clid_maps_cowner_head;
	while (curr_node != NULL) {
		if (curr_node->clientid == clientid) {
			curr_node->cowner = new_cowner;
			return 0;
		}
		curr_node = curr_node->next;
	}
	LogAlways(COMPONENT_ALL,
		  "CLIDMAP: Failed to update cowner for clientid %lu",
		  clientid);
	return -1;
}

/**
 * Get the cowner for a given clientid.
 * Returns 0 on success, -1 on failure
 */
int get_clid_maps_cowner(uint64_t clientid, char **cowner)
{
	LogAlways(COMPONENT_ALL, "CLIDMAP: Getting cowner for clientid %lu",
		  clientid);
	struct clid_maps_cowner *curr_node = clid_maps_cowner_head;
	while (curr_node != NULL) {
		if (curr_node->clientid == clientid) {
			*cowner = curr_node->cowner;
			return 0;
		}
		curr_node = curr_node->next;
	}
	LogAlways(COMPONENT_ALL,
		  "CLIDMAP: Failed to get cowner for clientid %lu", clientid);
	return -1;
}

/**
 * Delete the cowner for a given clientid.
 * Returns 0 on success, -1 on failure
 */
int del_clid_maps_cowner(uint64_t clientid)
{
	LogAlways(COMPONENT_ALL, "CLIDMAP: Deleting cowner for clientid %lu",
		  clientid);
	struct clid_maps_cowner *curr_node = clid_maps_cowner_head;
	struct clid_maps_cowner *prev_node = NULL;
	while (curr_node != NULL) {
		if (curr_node->clientid != clientid) {
			prev_node = curr_node;
			curr_node = curr_node->next;
			continue;
		}
		if (prev_node == NULL) {
			clid_maps_cowner_head = curr_node->next;
		} else {
			prev_node->next = curr_node->next;
		}
		return 0;
	}
	LogAlways(COMPONENT_ALL,
		  "CLIDMAP: Failed to delete cowner for clientid %lu",
		  clientid);
	return -1;
}