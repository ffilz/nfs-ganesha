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
#include "GrpcServerInit.h"
#include "GrpcServer.h"
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

GrpcServer ganesha_grpc_server;

/* stop gRPC server */
GrpcServer::~GrpcServer()
{
	stop();
}

static std::shared_ptr<grpc::ServerCredentials> make_server_credentials(
	grpc_credentials_type_t cred_type, const std::string &server_address,
	std::string server_crt, std::string server_key, std::string ca_crt)
{
	if (cred_type == GRPC_CRED_LOCAL) {
		LogInfo(COMPONENT_GRPC,
			"Starting gRPC server with LocalServerCredentials on %s",
			server_address.c_str());
		return grpc::experimental::LocalServerCredentials(LOCAL_TCP);
	}

	LogInfo(COMPONENT_GRPC,
		"Starting gRPC server with SSL/mTLS credentials on %s",
		server_address.c_str());
	grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert_pair = {
		std::move(server_key), std::move(server_crt)
	};

	grpc::SslServerCredentialsOptions ssl_opts;
	ssl_opts.pem_key_cert_pairs.push_back(std::move(key_cert_pair));

	/* To validate client certificates */
	ssl_opts.pem_root_certs = std::move(ca_crt);
	ssl_opts.client_certificate_request =
		GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

	return grpc::SslServerCredentials(ssl_opts);
}

/* start gRPC server */
void GrpcServer::start(uint16_t port, grpc_credentials_type_t cred_type,
		       std::string server_crt, std::string server_key,
		       std::string ca_crt, std::string ip_addr,
		       uint16_t ip_family)
{
	std::call_once(start_once_, [this, port, cred_type,
				     server_crt = std::move(server_crt),
				     server_key = std::move(server_key),
				     ca_crt = std::move(ca_crt),
				     ip_addr = std::move(ip_addr),
				     ip_family]() {
		server_thread_ = std::thread(
			[this, port, cred_type,
			 server_crt = std::move(server_crt),
			 server_key = std::move(server_key),
			 ca_crt = std::move(ca_crt),
			 ip_addr = std::move(ip_addr), ip_family]() {
				gRPCServerStart(port, cred_type,
						std::move(server_crt),
						std::move(server_key),
						std::move(ca_crt),
						std::move(ip_addr), ip_family);
			});
	});
}

/* gRPC Server thread function */
void GrpcServer::gRPCServerStart(uint16_t port,
				 grpc_credentials_type_t cred_type,
				 std::string server_crt, std::string server_key,
				 std::string ca_crt, std::string ip_addr,
				 uint16_t ip_family)
{
	{ /* Taking a lock */
		const std::lock_guard<std::mutex> lock(mutex_);

		/* If default address is provided and
		** v6 ip is allowed than the sever will
		** automatically accept IPv6 connections
		** default port number is 50051
        */

		if (server_) {
			LogDebug(COMPONENT_GRPC,
				 "gRPC server is already running");
			return;
		}

		if (ip_family == AF_INET6) {
			server_address_ =
				"[" + ip_addr + "]:" + std::to_string(port);
		} else {
			server_address_ = ip_addr + ":" + std::to_string(port);
		}

		std::shared_ptr<grpc::ServerCredentials> server_creds =
			make_server_credentials(cred_type, server_address_,
						std::move(server_crt),
						std::move(server_key),
						std::move(ca_crt));

		grpc::ServerBuilder builder;
		/* Adding the listening port */
		builder.AddListeningPort(server_address_, server_creds);

		/* Register the service with the builder */
		builder.RegisterService(&showClientService);

		builder.RegisterService(&nfsIngrace);

		builder.RegisterService(&getClientSessionIds);

		builder.RegisterService(&fakeRecallService);

		builder.RegisterService(&startNfsGrace);

		builder.RegisterService(&clientStatsService);

		builder.RegisterService(&clientMgrService);

		builder.RegisterService(&clusterMembersService);

		builder.RegisterService(&nfsAdmin);

		builder.RegisterService(&exportStatsService);

		builder.RegisterService(&exportService);

		builder.RegisterService(&cachemgr);

		builder.RegisterService(&qosMgrService);

		builder.RegisterService(&nfsMetricsService);

		/* Reflection Service to enable grpc CLI */
		grpc::reflection::InitProtoReflectionServerBuilderPlugin();

		/* Start the server */
		server_ = builder.BuildAndStart();

		if (!server_) {
			LogFatal(COMPONENT_GRPC,
				 "Failed to start gRPC server on %s",
				 server_address_.c_str());
		}
	}

	server_->Wait();
}

/* Stop the gRPC server */
void GrpcServer::stop()
{
	{
		const std::lock_guard<std::mutex> lock(mutex_);

		if (server_) {
			server_->Shutdown();
			server_->Wait();
			server_.reset();
		}
	}

	if (server_thread_.joinable()) {
		/* Wait for the server thread to finish */
		server_thread_.join();
	}
}

/* Check whether addr is 127.0.0.1, ::1 or ::ffff:127.0.0.1.
 */
static bool grpc_addr_is_loopback(const sockaddr_t *addr)
{
	if (addr->ss_family == AF_INET) {
		const struct sockaddr_in *sin =
			reinterpret_cast<const struct sockaddr_in *>(addr);

		return ntohl(sin->sin_addr.s_addr) == INADDR_LOOPBACK;
	}

	if (addr->ss_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 =
			reinterpret_cast<const struct sockaddr_in6 *>(addr);

		if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
			return true;

		return IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr) &&
		       ntohl(sin6->sin6_addr.s6_addr32[3]) == INADDR_LOOPBACK;
	}

	return false;
}

static std::string format_listen_ip(grpc_credentials_type_t cred_type,
				    sockaddr_t *addr)
{
	char ipstring[SOCK_NAME_MAX] = "\0";
	struct display_buffer dspbuf = { sizeof(ipstring), ipstring, ipstring };

	display_sockip(&dspbuf, addr);
	if (cred_type != GRPC_CRED_LOCAL || grpc_addr_is_loopback(addr))
		return ipstring;

	/* Local credentials only accept connections to 127.0.0.1 or ::1, so
	 * replace a wildcard Grpc_Addr with loopback and reject anything else.
	 */
	if (is_inaddrany(addr)) {
		const char *const loopback =
			addr->ss_family == AF_INET6 ? "::1" : "127.0.0.1";
		LogInfo(COMPONENT_GRPC,
			"Grpc_Credentials = local, listening on %s instead of the wildcard address",
			loopback);
		return loopback;
	}

	LogFatal(
		COMPONENT_GRPC,
		"Grpc_Credentials = local requires Grpc_Addr 127.0.0.1 or ::1, got %s",
		ipstring);
}

extern "C" {

/* The event is triggered when NFS is initialized */
void grpc__init(uint16_t port, grpc_credentials_type_t cred_type,
		char *server_crt, char *server_key, char *ca_crt,
		sockaddr_t *addr)
{
	static bool initialized = false;
	if (initialized)
		return;

	std::string ip_addr = format_listen_ip(cred_type, addr);
	if (cred_type == GRPC_CRED_LOCAL) {
		ganesha_grpc_server.start(port, cred_type, "", "", "",
					  std::move(ip_addr), addr->ss_family);
		initialized = true;
		return;
	}

	const char *const crt_path = server_crt ? server_crt : "none";
	const char *const key_path = server_key ? server_key : "none";
	const char *const ca_path = ca_crt ? ca_crt : "none";
	LogDebug(
		COMPONENT_GRPC,
		"Path to server certificate: %s Path to server key : %s Path to ca certificate : %s",
		crt_path, key_path, ca_path);

	std::string key = read_cert_file(server_key ? server_key : "");
	std::string cert = read_cert_file(server_crt ? server_crt : "");
	std::string ca = read_cert_file(ca_crt ? ca_crt : "");
	/* gRPC cannot run securely without the server key, the server
	 * certificate and the CA certificate, so do not start it.
	 */
	if (key.empty() || cert.empty() || ca.empty()) {
		LogWarn(COMPONENT_GRPC,
			"Failed to read gRPC server key (%s), server certificate (%s) or CA certificate (%s), gRPC server not started",
			key_path, crt_path, ca_path);
		return;
	}

	ganesha_grpc_server.start(port, cred_type, std::move(cert),
				  std::move(key), std::move(ca),
				  std::move(ip_addr), addr->ss_family);
	initialized = true;
}

/* Shutdown gRPC */
void grpc__shutdown()
{
	ganesha_grpc_server.stop();
}

} /* extern C */
