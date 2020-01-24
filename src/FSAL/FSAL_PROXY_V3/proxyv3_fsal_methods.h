/*
 * Copyright 2020 Google LLC
 * Author: Solomon Boulos <boulos@google.com>
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
 *
 * -------------
 */

#ifndef _PROXY_V3_FSAL_METHODS_H_
#define _PROXY_V3_FSAL_METHODS_H_

#include "config.h"
#include "fsal.h"
#include "FSAL/fsal_init.h"

struct proxyv3_fsal_module {
   struct fsal_module module;
   struct fsal_obj_ops handle_ops;
};

// Start with just needing the Srv_Addr parameter to an NFSv3 server.
struct proxyv3_client_params {
   sockaddr_t srv_addr;
};

struct proxyv3_export {
   struct fsal_export export;
   struct proxyv3_client_params params;
};

extern struct proxyv3_fsal_module PROXY_V3;

#endif
