/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "nfsMetricsService.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "nfsMetricsService.grpc.pb.h"
#include "nfsMetricsService.pb.h"

#include "monitoring.h"
#include "log.h"
#include "prometheus/client_metric.h"
#include "prometheus/metric.h"
#include "prometheus/metric_family.h"
#include "prometheus/registry.h"

namespace
{

ganesha::metrics::Distribution
convert_to_distribution(const prometheus::ClientMetric::Histogram &histogram)
{
	ganesha::metrics::Distribution distribution;

	distribution.set_count(histogram.sample_count);
	distribution.set_mean(histogram.sample_count == 0
				      ? 0.0
				      : histogram.sample_sum /
						histogram.sample_count);

	auto *explicit_buckets = distribution.mutable_explicit_buckets();
	distribution.mutable_bucket_counts()->Reserve(histogram.bucket.size());
	explicit_buckets->mutable_bounds()->Reserve(histogram.bucket.size());

	int64_t previous_cumulative_count = 0;
	for (const auto &bucket : histogram.bucket) {
		const int64_t count =
			bucket.cumulative_count - previous_cumulative_count;
		distribution.add_bucket_counts(count);
		previous_cumulative_count = bucket.cumulative_count;

		if (bucket.upper_bound !=
		    std::numeric_limits<double>::infinity()) {
			explicit_buckets->add_bounds(bucket.upper_bound);
		}
	}

	return distribution;
}

bool format_metric_value(const prometheus::ClientMetric &metric,
			 prometheus::Metric::Type metric_type,
			 int64_t service_start_time_unix_nanos,
			 int64_t collection_time_unix_nanos,
			 ganesha::metrics::MetricValue *metric_value)
{
	switch (metric_type) {
	case prometheus::Metric::Type::Counter:
		metric_value->set_int64_value(
			static_cast<int64_t>(metric.counter.value));
		metric_value->set_start_time_unix_nanos(
			service_start_time_unix_nanos);
		metric_value->set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Gauge:
		metric_value->set_int64_value(
			static_cast<int64_t>(metric.gauge.value));
		metric_value->set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Histogram:
		*metric_value->mutable_distribution_value() =
			convert_to_distribution(metric.histogram);
		metric_value->set_start_time_unix_nanos(
			service_start_time_unix_nanos);
		metric_value->set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Summary:
	case prometheus::Metric::Type::Untyped:
		return false;
	}

	for (const auto &label : metric.label) {
		(*metric_value->mutable_labels())[label.name] = label.value;
	}
	return true;
}

bool format_metric_family(const prometheus::MetricFamily &metric_family,
			  int64_t service_start_time_unix_nanos,
			  int64_t collection_time_unix_nanos,
			  ganesha::metrics::MetricValueSet *metric_value_set)
{
	metric_value_set->set_metric_name(metric_family.name);

	for (const auto &metric : metric_family.metric) {
		ganesha::metrics::MetricValue value;
		if (format_metric_value(metric, metric_family.type,
					service_start_time_unix_nanos,
					collection_time_unix_nanos, &value)) {
			*metric_value_set->add_metric_values() =
				std::move(value);
		}
	}

	return true;
}

} /* namespace */

NfsMetricsService::NfsMetricsService()
	: service_start_time_unix_nanos_(
		  std::chrono::duration_cast<std::chrono::nanoseconds>(
			  std::chrono::system_clock::now().time_since_epoch())
			  .count())
{
}

::grpc::ServerUnaryReactor *NfsMetricsService::CollectMetrics(
	::grpc::CallbackServerContext *context,
	const ::ganesha::metrics::CollectMetricsRequest *request,
	::ganesha::metrics::CollectMetricsResponse *response)
{
	auto *reactor = context->DefaultReactor();

#ifdef USE_MONITORING
	prometheus_registry_handle_t registry_handle =
		monitoring__get_registry_handle();
	if (registry_handle.registry == nullptr) {
		LogWarn(COMPONENT_GRPC,
			"Ganesha Prometheus registry handle is null");
		reactor->Finish(::grpc::Status(
			::grpc::StatusCode::UNAVAILABLE,
			"Prometheus registry is not initialized"));
		return reactor;
	}

	const int64_t collection_time_unix_nanos =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count();

	auto *registry =
		static_cast<prometheus::Registry *>(registry_handle.registry);
	std::vector<prometheus::MetricFamily> metric_families =
		registry->Collect();

	for (const auto &metric_family : metric_families) {
		ganesha::metrics::MetricValueSet metric_value_set;
		if (!format_metric_family(metric_family,
					  service_start_time_unix_nanos_,
					  collection_time_unix_nanos,
					  &metric_value_set)) {
			LogDebug(COMPONENT_GRPC,
				 "Failed to format metric family %s",
				 metric_family.name.c_str());
			continue;
		}

		if (!metric_value_set.metric_values().empty() ||
		    request->include_empty_metrics()) {
			*response->add_metric_value_sets() =
				std::move(metric_value_set);
		}
	}

	LogDebug(COMPONENT_GRPC, "CollectMetrics reported %d metric value sets",
		 response->metric_value_sets_size());

	reactor->Finish(::grpc::Status::OK);
	return reactor;
#else
	reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
				       "Monitoring is disabled in this build"));
	return reactor;
#endif /* USE_MONITORING */
}
