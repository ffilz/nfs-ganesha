/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * @brief gRPC service that exports Ganesha's metrics in the MetricValueSet
 * format (google.api.servicecontrol.v1) for Service Control collection agents.
 */
#ifndef METRICS_SERVICE_H
#define METRICS_SERVICE_H

#include <cstdint>

#include "third_party/grpc/include/grpcpp/server_context.h"
#include "metricsService.grpc.pb.h"
#include "metricsService.pb.h"

/*
 * Implementation of ganesha::metrics::MetricsService for NFS Ganesha.
 *
 * Collects Prometheus metrics directly from Ganesha's in-memory Prometheus
 * registry (managed in libntirpc) and formats them into MetricValueSet protos.
 */
class MetricsService final
	: public ::ganesha::metrics::MetricsService::CallbackService {
    public:
	MetricsService();
	~MetricsService() override = default;

    private:
	::grpc::ServerUnaryReactor *CollectMetrics(
		::grpc::CallbackServerContext *context,
		const ::ganesha::metrics::CollectMetricsRequest *request,
		::ganesha::metrics::CollectMetricsResponse *response) override;

	/* Unused when built without USE_MONITORING. */
	[[maybe_unused]] const int64_t service_start_time_unix_nanos_;
};

#endif /* METRICS_SERVICE_H */
