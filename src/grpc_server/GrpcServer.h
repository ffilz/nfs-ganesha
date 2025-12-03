/* SPDX-License-Identifier: LGPL-3.0-or-later
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

#endif /* GANESHA_GRPC_H */
