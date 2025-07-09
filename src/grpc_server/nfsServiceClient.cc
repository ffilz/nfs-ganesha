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

#include <iostream>
#include <grpcpp/grpcpp.h>
#include <nfsService.pb.h>
#include <nfsService.grpc.pb.h>
#include <string>

void GetClientIds(const std::string &server_address, const std::string &ca_crt,
		  const std::string &client_cert, const std::string &client_key)
{
	grpc::SslCredentialsOptions ssl_opts;

	ssl_opts.pem_root_certs = ganesha_grpc_server.read_file(ca_cert);
	ssl_opts.pem_cert_chain = ganesha_grpc_server.read_file(client_cert);
	ssl_opts.pem_private_key = ganesha_grpc_server.read_file(client_key);

	auto creds = grpc::SslCredentials(ssl_opts);

	// Creating an secure channel to communicate with the server
	std::shared_ptr<grpc::Channel> channel =
		grpc::CreateChannel(server_address, creds);
	std::unique_ptr<nfsService::GetClientId::Stub> stub =
		nfsService::GetClientId::NewStub(channel);

	// Creating a request and response
	nfsService::GetClientIdsRequest request;
	nfsService::GetClientIdsResponse response;
	grpc::ClientContext context;

	// Make the gRPC call
	grpc::Status status = stub->GetClientIds(&context, request, &response);
}

void GetNfsGracePeriod(const std::string &server_address,
		       const std::string &ca_crt,
		       const std::string &client_cert,
		       const std::string &client_key)
{
	grpc::SslCredentialsOptions ssl_opts;

	ssl_opts.pem_root_certs = ganesha_grpc_server.read_file(ca_cert);
	ssl_opts.pem_cert_chain = ganesha_grpc_server.read_file(client_cert);
	ssl_opts.pem_private_key = ganesha_grpc_server.read_file(client_key);

	auto creds = grpc::SslCredentials(ssl_opts);

	// Creating a secure channel to communicate with the server
	std::shared_ptr<grpc::Channel> channel =
		grpc::CreateChannel(server_address, creds);
	std::unique_ptr<nfsService::GetNfsGrace::Stub> stub =
		nfsService::GetNfsGrace::NewStub(channel);

	// Creating a request and response
	nfsService::GetNfsGraceRequest request;
	nfsService::GetNfsGraceResponse response;
	grpc::ClientContext context;

	// Make the gRPC call
	grpc::Status status =
		stub->GetGracePeriod(&context, request, &response);
}

void GetClientSessionIds(const std::string &server_address,
			 const std::string &ca_crt,
			 const std::string &client_cert,
			 const std::string &client_key)
{
	grpc::SslCredentialsOptions ssl_opts;

	ssl_opts.pem_root_certs = ganesha_grpc_server.read_file(ca_cert);
	ssl_opts.pem_cert_chain = ganesha_grpc_server.read_file(client_cert);
	ssl_opts.pem_private_key = ganesha_grpc_server.read_file(client_key);

	auto creds = grpc::SslCredentials(ssl_opts);

	// Creating a secure channel to communicate with the server
	std::shared_ptr<grpc::Channel> channel =
		grpc::CreateChannel(server_address, creds);
	std::unique_ptr<nfsService::GetSessionId::Stub> stub =
		nfsService::GetSessionId::NewStub(channel);

	// Creating a request and response
	nfsService::GetSessionIdsRequest request;
	nfsService::GetSessionIdsResponse response;
	grpc::ClientContext context;

	// Make the gRPC call
	grpc::Status status = stub->GetSessionIds(&context, request, &response);
}

void StartGraceWithEvent(const std::string &server_address)
{
	// Creating a channel to communicate with the server
	std::shared_ptr<grpc::Channel> channel =
		grpc::CreateChannel(server_address,
				    grpc::InsecureChannelCredentials());
	std::unique_ptr<nfsService::StartNfsGrace::Stub> stub =
		nfsService::StartNfsGrace::NewStub(channel);

	// Creating a request and response
	nfsService::GraceWithEvent request;
	nfsService::GraceStatus response;
	grpc::ClientContext context;

	// Make the gRPC call
	grpc::Status status =
		stub->StartGraceWithEvent(&context, request, &response);
}
