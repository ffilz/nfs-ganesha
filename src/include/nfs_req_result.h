/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2025 SmartX, LLC.
 * Author: Zhitao Li <zhitao.li@smartx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file   nfs_req_result.h
 * @author Zhitao Li <zhitao.li@smartx.com>
 * @date   Wed Mar 19 14:15:00 2025
 *
 * @brief declaration and to_str functiosn of enum nfs_req_result
 *
 * @note used by nfs_metrics.h, nfs_proto_data.h
 */

#ifndef NFS_REQ_RESULT_H
#define NFS_REQ_RESULT_H

enum nfs_req_result {
	NFS_REQ_OK,
	NFS_REQ_DROP,
	NFS_REQ_ERROR,
	NFS_REQ_REPLAY,
	NFS_REQ_ASYNC_WAIT,
	NFS_REQ_XPRT_DIED,
	NFS_REQ_AUTH_ERR,
};
typedef enum nfs_req_result nfs_req_result;

#endif /* NFS_REQ_RESULT_H */
