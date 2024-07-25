// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2024 Google LLC
 * contributeur : Shahar Hochma shaharhoch@google.com
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 */

#ifndef __GSH_LTTNG_H__
#define __GSH_LTTNG_H__

#ifdef USE_LTTNG

#define __S1(x) #x
#define __S2(x) __S1(x)
#define LINE_AS_STRING __S2(__LINE__)

// Change include to relative path
#include "gsh_config.h"
#include "lttng_generator.h"
#include <lttng/tracepoint.h>

extern __thread struct req_op_context *op_ctx;

/* Note that __func__ is not a string literal (see http://shortn/_GQDGGpmvUd)
 * and so, unfortunately, cannot be efficiently saved at compile time, so we
 * don't include it in the trace line.
 * To circumvent this, the lttng generator adds the function to the format
 * string itself. */

/* We define temp variable for op_id so that it has a representative key name
 * generated. */
#define GSH_AUTO_TRACEPOINT(prov_name, event_name, log_level, format, ...)     \
  do {                                                                         \
    const uint32_t _server_id_ = nfs_param.core_param.unique_server_id;        \
                                                                               \
    uint32_t _op_id_ = 0;                                                      \
    int32_t _xprt_fd_ = -1;                                                    \
    if (op_ctx) {                                                              \
      _op_id_ = op_ctx->op_id;                                                 \
      if (op_ctx->nfs_reqdata != NULL &&                                       \
          op_ctx->nfs_reqdata->svc.rq_xprt != NULL) {                          \
        _xprt_fd_ = op_ctx->nfs_reqdata->svc.rq_xprt->xp_fd;                   \
      }                                                                        \
    }                                                                          \
                                                                               \
    AUTO_TRACEPOINT(prov_name, event_name, log_level,                          \
                    __FILE__                                                   \
                    ":" LINE_AS_STRING                                         \
                    " | server_id={} | op_id={} | xprt_fd={} | " format,       \
                    _server_id_, _op_id_, _xprt_fd_, ##__VA_ARGS__);           \
  } while (0)

#define GSH_UNIQUE_AUTO_TRACEPOINT(prov_name, event_name, log_level, format,   \
                                   ...)                                        \
  do {                                                                         \
    const uint32_t _server_id_ = nfs_param.core_param.unique_server_id;        \
                                                                               \
    uint32_t _op_id_ = 0;                                                      \
    int32_t _xprt_fd_ = -1;                                                    \
    if (op_ctx) {                                                              \
      _op_id_ = op_ctx->op_id;                                                 \
      if (op_ctx->nfs_reqdata != NULL &&                                       \
          op_ctx->nfs_reqdata->svc.rq_xprt != NULL) {                          \
        _xprt_fd_ = op_ctx->nfs_reqdata->svc.rq_xprt->xp_fd;                   \
      }                                                                        \
    }                                                                          \
                                                                               \
    UNIQUE_AUTO_TRACEPOINT(                                                    \
        prov_name, event_name, log_level,                                      \
        __FILE__ ":" LINE_AS_STRING                                            \
                 " | server_id={} | op_id={} | xprt_fd={} | " format,          \
        _server_id_, _op_id_, _xprt_fd_, ##__VA_ARGS__);                       \
  } while (0)

#else // USE_LTTNG

#define GSH_AUTO_TRACEPOINT(...)
#define GSH_UNIQUE_AUTO_TRACEPOINT(...)

#endif /* USE_LTTNG */
#endif /* __GSH_LTTNG_H__ */