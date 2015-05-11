#include <sys/acl.h>

#include "fsal_types.h"

fsal_status_t
posix_acl_2_fsal_acl(acl_t p_posixacl, fsal_acl_t **p_falacl);

fsal_status_t
fsal_acl_2_posix_acl(fsal_acl_t *p_fsalacl, acl_t *p_posixacl);

acl_entry_t
find_entry(acl_t acl, acl_tag_t tag, int id);
