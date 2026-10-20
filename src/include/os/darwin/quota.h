/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Darwin (macOS) quota header.
 *
 * Darwin's quotactl(2) takes the BSD argument order --
 * int quotactl(const char *, int, int, caddr_t) -- matching FreeBSD rather
 * than Linux.
 */

#ifndef QUOTA_DARWIN_H
#define QUOTA_DARWIN_H

#include <sys/types.h>
#include <sys/quota.h>

#define QUOTACTL(cmd, path, id, addr) quotactl(path, (cmd), id, (caddr_t)(addr))

#endif /* QUOTA_DARWIN_H */
