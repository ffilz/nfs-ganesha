/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "nfsMetricsService.h"

#include <memory>
#include <string>

#include <nfsMetricsService.grpc.pb.h>
#include <nfsMetricsService.pb.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include "monitoring.h"
#include "log.h"

extern "C" {
static log_levels_t test_component_log_levels[COMPONENT_COUNT] = { NIV_NULL };
log_levels_t *component_log_level = test_component_log_levels;
log_levels_t *conditional_component_log_level = test_component_log_levels;
bool conditional_logging_configured = false;
bool is_op_context_conditional_flag_set(void)
{
	return false;
}
void DisplayLogComponentLevel(log_components_t component, const char *file,
			      int line, const char *function,
			      log_levels_t level, const char *format, ...)
{
}
}

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
	EXPECT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
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
		} else if (metric_set.metric_name() == "ganesha_test_gauge") {
			found_gauge = true;
			ASSERT_EQ(metric_set.metric_values_size(), 1);
			const auto &val = metric_set.metric_values(0);
			EXPECT_EQ(val.int64_value(), 99);
			EXPECT_EQ(val.labels().at("test_key"), "test_val");
		}
	}
	EXPECT_TRUE(found_counter) << "Did not find ganesha_test_counter";
	EXPECT_TRUE(found_gauge) << "Did not find ganesha_test_gauge";
#endif /* USE_MONITORING */
}

} // namespace
