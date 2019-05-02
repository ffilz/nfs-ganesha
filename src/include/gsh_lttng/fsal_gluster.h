#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER fsalgluster

#if !defined(GANESHA_LTTNG_FSALGLUSTER_TP_H) || \
	defined(TRACEPOINT_HEADER_MULTI_READ)
#define GANESHA_LTTNG_FSALGLUSTER_TP_H

#include <stdint.h>
#include <lttng/tracepoint.h>

/**
 * @brief Trace freeing of glusterfs_handle
 *
 * @param[in] function	Name of function freeing gluster handle
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_handle_release,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_handle_release,
	TRACE_INFO)

/**
 * @brief Trace lookup op
 *
 * @param[in] function	Name of the function
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_lookup,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_lookup,
	TRACE_INFO)

/**
 * @brief Trace fetching of security label
 *
 * @param[in] function	Name of the function
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_sec_label,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_sec_label,
	TRACE_INFO)

/**
 * @brief Trace operation readdir
 *
 * @param[in] function	Name of the function
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_readdir,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_readdir,
	TRACE_INFO)

/**
 * @brief Trace operation mkdir
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_mkdir,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_mkdir,
	TRACE_INFO)


/**
 * @brief Trace fsal_gluster objectoperation mknod
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_mknod,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_mknod,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster objectoperation symlink
 *
 * @param[in] function	Name of the function
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_make_symlink,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_make_symlink,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster objectoperation readlink
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_read_link,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_read_link,
	TRACE_INFO)

/**
 * @brief Trace getattrs call
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_getattrs,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_getattrs,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster objectoperation link
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_link,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_link,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster objectoperation rename
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_rename,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_rename,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster objectoperation unlink
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_unlink,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_unlink,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster open_fd
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_open_fd,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_open_fd,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster close_fd
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_close_fd,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_close_fd,
	TRACE_INFO)

/**
 * @brief Trace open call for fsal_obj_handle's global fd
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_open_global_fd,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_open_global_fd,
	TRACE_INFO)

/**
 * @brief Trace close call for fsal_obj_handle's global fd
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_close_global_fd,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_close_global_fd,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster find_fd call
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_find_fd,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_find_fd,
	TRACE_INFO)

/**
 * @brief Trace fsal_gluster handle_merge
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_handle_merge,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_handle_merge,
	TRACE_INFO)

/**
 * @brief Trace export op release
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_export_release,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_export_release,
	TRACE_INFO)


/**
 * @brief Trace export op lookup
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_export_lookup,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_export_lookup,
	TRACE_INFO)

/**
 * @brief Trace export op wire_to_host
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_export_wire,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_export_wire,
	TRACE_INFO)

/**
 * @brief Trace export op create_handle
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_create_handle,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_create_handle,
	TRACE_INFO)

/**
 * @brief Trace export op fs_dynamic_info
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_dynamic_info,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_dynamic_info,
	TRACE_INFO)

/**
 * @brief Trace export op create_export
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_export,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_export,
	TRACE_INFO)

/**
 * @brief Trace cleanup func  
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_cleanup,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_cleanup,
	TRACE_INFO)

/**
 * @brief Trace ACL read
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_acl,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_acl,
	TRACE_INFO)

/**
 * @brief Trace setxattr call for storing ACL
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 * @param[in] rc	Return value
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_setxattr_acl,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
		ctf_integer(int, rc, rc);
	)
)

TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_setxattr_acl,
	TRACE_INFO)

/**
 * @brief Trace setattr call to process ACL
 *
 * @param[in] function	Name of the function 
 * @param[in] line	Line number of call
 */

TRACEPOINT_EVENT(
	fsalgl,
	gl_setattr_acl,
	TP_ARGS(const char *, function,
		int, line),
	TP_FIELDS(
		ctf_string(function, function)
		ctf_integer(int, line, line)
	)


TRACEPOINT_LOGLEVEL(
	fsalgl,
	gl_setattr_acl,
	TRACE_INFO)

#endif /* GANESHA_LTTNG_FSALGLUSTER_TP_H */

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "gsh_lttng/fsal_gluster.h"

#include <lttng/tracepoint-event.h>
