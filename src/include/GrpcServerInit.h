/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * @brief gRPC library for NFS Ganesha.
 */

#ifndef GANESHA_GRPC_H
#define GANESHA_GRPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "ip_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inits grpc module. */
void grpc__init(uint16_t port, char *server_cert, char *server_key,
		char *ca_cert, sockaddr_t *addr);

static bool if_ip_addr_any(const sockaddr_t *addr)
{
	if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;

		for (int i = 0; i < 16; i++) {
			if (addr6->sin6_addr.__in6_u.__u6_addr8[i] != 0) {
				return false;
			}
		}
	} else {
		/* IPv4 */
		if (((struct sockaddr_in *)addr)->sin_addr.s_addr != INADDR_ANY)
			return false;
	}
	return true;
}

#ifdef __cplusplus
}
#endif
#endif /* GANESHA_GRPC_H */
