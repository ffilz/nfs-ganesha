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

		std::string server_address =
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

	std::unique_ptr< ::NfsMetricsService> service_;
	std::unique_ptr< ::grpc::Server> server_;
	std::shared_ptr< ::grpc::Channel> channel_;
	std::unique_ptr<NfsMetricsService::Stub> stub_;
};

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsOkStatus)
{
	::grpc::ClientContext context;
	CollectMetricsRequest request;
	CollectMetricsResponse response;

	::grpc::Status status =
		stub_->CollectMetrics(&context, request, &response);
#ifdef USE_MONITORING
	EXPECT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
#else
	EXPECT_EQ(status.error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
#endif
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsRegisteredMetrics)
{
#ifdef USE_MONITORING
	metric_metadata_t metadata = {
		.description = "Test Counter Description",
		.unit = METRIC_UNIT_NONE,
	};
	metric_label_t label = {
		.key = "test_key",
		.value = "test_val",
	};
	counter_metric_handle_t counter =
		monitoring__register_counter("ganesha_test_counter", metadata,
					     &label, 1);
	monitoring__counter_inc(counter, 42);

	gauge_metric_handle_t gauge =
		monitoring__register_gauge("ganesha_test_gauge", metadata,
					   &label, 1);
	monitoring__gauge_set(gauge, 99);

	::grpc::ClientContext context;
	CollectMetricsRequest request;
	CollectMetricsResponse response;

	::grpc::Status status =
		stub_->CollectMetrics(&context, request, &response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	bool found_counter = false;
	bool found_gauge = false;
	for (const auto &metric_set : response.metric_value_sets()) {
		if (metric_set.metric_name() == "ganesha_test_counter") {
			found_counter = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_EQ(val.int64_value(), 42);
			EXPECT_EQ(val.labels().at("test_key"), "test_val");
			EXPECT_GT(val.start_time_unix_nanos(), 0);
			EXPECT_GT(val.end_time_unix_nanos(), 0);
			EXPECT_GE(val.end_time_unix_nanos(),
				  val.start_time_unix_nanos());
		} else if (metric_set.metric_name() == "ganesha_test_gauge") {
			found_gauge = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_EQ(val.int64_value(), 99);
			EXPECT_EQ(val.labels().at("test_key"), "test_val");
			EXPECT_GT(val.end_time_unix_nanos(), 0);
		}
	}
	EXPECT_TRUE(found_counter) << "Did not find ganesha_test_counter";
	EXPECT_TRUE(found_gauge) << "Did not find ganesha_test_gauge";
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsHistogramMetric)
{
#ifdef USE_MONITORING
	metric_metadata_t metadata = {
		.description = "Test Histogram Description",
		.unit = METRIC_UNIT_NONE,
	};
	metric_label_t label = {
		.key = "test_key",
		.value = "test_val",
	};
	const int64_t bucket_bounds[] = { 10, 20, 30 };
	histogram_buckets_t buckets = {
		.buckets = bucket_bounds,
		.count = sizeof(bucket_bounds) / sizeof(*bucket_bounds),
	};
	histogram_metric_handle_t hist =
		monitoring__register_histogram("ganesha_test_histogram",
					       metadata, &label, 1, buckets);
	monitoring__histogram_observe(hist, 15);
	monitoring__histogram_observe(hist, 25);

	::grpc::ClientContext context;
	CollectMetricsRequest request;
	CollectMetricsResponse response;

	::grpc::Status status =
		stub_->CollectMetrics(&context, request, &response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	bool found_histogram = false;
	for (const auto &metric_set : response.metric_value_sets()) {
		if (metric_set.metric_name() == "ganesha_test_histogram") {
			found_histogram = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_EQ(val.labels().at("test_key"), "test_val");
			EXPECT_GT(val.start_time_unix_nanos(), 0);
			EXPECT_GT(val.end_time_unix_nanos(), 0);
			EXPECT_GE(val.end_time_unix_nanos(),
				  val.start_time_unix_nanos());
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
		}
	}
	EXPECT_TRUE(found_histogram) << "Did not find ganesha_test_histogram";
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsHistogramWithOverflow)
{
#ifdef USE_MONITORING
	metric_metadata_t metadata = {
		.description = "Test Histogram Overflow Description",
		.unit = METRIC_UNIT_NONE,
	};
	metric_label_t label = {
		.key = "test_key",
		.value = "test_val",
	};
	const int64_t bucket_bounds[] = { 10, 20, 30 };
	histogram_buckets_t buckets = {
		.buckets = bucket_bounds,
		.count = sizeof(bucket_bounds) / sizeof(*bucket_bounds),
	};
	histogram_metric_handle_t hist = monitoring__register_histogram(
		"ganesha_test_histogram_overflow", metadata, &label, 1,
		buckets);
	monitoring__histogram_observe(hist, 15);
	monitoring__histogram_observe(hist, 25);
	monitoring__histogram_observe(hist, 50);

	::grpc::ClientContext context;
	CollectMetricsRequest request;
	CollectMetricsResponse response;

	::grpc::Status status =
		stub_->CollectMetrics(&context, request, &response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	bool found_histogram = false;
	for (const auto &metric_set : response.metric_value_sets()) {
		if (metric_set.metric_name() ==
		    "ganesha_test_histogram_overflow") {
			found_histogram = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_EQ(val.labels().at("test_key"), "test_val");
			EXPECT_GT(val.start_time_unix_nanos(), 0);
			EXPECT_GT(val.end_time_unix_nanos(), 0);
			EXPECT_GE(val.end_time_unix_nanos(),
				  val.start_time_unix_nanos());
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
		}
	}
	EXPECT_TRUE(found_histogram)
		<< "Did not find ganesha_test_histogram_overflow";
#endif /* USE_MONITORING */
}

TEST_F(NfsMetricsServiceTest, CollectMetricsReturnsEmptyHistogram)
{
#ifdef USE_MONITORING
	metric_metadata_t metadata = {
		.description = "Test Empty Histogram Description",
		.unit = METRIC_UNIT_NONE,
	};
	metric_label_t label = {
		.key = "test_key",
		.value = "test_val",
	};
	const int64_t bucket_bounds[] = { 10, 20 };
	histogram_buckets_t buckets = {
		.buckets = bucket_bounds,
		.count = sizeof(bucket_bounds) / sizeof(*bucket_bounds),
	};
	monitoring__register_histogram("ganesha_test_histogram_empty", metadata,
				       &label, 1, buckets);

	::grpc::ClientContext context;
	CollectMetricsRequest request;
	CollectMetricsResponse response;

	::grpc::Status status =
		stub_->CollectMetrics(&context, request, &response);
	ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();

	bool found_histogram = false;
	for (const auto &metric_set : response.metric_value_sets()) {
		if (metric_set.metric_name() ==
		    "ganesha_test_histogram_empty") {
			found_histogram = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_GT(val.start_time_unix_nanos(), 0);
			EXPECT_GT(val.end_time_unix_nanos(), 0);
			EXPECT_GE(val.end_time_unix_nanos(),
				  val.start_time_unix_nanos());
			ASSERT_TRUE(val.has_distribution_value());
			const auto &dist = val.distribution_value();
			EXPECT_EQ(dist.count(), 0);
			EXPECT_DOUBLE_EQ(dist.mean(), 0.0);
		}
	}
	EXPECT_TRUE(found_histogram)
		<< "Did not find ganesha_test_histogram_empty";
#endif /* USE_MONITORING */
}

} // namespace
