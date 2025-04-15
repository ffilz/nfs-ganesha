/* Copyright (C) 2025, IBM
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
#include "gRPC/GrpcServerInit.h"
#include "GrpcServer.h"
#include "nfsService.h"
#include <grpcpp/ext/proto_server_reflection_plugin.h>

/*
 *     gRPC (gRPC Remote Procedure Call) is an open-source framework that
 *     enables communication between services through remote procedure
 *     calls (RPCs). It is built on top of HTTP/2 and uses Protocol Buffers
 *     (protobuf) as the interface definition language (IDL).
 *     The below infra facilitates intra server as well as inter server
 *     communication.
 */

GrpcServer::GrpcServer()
	: running_(false)
{
}

// stop gRPC server
GrpcServer::~GrpcServer()
{
	stop();
}

// start gRPC server
void GrpcServer::start(uint16_t port, char *server_crt, char *server_key,
		       char *ca_crt)
{
	server_thread_ =
		std::thread([this, port, server_crt, server_key, ca_crt]() {
			gRPCServerStart(port, server_crt, server_key, ca_crt);
		});
}

// gRPC Server thread function
void GrpcServer::gRPCServerStart(uint16_t port, char *server_crt,
				 char *server_key, char *ca_crt)
{
	grpc::ServerBuilder builder;
	GetClientIdService showClientService;
	GetNfsGraceService nfsIngrace;
	GetSessionIdService getClientSessionIds;
	StartNfsGraceService startNfsGrace;

	std::string key = "";
	std::string cert = "";
	std::string ca = "";

	grpc::SslServerCredentialsOptions ssl_opts;
	std::shared_ptr<grpc::ServerCredentials> server_creds;

	{ // Taking a lock
		const std::lock_guard<std::mutex> lock(mutex_);

		// Server will listen on localhost:port_number or
		// 127.0.0.1:port_number
		// default port_number is 50051
		server_address += std::to_string(port);

		LogDebug(COMPONENT_GRPC,
			 "Path to server certificate: %s "
			 "Path to server key : %s"
			 "Path to ca certificate : %s",
			 server_crt, server_key, ca_crt);

		// We do not want to run multiple instances of
		// gRPC server
		if (running_) {
			LogWarn(COMPONENT_GRPC,
				"gRPC server is already running");

			return;
		}

		running_ = true;

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

			running_ = false;

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

		std::cout << "We are here 1 " << std::endl;
		// If CA certificate is not available than we cannot
		// validate client certificates
		if (!ca.empty()) {
			ssl_opts.pem_root_certs = ca;

			ssl_opts.client_certificate_request =
				GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
			std::cout << "We are here 2" << std::endl;
		} else {
			LogInfo(COMPONENT_GRPC,
				"CA certificate was not found; as a result,"
				"the server will not validate client certificates");
		}

		server_creds = grpc::SslServerCredentials(ssl_opts);

		std::cout << "We are here 3 " << std::endl;
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

	std::cout << "We are here 4 " << std::endl;
	// Start the server
	server_ = builder.BuildAndStart();

	if (!server_) {
		std::cout << "We are here 6 " << std::endl;
		LogFatal(COMPONENT_GRPC, "Failed to start server on %s",
			 server_address);
	}

	std::cout << "We are here 5 " << std::endl;
	server_->Wait();
}

// Stop the gRPC server
void GrpcServer::stop()
{
	const std::lock_guard<std::mutex> lock(mutex_);

	if (running_) {
		running_ = false;

		server_->Shutdown();

		if (server_thread_.joinable()) {
			// Wait for the server thread to finish
			server_thread_.join();
		}
	}
}

extern "C" {

// The event is triggered when NFS is initialized
void grpc__init(uint16_t port, char *server_crt, char *server_key, char *ca_crt)
{
	static bool initialized = false;

	if (initialized)
		return;

	// Start gRPC server
	ganesha_grpc_server.start(port, server_crt, server_key, ca_crt);

	initialized = true;
}

} /* extern C */
