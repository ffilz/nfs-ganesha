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

typedef enum grpc_credentials_type {
	GRPC_CRED_LOCAL = 0,
	GRPC_CRED_SSL = 1,
} grpc_credentials_type_t;

/* Inits grpc module. */
void grpc__init(uint16_t port, grpc_credentials_type_t cred_type,
		char *server_cert, char *server_key, char *ca_cert,
		sockaddr_t *addr);

void grpc__shutdown(void);
#ifdef __cplusplus
}
#endif
#endif /* GANESHA_GRPC_H */
