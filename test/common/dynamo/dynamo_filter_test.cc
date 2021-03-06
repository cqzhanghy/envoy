#include "common/buffer/buffer_impl.h"
#include "common/dynamo/dynamo_filter.h"
#include "common/http/header_map_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"

using testing::_;
using testing::NiceMock;
using testing::Return;

namespace Dynamo {

class DynamoFilterTest : public testing::Test {
public:
  void setup(bool enabled) {
    ON_CALL(loader_.snapshot_, featureEnabled("dynamodb.filter_enabled", 100))
        .WillByDefault(Return(enabled));
    EXPECT_CALL(loader_.snapshot_, featureEnabled("dynamodb.filter_enabled", 100));

    filter_.reset(new DynamoFilter(loader_, stat_prefix_, stats_));

    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
  }

  std::unique_ptr<DynamoFilter> filter_;
  NiceMock<Runtime::MockLoader> loader_;
  std::string stat_prefix_{"prefix."};
  Stats::MockStore stats_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
};

TEST_F(DynamoFilterTest, operatorPresent) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.Get"}, {"random", "random"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation_missing")).Times(0);
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table_missing"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.Get.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.Get.upstream_rq_total_200"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.Get.upstream_rq_total"));

  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.Get.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.Get.upstream_rq_time_200", _));
  EXPECT_CALL(stats_, deliverTimingToSinks("prefix.dynamodb.operation.Get.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
}

TEST_F(DynamoFilterTest, jsonBodyNotWellFormed) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.GetItem"}, {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  buffer.add("test", 4);
  buffer.add("test2", 5);

  EXPECT_CALL(stats_, counter("prefix.dynamodb.invalid_req_body"));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(buffer, true));
}

TEST_F(DynamoFilterTest, bothOperationAndTableIncorrect) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version"}, {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation_missing"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table_missing"));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
}

TEST_F(DynamoFilterTest, handleErrorTypeTableMissing) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version"}, {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation_missing"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table_missing"));

  Http::HeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl error_data;
  std::string internal_error =
      "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ValidationException\"}";
  error_data.add(internal_error);
  EXPECT_CALL(stats_, counter("prefix.dynamodb.error.no_table.ValidationException"));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(error_data, true));

  error_data.add("}", 1);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->encodeData(error_data, false));
  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).WillRepeatedly(Return(&error_data));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.invalid_resp_body"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation_missing"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table_missing"));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(request_headers));
}

TEST_F(DynamoFilterTest, HandleErrorTypeTablePresent) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.GetItem"}, {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  std::string buffer_content = "{\"TableName\":\"locations\"}";
  buffer.add(buffer_content);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(buffer, true));

  Http::HeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl error_data;
  std::string internal_error =
      "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ValidationException\"}";
  error_data.add(internal_error);
  EXPECT_CALL(stats_, counter("prefix.dynamodb.error.locations.ValidationException"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total_4xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total_400"));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time_4xx", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time_400", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time", _));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total_4xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total_400"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total"));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time_4xx", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time_400", _));
  EXPECT_CALL(stats_, deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(error_data, true));
}

TEST_F(DynamoFilterTest, BatchMultipleTables) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.BatchGetItem"},
                                      {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  std::string buffer_content = R"EOF(
{
  "RequestItems": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  buffer.add(buffer_content);

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(buffer, false));
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&buffer));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(stats_, counter("prefix.dynamodb.multiple_tables"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_200"));

  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_200", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.BatchGetItem.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
}

TEST_F(DynamoFilterTest, BatchMultipleTablesUnprocessedKeys) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.BatchGetItem"},
                                      {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  std::string buffer_content = R"EOF(
{
  "RequestItems": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  buffer.add(buffer_content);

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(buffer, false));
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&buffer));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(stats_, counter("prefix.dynamodb.multiple_tables"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_200"));

  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_200", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.BatchGetItem.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl empty_data;
  Buffer::OwnedImpl response_data;
  std::string response_content = R"EOF(
{
  "UnprocessedKeys": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  response_data.add(response_content);

  EXPECT_CALL(stats_, counter("prefix.dynamodb.error.table_1.BatchFailureUnprocessedKeys"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.error.table_2.BatchFailureUnprocessedKeys"));
  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).Times(1).WillRepeatedly(Return(&response_data));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(empty_data, true));
}

TEST_F(DynamoFilterTest, BatchMultipleTablesNoUnprocessedKeys) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.BatchGetItem"},
                                      {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  std::string buffer_content = R"EOF(
{
  "RequestItems": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  buffer.add(buffer_content);

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(buffer, false));
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&buffer));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(stats_, counter("prefix.dynamodb.multiple_tables"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_200"));

  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_200", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.BatchGetItem.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl empty_data;
  Buffer::OwnedImpl response_data;
  std::string response_content = R"EOF(
{
  "UnprocessedKeys": {
  }
}
)EOF";
  response_data.add(response_content);

  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).Times(1).WillRepeatedly(Return(&response_data));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(empty_data, true));
}

TEST_F(DynamoFilterTest, BatchMultipleTablesInvalidResponseBody) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.BatchGetItem"},
                                      {"random", "random"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  Buffer::OwnedImpl buffer;
  std::string buffer_content = R"EOF(
{
  "RequestItems": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  buffer.add(buffer_content);

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(buffer, false));
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&buffer));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_headers));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(stats_, counter("prefix.dynamodb.multiple_tables"));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.BatchGetItem.upstream_rq_total_200"));

  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_, deliverTimingToSinks(
                          "prefix.dynamodb.operation.BatchGetItem.upstream_rq_time_200", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.BatchGetItem.upstream_rq_time", _));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl empty_data;
  Buffer::OwnedImpl response_data;
  std::string response_content = R"EOF(
{
  "UnprocessedKeys": {
    "table_1": { "test1" : "something" },
    "table_2": { "test2" : "something" }
  }
}
)EOF";
  response_data.add(response_content);
  response_data.add("}", 1);

  EXPECT_CALL(stats_, counter("prefix.dynamodb.invalid_resp_body"));
  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).Times(1).WillRepeatedly(Return(&response_data));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(empty_data, true));
}

TEST_F(DynamoFilterTest, bothOperationAndTableCorrect) {
  setup(true);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.GetItem"}};
  Buffer::OwnedImpl buffer;
  std::string buffer_content = "{\"TableName\":\"locations\"";
  buffer.add(buffer_content);
  EXPECT_CALL(decoder_callbacks_, decodingBuffer()).WillRepeatedly(Return(&buffer));
  Buffer::OwnedImpl data;
  data.add("}", 1);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_->decodeData(data, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total_200"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.operation.GetItem.upstream_rq_total"));

  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time_200", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.operation.GetItem.upstream_rq_time", _));

  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total_2xx"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total_200"));
  EXPECT_CALL(stats_, counter("prefix.dynamodb.table.locations.upstream_rq_total"));

  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time_2xx", _));
  EXPECT_CALL(stats_,
              deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time_200", _));
  EXPECT_CALL(stats_, deliverTimingToSinks("prefix.dynamodb.table.locations.upstream_rq_time", _));

  Http::HeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
}

TEST_F(DynamoFilterTest, operatorPresentRuntimeDisabled) {
  setup(false);

  EXPECT_CALL(stats_, counter(_)).Times(0);
  EXPECT_CALL(stats_, deliverTimingToSinks(_, _)).Times(0);

  Http::HeaderMapImpl request_headers{{"x-amz-target", "version.operator"}, {"random", "random"}};
  Http::HeaderMapImpl response_headers{{":status", "200"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_headers));
}

} // Dynamo
