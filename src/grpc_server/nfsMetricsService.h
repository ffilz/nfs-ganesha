/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef NFS_METRICS_SERVICE_H
#define NFS_METRICS_SERVICE_H

#include <cstdint>

#include <grpcpp/server_context.h>
#include "nfsMetricsService.grpc.pb.h"
#include "nfsMetricsService.pb.h"

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

	const int64_t service_start_time_unix_nanos_;
};

#endif /* NFS_METRICS_SERVICE_H */
