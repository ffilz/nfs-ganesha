/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2025, IBM
 * Contributor : Avani Rateria <arateria@redhat.com>
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
 * -------------
 */

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <thread>
#include <mutex>
#include "GrpcServerInit.h"
#include "GrpcServer.h"
#include "nfsService.h"
#include "gsh_rpc.h"
#include <grpcpp/ext/proto_server_reflection_plugin.h>

/*
 *     gRPC (gRPC Remote Procedure Call) is an open-source framework that
 *     enables communication between services through remote procedure
 *     calls (RPCs). It is built on top of HTTP/2 and uses Protocol Buffers
 *     (protobuf) as the interface definition language (IDL).
 *     The below infra facilitates intra server as well as inter server
 *     communication.
 */

// start gRPC server
void GrpcServer::start(uint16_t port, std::string_view server_crt,
		       std::string_view server_key, std::string_view ca_crt)
{
	server_thread_ =
		std::thread([this, port, server_crt, server_key, ca_crt]() {
			gRPCServerStart(port, server_crt, server_key, ca_crt);
		});
}

// gRPC Server thread function
void GrpcServer::gRPCServerStart(uint16_t port, std::string_view server_crt,
				 std::string_view server_key,
				 std::string_view ca_crt)
{
	grpc::ServerBuilder builder;
	GetClientIdService showClientService;
	GetNfsGraceService nfsIngrace;
	GetSessionIdService getClientSessionIds;
	StartNfsGraceService startNfsGrace;
	std::string server_address;

	std::string key = "";
	std::string cert = "";
	std::string ca = "";

	grpc::SslServerCredentialsOptions ssl_opts;
	std::shared_ptr<grpc::ServerCredentials> server_creds;

	{ // Taking a lock
		const std::lock_guard<std::mutex> lock(mutex_);

		LogDebug(COMPONENT_GRPC,
			 "Path to server certificate: %s "
			 "Path to server key : %s"
			 "Path to ca certificate : %s",
			 std::string(server_crt).data(),
			 std::string(server_key).data(),
			 std::string(ca_crt).data());

		// Default IP address family is IPv6 if IPv6 is available
		// and bind to IPv6 wildcard address
		// Else we fall back to IPv4 and bind to IPv4 wildcard address
		// i.e 0.0.0.0.
		// If both are enabled than the sever will automatically
		// accept both IPv6 and IPv4 connections
		// default port_number is 50051

		if (Ipfamily_ == AF_INET6) {
			server_address = ("[" + std::string(ipAddr_) +
					  "]:" + std::to_string(port));
		} else {
			server_address = (std::string(ipAddr_) + ":" +
					  std::to_string(port));
		}

		// We do not want to run multiple instances of
		// gRPC server
		if (server_) {
			LogWarn(COMPONENT_GRPC,
				"gRPC server is already running");

			return;
		}

		// Read the TLS certificate and key files
		key = read_cert_file(server_key);
		cert = read_cert_file(server_crt);
		ca = read_cert_file(ca_crt);

		// If the key or certificated files are not found
		// than gRPC cannot run securely, hence exiting.
		if (key.empty() || cert.empty()) {
			LogWarn(COMPONENT_GRPC,
				"Failed to get server key or certificate."
				"Shutting down gRPC server");

			return;
		}

		// Log the size of the read key
		LogDebug(
			COMPONENT_GRPC,
			"Loaded key : %zu bytes), cert : %zu bytes, ca : %zu bytes",
			key.size(), cert.size(), ca.size());

		grpc::SslServerCredentialsOptions::PemKeyCertPair
			key_cert_pair = { key, cert };

		ssl_opts.pem_key_cert_pairs.push_back(key_cert_pair);

		// If CA certificate is not available than we cannot
		// validate client certificates
		if (!ca.empty()) {
			ssl_opts.pem_root_certs = ca;

			ssl_opts.client_certificate_request =
				GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
		} else {
			LogInfo(COMPONENT_GRPC,
				"CA certificate was not found; as a result,"
				"the server will not validate client certificates");
		}

		server_creds = grpc::SslServerCredentials(ssl_opts);

		// Adding the listening port
		builder.AddListeningPort(server_address, server_creds);

		// Register the service with the builder
		builder.RegisterService(&showClientService);

		builder.RegisterService(&nfsIngrace);

		builder.RegisterService(&getClientSessionIds);

		builder.RegisterService(&startNfsGrace);

		// Reflection Service to enable grpc CLI
		grpc::reflection::InitProtoReflectionServerBuilderPlugin();
	} // Unlocking

	// Start the server
	server_ = builder.BuildAndStart();

	if (!server_) {
		LogFatal(COMPONENT_GRPC, "Failed to start server on %s",
			 server_address.c_str());
	}

	server_->Wait();
}

// Stop the gRPC server
void GrpcServer::stop()
{
	const std::lock_guard<std::mutex> lock(mutex_);

	if (server_) {
		server_->Shutdown();

		if (server_thread_.joinable()) {
			// Wait for the server thread to finish
			server_thread_.join();
		}
	}
}

extern "C" {

// The event is triggered when NFS is initialized
void grpc__init(uint16_t port, char *server_crt, char *server_key, char *ca_crt,
		sockaddr_t *addr)
{
	static bool initialized = false;
	char ipstring[SOCK_NAME_MAX] = "\0";

	if (initialized)
		return;

	//Converting IP address from sockaddr_t to string format
	/*	switch (addr->ss_family) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)addr;
		inet_ntop(AF_INET, &sin->sin_addr, ipstring, INET_ADDRSTRLEN);
        LogInfo(COMPONENT_GRPC, "gRPC Server, IP Address:%s", ipstring);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
		inet_ntop(AF_INET6, &sin6->sin6_addr, ipstring,
			  INET6_ADDRSTRLEN);
        LogInfo(COMPONENT_GRPC, "gRPC Server, IP Address:%s", ipstring);
		break;
	}
	default:
		LogInfo(COMPONENT_GRPC,
			"Failed to start gRPC Server, Reason:Unknown IP Address");
		return;
	}
*/
	struct display_buffer dspbuf = { sizeof(ipstring), ipstring, ipstring };
	display_sockip(&dspbuf, addr);

	ganesha_grpc_server.setIpAddress(ipstring);
	ganesha_grpc_server.setIpFamily(addr->ss_family);
	// Start gRPC server
	ganesha_grpc_server.start(port, server_crt, server_key, ca_crt);

	initialized = true;
}

} /* extern C */
