/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/socket/heartbeat_handler.h"

#include <memory>
#include <utility>
#include <vector>

#include "net/dcsctp/packet/chunk/heartbeat_ack_chunk.h"
#include "net/dcsctp/packet/chunk/heartbeat_request_chunk.h"
#include "net/dcsctp/packet/parameter/heartbeat_info_parameter.h"
#include "net/dcsctp/public/types.h"
#include "net/dcsctp/socket/mock_context.h"
#include "net/dcsctp/testing/testing_macros.h"
#include "rtc_base/gunit.h"
#include "test/gmock.h"

namespace dcsctp {
namespace {
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SizeIs;

DcSctpOptions MakeOptions() {
  DcSctpOptions options;
  options.heartbeat_interval_include_rtt = false;
  options.heartbeat_interval = DurationMs(30'000);
  return options;
}

class HeartbeatHandlerTest : public testing::Test {
 protected:
  HeartbeatHandlerTest()
      : options_(MakeOptions()),
        context_(&callbacks_),
        timer_manager_([this]() { return callbacks_.CreateTimeout(); }),
        handler_("log: ", options_, &context_, &timer_manager_) {}

  const DcSctpOptions options_;
  NiceMock<MockDcSctpSocketCallbacks> callbacks_;
  NiceMock<MockContext> context_;
  TimerManager timer_manager_;
  HeartbeatHandler handler_;
};

TEST_F(HeartbeatHandlerTest, RepliesToHeartbeatRequests) {
  uint8_t info_data[] = {1, 2, 3, 4, 5};
  HeartbeatRequestChunk request(
      Parameters::Builder().Add(HeartbeatInfoParameter(info_data)).Build());

  handler_.HandleHeartbeatRequest(std::move(request));

  std::vector<uint8_t> payload = callbacks_.ConsumeSentPacket();
  ASSERT_HAS_VALUE_AND_ASSIGN(SctpPacket packet, SctpPacket::Parse(payload));
  ASSERT_THAT(packet.descriptors(), SizeIs(1));

  ASSERT_HAS_VALUE_AND_ASSIGN(
      HeartbeatAckChunk response,
      HeartbeatAckChunk::Parse(packet.descriptors()[0].data));

  ASSERT_HAS_VALUE_AND_ASSIGN(
      HeartbeatInfoParameter param,
      response.parameters().get<HeartbeatInfoParameter>());

  EXPECT_THAT(param.info(), ElementsAre(1, 2, 3, 4, 5));
}

TEST_F(HeartbeatHandlerTest, SendsHeartbeatRequestsOnIdleChannel) {
  callbacks_.AdvanceTime(options_.heartbeat_interval);
  for (TimeoutID id : callbacks_.RunTimers()) {
    timer_manager_.HandleTimeout(id);
  }

  // Grab the request, and make a response.
  std::vector<uint8_t> payload = callbacks_.ConsumeSentPacket();
  ASSERT_HAS_VALUE_AND_ASSIGN(SctpPacket packet, SctpPacket::Parse(payload));
  ASSERT_THAT(packet.descriptors(), SizeIs(1));

  ASSERT_HAS_VALUE_AND_ASSIGN(
      HeartbeatRequestChunk req,
      HeartbeatRequestChunk::Parse(packet.descriptors()[0].data));

  HeartbeatAckChunk ack(std::move(req).extract_parameters());

  // Respond a while later. This RTT will be measured by the handler
  constexpr DurationMs rtt(313);

  EXPECT_CALL(context_, ObserveRTT(rtt)).Times(1);

  callbacks_.AdvanceTime(rtt);
  handler_.HandleHeartbeatAck(std::move(ack));
}

TEST_F(HeartbeatHandlerTest, IncreasesErrorIfNotAckedInTime) {
  callbacks_.AdvanceTime(options_.heartbeat_interval);

  DurationMs rto(105);
  EXPECT_CALL(context_, current_rto).WillOnce(Return(rto));
  for (TimeoutID id : callbacks_.RunTimers()) {
    timer_manager_.HandleTimeout(id);
  }

  // Validate that a request was sent.
  EXPECT_THAT(callbacks_.ConsumeSentPacket(), Not(IsEmpty()));

  EXPECT_CALL(context_, IncrementTxErrorCounter).Times(1);
  callbacks_.AdvanceTime(rto);
  for (TimeoutID id : callbacks_.RunTimers()) {
    timer_manager_.HandleTimeout(id);
  }
}

}  // namespace
}  // namespace dcsctp
