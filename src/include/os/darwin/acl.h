/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Darwin (macOS) ACL header.
 *
 * Unlike the Linux variant this declares no fallback prototypes: macOS ships
 * acl_get_fd_np()/acl_set_fd_np() in libc (they are BSD-origin).
 */

#ifndef _ACL_DARWIN_H
#define _ACL_DARWIN_H

#include <limits.h>
#include <sys/types.h>
#include <sys/acl.h>

#endif /* _ACL_DARWIN_H */
