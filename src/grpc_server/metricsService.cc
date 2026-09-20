/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * @brief gRPC service that exports Ganesha's metrics in the MetricValueSet
 * format (google.api.servicecontrol.v1) for Service Control collection agents.
 */
#include "metricsService.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "metricsService.grpc.pb.h"
#include "metricsService.pb.h"

#include "log.h"

#ifdef USE_MONITORING
#include "monitoring.h"
#include "prometheus/client_metric.h"
#include "prometheus/metric.h"
#include "prometheus/metric_family.h"
#include "prometheus/registry.h"
#endif /* USE_MONITORING */

namespace
{

int64_t now_unix_nanos(void)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

#ifdef USE_MONITORING

bool is_supported_metric_type(prometheus::Metric::Type metric_type)
{
	switch (metric_type) {
	case prometheus::Metric::Type::Counter:
	case prometheus::Metric::Type::Gauge:
	case prometheus::Metric::Type::Histogram:
		return true;

	case prometheus::Metric::Type::Summary:
	case prometheus::Metric::Type::Untyped:
		return false;
	}
	return false;
}

/* Prometheus buckets include their upper bound (bounds[i-1], bounds[i]],
 * while Service Control buckets include their lower bound
 * [bounds[i-1], bounds[i]). We copy the bounds as is, so a sample equal to a
 * bound is reported one bucket lower. Count and mean are exact.
 */
ganesha::metrics::Distribution
convert_to_distribution(const prometheus::ClientMetric::Histogram &histogram)
{
	ganesha::metrics::Distribution distribution;

	distribution.set_count(histogram.sample_count);
	distribution.set_mean(histogram.sample_count == 0
				      ? 0.0
				      : histogram.sample_sum /
						histogram.sample_count);

	auto *const explicit_buckets = distribution.mutable_explicit_buckets();
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

/* libntirpc only registers int64_t counters and gauges; prometheus-cpp-lite
 * widens them to double in ClientMetric, so cast back to int64_t here.
 */
ganesha::metrics::MetricValue
format_metric_value(const prometheus::ClientMetric &metric,
		    prometheus::Metric::Type metric_type,
		    int64_t service_start_time_unix_nanos,
		    int64_t collection_time_unix_nanos)
{
	ganesha::metrics::MetricValue metric_value;

	switch (metric_type) {
	case prometheus::Metric::Type::Counter:
		metric_value.set_int64_value(
			static_cast<int64_t>(metric.counter.value));
		metric_value.set_start_time_unix_nanos(
			service_start_time_unix_nanos);
		metric_value.set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Gauge:
		metric_value.set_int64_value(
			static_cast<int64_t>(metric.gauge.value));
		metric_value.set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Histogram:
		*metric_value.mutable_distribution_value() =
			convert_to_distribution(metric.histogram);
		metric_value.set_start_time_unix_nanos(
			service_start_time_unix_nanos);
		metric_value.set_end_time_unix_nanos(
			collection_time_unix_nanos);
		break;
	case prometheus::Metric::Type::Summary:
	case prometheus::Metric::Type::Untyped:
		break;
	}

	for (const auto &label : metric.label) {
		(*metric_value.mutable_labels())[label.name] = label.value;
	}
	return metric_value;
}

/* Whether a series was never updated. Ganesha registers many series up front
 * (e.g. every NFSv4 operation x status), and most of them stay empty.
 */
bool is_metric_empty(prometheus::Metric::Type metric_type,
		     const prometheus::ClientMetric &metric)
{
	switch (metric_type) {
	case prometheus::Metric::Type::Counter:
		return metric.counter.value == 0.0;
	case prometheus::Metric::Type::Summary:
		return metric.summary.sample_count == 0;
	case prometheus::Metric::Type::Histogram:
		return metric.histogram.sample_count == 0;
	default:
		return false;
	}
}

ganesha::metrics::MetricValueSet format_metric_family(
	const prometheus::MetricFamily &metric_family,
	bool include_empty_metrics, int64_t service_start_time_unix_nanos,
	int64_t collection_time_unix_nanos)
{
	ganesha::metrics::MetricValueSet metric_value_set;

	metric_value_set.set_metric_name(metric_family.name);

	for (const auto &metric : metric_family.metric) {
		if (!include_empty_metrics &&
		    is_metric_empty(metric_family.type, metric))
			continue;

		*metric_value_set.add_metric_values() =
			format_metric_value(metric, metric_family.type,
					    service_start_time_unix_nanos,
					    collection_time_unix_nanos);
	}

	/* Like the Prometheus exporter (compact_family), keep one series of a
	 * family whose series are all empty, so that the family stays visible.
	 */
	if (metric_value_set.metric_values().empty() &&
	    !metric_family.metric.empty()) {
		*metric_value_set.add_metric_values() =
			format_metric_value(metric_family.metric.front(),
					    metric_family.type,
					    service_start_time_unix_nanos,
					    collection_time_unix_nanos);
	}

	return metric_value_set;
}

void populate_metric_value_sets(
	const std::vector<prometheus::MetricFamily> &metric_families,
	bool include_empty_metrics, int64_t service_start_time_unix_nanos,
	int64_t collection_time_unix_nanos,
	ganesha::metrics::CollectMetricsResponse *response)
{
	for (const auto &metric_family : metric_families) {
		if (!is_supported_metric_type(metric_family.type))
			continue;

		ganesha::metrics::MetricValueSet metric_value_set =
			format_metric_family(metric_family,
					     include_empty_metrics,
					     service_start_time_unix_nanos,
					     collection_time_unix_nanos);

		if (!metric_value_set.metric_values().empty()) {
			*response->add_metric_value_sets() =
				std::move(metric_value_set);
		}
	}
}

#endif /* USE_MONITORING */

} /* namespace */

MetricsService::MetricsService()
	: service_start_time_unix_nanos_(now_unix_nanos())
{
}

::grpc::ServerUnaryReactor *MetricsService::CollectMetrics(
	::grpc::CallbackServerContext *context,
	const ::ganesha::metrics::CollectMetricsRequest *request,
	::ganesha::metrics::CollectMetricsResponse *response)
{
	auto *const reactor = context->DefaultReactor();
#ifdef USE_MONITORING
	const int64_t collection_time_unix_nanos = now_unix_nanos();
	auto *const registry = static_cast<prometheus::Registry *>(
		monitoring__get_registry_handle().registry);
	populate_metric_value_sets(registry->Collect(),
				   request->include_empty_metrics(),
				   service_start_time_unix_nanos_,
				   collection_time_unix_nanos, response);
	LogDebug(COMPONENT_GRPC, "CollectMetrics reported %d metric value sets",
		 response->metric_value_sets_size());
	reactor->Finish(::grpc::Status::OK);
	return reactor;
#else
	(void)request;
	(void)response;
	reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
				       "Monitoring is disabled in this build"));
	return reactor;
#endif /* USE_MONITORING */
}
