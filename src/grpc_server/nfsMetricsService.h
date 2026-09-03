/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef THIRD_PARTY_NFS_GANESHA_FILESTORE_SRC_GRPC_SERVER_NFS_METRICS_SERVICE_H_
#define THIRD_PARTY_NFS_GANESHA_FILESTORE_SRC_GRPC_SERVER_NFS_METRICS_SERVICE_H_

#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_context.h>
#include <nfsMetricsService.grpc.pb.h>
#include <nfsMetricsService.pb.h>

/*
 * Implementation of ganesha::metrics::NfsMetricsService for NFS Ganesha.
 *
 * Collects Prometheus metrics directly from Ganesha's in-memory Prometheus
 * registry (managed in libntirpc) and formats them into MetricValueSet protos.
 */
class NfsMetricsService final
	: public ::ganesha::metrics::NfsMetricsService::CallbackService {
    public:
	NfsMetricsService();
	~NfsMetricsService() override = default;

    private:
	::grpc::ServerUnaryReactor *CollectMetrics(
		::grpc::CallbackServerContext *context,
		const ::ganesha::metrics::CollectMetricsRequest *request,
		::ganesha::metrics::CollectMetricsResponse *response) override;

	const std::chrono::system_clock::time_point service_start_time_;
};

#endif
