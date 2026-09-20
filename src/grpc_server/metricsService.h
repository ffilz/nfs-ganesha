/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2026 Google LLC
 * Contributor : Inna Batenkov  batenkovinna@google.com
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @brief gRPC service that exports Ganesha's metrics in the MetricValueSet
 * format (google.api.servicecontrol.v1) for Service Control collection agents.
 */
#ifndef METRICS_SERVICE_H
#define METRICS_SERVICE_H

#include <cstdint>

#include <grpcpp/server_context.h>
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
