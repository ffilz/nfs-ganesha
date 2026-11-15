/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Tests for the ordering of GrpcServer::start() and GrpcServer::stop().
 */
#include "GrpcServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

#include <gtest/gtest.h>

namespace
{

constexpr auto stop_deadline = std::chrono::seconds(10);

/* Returns a port on 127.0.0.1 that nothing is bound to right now: bind to
 * port 0 so the kernel picks a free one, then release it.
 */
int pick_unused_port()
{
	struct sockaddr_in addr = {};
	socklen_t len = sizeof(addr);
	const int fd = socket(AF_INET, SOCK_STREAM, 0);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 ||
	    bind(fd, reinterpret_cast<const struct sockaddr *>(&addr),
		 sizeof(addr)) != 0 ||
	    getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) !=
		    0) {
		perror("pick_unused_port");
		abort();
	}
	close(fd);
	return ntohs(addr.sin_port);
}

/* Runs server->stop() and aborts if it does not return within stop_deadline:
 * a hung join() inside stop() cannot be recovered from in-process.
 */
void stop_or_abort(GrpcServer *server, const char *what)
{
	std::promise<void> done;
	std::future<void> stopped = done.get_future();
	std::thread stopper([server, &done]() {
		server->stop();
		done.set_value();
	});

	if (stopped.wait_for(stop_deadline) != std::future_status::ready) {
		fprintf(stderr, "GrpcServer::stop() did not return: %s\n",
			what);
		abort();
	}
	stopper.join();
}

void start_local(GrpcServer *server, int port)
{
	server->start(port, GRPC_CRED_LOCAL, "", "", "", "127.0.0.1", AF_INET);
}

/* Whether something accepts TCP connections on 127.0.0.1:port. */
bool is_listening(int port)
{
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	const bool connected =
		connect(fd, reinterpret_cast<const struct sockaddr *>(&addr),
			sizeof(addr)) == 0;
	close(fd);
	return connected;
}

/* A stop() right after start() must find the server and shut it down, not
 * miss it and then wait forever for it to stop.
 */
TEST(GrpcServerTest, StopRightAfterStartReturns)
{
	GrpcServer server;
	const int port = pick_unused_port();

	start_local(&server, port);
	stop_or_abort(&server, "stop() right after start()");

	EXPECT_FALSE(is_listening(port));
}

/* start() returns with the server listening. stop() while it serves must
 * return and must not free the server while anything still uses it. A
 * use-after-free here shows up when built with AddressSanitizer.
 */
TEST(GrpcServerTest, StopWhileServingReturns)
{
	GrpcServer server;
	const int port = pick_unused_port();

	start_local(&server, port);
	ASSERT_TRUE(is_listening(port));

	stop_or_abort(&server, "stop() while serving");
	EXPECT_FALSE(is_listening(port));
}

} /* namespace */
