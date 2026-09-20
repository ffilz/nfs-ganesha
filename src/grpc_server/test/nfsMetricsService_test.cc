/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "nfsMetricsService.h"

#include <memory>
#include <string>

#include "gtest/gtest.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include "monitoring.h"
#include "nfsMetricsService.grpc.pb.h"
#include "nfsMetricsService.pb.h"

namespace
{

using ::ganesha::metrics::CollectMetricsRequest;
using ::ganesha::metrics::CollectMetricsResponse;
using ::ganesha::metrics::NfsMetricsService;

#ifdef USE_MONITORING
constexpr metric_metadata_t kTestMetadata = {
	.description = "Test Metric Description",
	.unit = METRIC_UNIT_NONE,
};
constexpr metric_label_t kTestLabel = {
	.key = "test_key",
	.value = "test_val",
};
constexpr metric_label_t kOpALabel = { .key = "op", .value = "a" };
constexpr metric_label_t kOpBLabel = { .key = "op", .value = "b" };
constexpr int64_t kThreeBucketBounds[] = { 10, 20, 30 };
constexpr histogram_buckets_t kThreeBuckets = {
	.buckets = kThreeBucketBounds,
	.count = sizeof(kThreeBucketBounds) / sizeof(*kThreeBucketBounds),
};
constexpr int64_t kTwoBucketBounds[] = { 10, 20 };
constexpr histogram_buckets_t kTwoBuckets = {
	.buckets = kTwoBucketBounds,
	.count = sizeof(kTwoBucketBounds) / sizeof(*kTwoBucketBounds),
};

/* Finds the MetricValueSet named name in response, or returns nullptr. */
const ::ganesha::metrics::MetricValueSet *find_metric_value_set(
	const CollectMetricsResponse &response, const std::string &name)
{
	for (const auto &metric_set : response.metric_value_sets()) {
		if (metric_set.metric_name() == name)
			return &metric_set;
	}
	return nullptr;
}
#endif /* USE_MONITORING */

class NfsMetricsServiceTest : public testing::Test {
    protected:
	void SetUp() override
	{
		service_ = std::make_unique< ::NfsMetricsService>();

		::grpc::ServerBuilder builder;
		int port = 0;
		builder.AddListeningPort(
			"localhost:0",
			::grpc::experimental::LocalServerCredentials(LOCAL_TCP),
			&port);
		builder.RegisterService(service_.get());
		server_ = builder.BuildAndStart();
		ASSERT_NE(server_, nullptr);

		const std::string server_address =
			std::string("localhost:") + std::to_string(port);
		channel_ = ::grpc::CreateChannel(
			server_address,
			::grpc::experimental::LocalCredentials(LOCAL_TCP));
		stub_ = NfsMetricsService::NewStub(channel_);
	}

	void TearDown() override
	{
		if (server_) {
			server_->Shutdown();
		}
	}

	::grpc::Status collect_metrics(CollectMetricsResponse *response,
				       bool include_empty_metrics = false)
	{
		::grpc::ClientContext context;
		CollectMetricsRequest request;
		request.set_include_empty_metrics(include_empty_metrics);
		return stub_->CollectMetrics(&context, request, response);
	}

	std::unique_ptr< ::NfsMetricsService> service_;
	std::unique_ptr< ::grpc::Server> server_;
	std::shared_ptr< ::grpc::Channel> channel_;
	std::unique_ptr<NfsMetricsService::Stub> stub_;
};

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsOkStatus)
{
	CollectMetricsResponse response;

	const ::grpc::Status status = collect_metrics(&response);
#ifdef USE_MONITORING
	EXPECT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
#else
	EXPECT_EQ(status.error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
#endif
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsRegisteredMetrics)
{
#ifdef USE_MONITORING
	const counter_metric_handle_t counter =
		monitoring__register_counter("ganesha_test_counter",
					     kTestMetadata, &kTestLabel, 1);
	monitoring__counter_inc(counter, 42);
	const gauge_metric_handle_t gauge =
		monitoring__register_gauge("ganesha_test_gauge", kTestMetadata,
					   &kTestLabel, 1);
	monitoring__gauge_set(gauge, 99);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const counter_set =
		find_metric_value_set(response, "ganesha_test_counter");
	ASSERT_NE(counter_set, nullptr);
	ASSERT_EQ(counter_set->metric_values_size(), 1);
	const auto &counter_val = counter_set->metric_values(0);
	EXPECT_EQ(counter_val.int64_value(), 42);
	EXPECT_EQ(counter_val.labels().at("test_key"), "test_val");
	EXPECT_GT(counter_val.start_time_unix_nanos(), 0);
	EXPECT_GT(counter_val.end_time_unix_nanos(), 0);
	EXPECT_GE(counter_val.end_time_unix_nanos(),
		  counter_val.start_time_unix_nanos());

	const auto *const gauge_set =
		find_metric_value_set(response, "ganesha_test_gauge");
	ASSERT_NE(gauge_set, nullptr);
	ASSERT_EQ(gauge_set->metric_values_size(), 1);
	const auto &gauge_val = gauge_set->metric_values(0);
	EXPECT_EQ(gauge_val.int64_value(), 99);
	EXPECT_EQ(gauge_val.labels().at("test_key"), "test_val");
	EXPECT_EQ(gauge_val.start_time_unix_nanos(), 0);
	EXPECT_GT(gauge_val.end_time_unix_nanos(), 0);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsHistogramMetric)
{
#ifdef USE_MONITORING
	const histogram_metric_handle_t hist =
		monitoring__register_histogram("ganesha_test_histogram",
					       kTestMetadata, &kTestLabel, 1,
					       kThreeBuckets);
	monitoring__histogram_observe(hist, 15);
	monitoring__histogram_observe(hist, 25);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const histogram_set =
		find_metric_value_set(response, "ganesha_test_histogram");
	ASSERT_NE(histogram_set, nullptr);
	ASSERT_EQ(histogram_set->metric_values_size(), 1);
	const auto &val = histogram_set->metric_values(0);
	EXPECT_EQ(val.labels().at("test_key"), "test_val");
	EXPECT_GT(val.start_time_unix_nanos(), 0);
	EXPECT_GT(val.end_time_unix_nanos(), 0);
	EXPECT_GE(val.end_time_unix_nanos(), val.start_time_unix_nanos());
	ASSERT_TRUE(val.has_distribution_value());
	const auto &dist = val.distribution_value();
	EXPECT_EQ(dist.count(), 2);
	EXPECT_DOUBLE_EQ(dist.mean(), 20.0);
	ASSERT_EQ(dist.explicit_buckets().bounds_size(), 3);
	EXPECT_EQ(dist.explicit_buckets().bounds(0), 10);
	EXPECT_EQ(dist.explicit_buckets().bounds(1), 20);
	EXPECT_EQ(dist.explicit_buckets().bounds(2), 30);
	ASSERT_EQ(dist.bucket_counts_size(), 4);
	EXPECT_EQ(dist.bucket_counts(0), 0);
	EXPECT_EQ(dist.bucket_counts(1), 1);
	EXPECT_EQ(dist.bucket_counts(2), 1);
	EXPECT_EQ(dist.bucket_counts(3), 0);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsHistogramWithOverflow)
{
#ifdef USE_MONITORING
	const histogram_metric_handle_t hist = monitoring__register_histogram(
		"ganesha_test_histogram_overflow", kTestMetadata, &kTestLabel,
		1, kThreeBuckets);
	monitoring__histogram_observe(hist, 15);
	monitoring__histogram_observe(hist, 25);
	monitoring__histogram_observe(hist, 50);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const histogram_set =
		find_metric_value_set(response,
				      "ganesha_test_histogram_overflow");
	ASSERT_NE(histogram_set, nullptr);
	ASSERT_EQ(histogram_set->metric_values_size(), 1);
	const auto &val = histogram_set->metric_values(0);
	EXPECT_EQ(val.labels().at("test_key"), "test_val");
	EXPECT_GT(val.start_time_unix_nanos(), 0);
	EXPECT_GT(val.end_time_unix_nanos(), 0);
	EXPECT_GE(val.end_time_unix_nanos(), val.start_time_unix_nanos());
	ASSERT_TRUE(val.has_distribution_value());
	const auto &dist = val.distribution_value();
	EXPECT_EQ(dist.count(), 3);
	EXPECT_DOUBLE_EQ(dist.mean(), 30.0);
	ASSERT_EQ(dist.explicit_buckets().bounds_size(), 3);
	EXPECT_EQ(dist.explicit_buckets().bounds(0), 10);
	EXPECT_EQ(dist.explicit_buckets().bounds(1), 20);
	EXPECT_EQ(dist.explicit_buckets().bounds(2), 30);
	ASSERT_EQ(dist.bucket_counts_size(), 4);
	EXPECT_EQ(dist.bucket_counts(0), 0);
	EXPECT_EQ(dist.bucket_counts(1), 1);
	EXPECT_EQ(dist.bucket_counts(2), 1);
	EXPECT_EQ(dist.bucket_counts(3), 1);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsEmptyHistogram)
{
#ifdef USE_MONITORING
	monitoring__register_histogram("ganesha_test_histogram_empty",
				       kTestMetadata, &kTestLabel, 1,
				       kTwoBuckets);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const histogram_set =
		find_metric_value_set(response, "ganesha_test_histogram_empty");
	ASSERT_NE(histogram_set, nullptr);
	ASSERT_EQ(histogram_set->metric_values_size(), 1);
	const auto &val = histogram_set->metric_values(0);
	EXPECT_GT(val.start_time_unix_nanos(), 0);
	EXPECT_GT(val.end_time_unix_nanos(), 0);
	EXPECT_GE(val.end_time_unix_nanos(), val.start_time_unix_nanos());
	ASSERT_TRUE(val.has_distribution_value());
	const auto &dist = val.distribution_value();
	EXPECT_EQ(dist.count(), 0);
	EXPECT_DOUBLE_EQ(dist.mean(), 0.0);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsOmitsEmptySeries)
{
#ifdef USE_MONITORING
	const counter_metric_handle_t counter_a =
		monitoring__register_counter("ganesha_test_compact_counter",
					     kTestMetadata, &kOpALabel, 1);
	monitoring__register_counter("ganesha_test_compact_counter",
				     kTestMetadata, &kOpBLabel, 1);
	monitoring__counter_inc(counter_a, 1);
	const histogram_metric_handle_t hist_a =
		monitoring__register_histogram("ganesha_test_compact_histogram",
					       kTestMetadata, &kOpALabel, 1,
					       kTwoBuckets);
	monitoring__register_histogram("ganesha_test_compact_histogram",
				       kTestMetadata, &kOpBLabel, 1,
				       kTwoBuckets);
	monitoring__histogram_observe(hist_a, 15);
	/* Gauges at zero are values, not empty series. */
	const gauge_metric_handle_t gauge_a =
		monitoring__register_gauge("ganesha_test_compact_gauge",
					   kTestMetadata, &kOpALabel, 1);
	monitoring__register_gauge("ganesha_test_compact_gauge", kTestMetadata,
				   &kOpBLabel, 1);
	monitoring__gauge_set(gauge_a, 5);

	CollectMetricsResponse response;
	ASSERT_TRUE(collect_metrics(&response).ok());

	const auto *const counter_set =
		find_metric_value_set(response, "ganesha_test_compact_counter");
	ASSERT_NE(counter_set, nullptr);
	ASSERT_EQ(counter_set->metric_values_size(), 1);
	EXPECT_EQ(counter_set->metric_values(0).labels().at("op"), "a");
	EXPECT_EQ(counter_set->metric_values(0).int64_value(), 1);
	const auto *const hist_set =
		find_metric_value_set(response,
				      "ganesha_test_compact_histogram");
	ASSERT_NE(hist_set, nullptr);
	ASSERT_EQ(hist_set->metric_values_size(), 1);
	EXPECT_EQ(hist_set->metric_values(0).labels().at("op"), "a");
	EXPECT_EQ(hist_set->metric_values(0).distribution_value().count(), 1);
	const auto *const gauge_set =
		find_metric_value_set(response, "ganesha_test_compact_gauge");
	ASSERT_NE(gauge_set, nullptr);
	EXPECT_EQ(gauge_set->metric_values_size(), 2);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsKeepsOneSeriesOfEmptyMetric)
{
#ifdef USE_MONITORING
	monitoring__register_counter("ganesha_test_all_empty_counter",
				     kTestMetadata, &kOpALabel, 1);
	monitoring__register_counter("ganesha_test_all_empty_counter",
				     kTestMetadata, &kOpBLabel, 1);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const counter_set =
		find_metric_value_set(response,
				      "ganesha_test_all_empty_counter");
	ASSERT_NE(counter_set, nullptr);
	ASSERT_EQ(counter_set->metric_values_size(), 1);
	EXPECT_EQ(counter_set->metric_values(0).int64_value(), 0);
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsIncludeEmptyMetricsReturnsAll)
{
#ifdef USE_MONITORING
	const counter_metric_handle_t counter_a = monitoring__register_counter(
		"ganesha_test_include_empty_counter", kTestMetadata, &kOpALabel,
		1);
	monitoring__register_counter("ganesha_test_include_empty_counter",
				     kTestMetadata, &kOpBLabel, 1);
	monitoring__counter_inc(counter_a, 1);

	CollectMetricsResponse response;
	const ::grpc::Status status = collect_metrics(&response, true);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	const auto *const counter_set =
		find_metric_value_set(response,
				      "ganesha_test_include_empty_counter");
	ASSERT_NE(counter_set, nullptr);
	EXPECT_EQ(counter_set->metric_values_size(), 2);
#endif /* USE_MONITORING */
}

} /* namespace */
