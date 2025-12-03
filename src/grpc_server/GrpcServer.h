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
 *
 * @brief gRPC library for NFS Ganesha.
 */

#ifndef GRPC_SERVER_H
#define GRPC_SERVER_H

/* gRPC Server */
class GrpcServer {
    public:
	GrpcServer();
	void start(uint16_t port, std::string_view server_crt,
		   std::string_view server_key, std::string_view ca_crt);

	void stop(void);
	void setIpAddress(char *ipAddr);
	void setIpFamily(int Ipfamily);
	~GrpcServer();

	// Delete copy/move constructor/assignment
	GrpcServer(const GrpcServer &) = delete;
	GrpcServer &operator=(const GrpcServer &) = delete;
	GrpcServer(GrpcServer &&) = delete;
	GrpcServer &operator=(GrpcServer &&) = delete;

    private:
	std::mutex mutex_;
	std::unique_ptr<grpc::Server> server_;
	std::thread server_thread_;
	std::string ipAddr_;
	int Ipfamily_;

	void gRPCServerStart(uint16_t port, std::string_view server_crt,
			     std::string_view server_key,
			     std::string_view ca_crt);

} ganesha_grpc_server;

GrpcServer::GrpcServer()
{
}

/* stop gRPC server */
GrpcServer::~GrpcServer()
{
	stop();
}

/* Set IP address */
void GrpcServer::setIpAddress(char *ipAddr)
{
	ipAddr_ = ipAddr;
}

/* Set IP Family */
void GrpcServer::setIpFamily(int Ipfamily)
{
	Ipfamily_ = Ipfamily;
}

#endif /* GANESHA_GRPC_H */
