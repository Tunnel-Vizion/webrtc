/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_sender_egress.h"

#include <string>

#include "absl/types/optional.h"
#include "api/array_view.h"
#include "api/call/transport.h"
#include "api/units/data_size.h"
#include "api/units/timestamp.h"
#include "logging/rtc_event_log/mock/mock_rtc_event_log.h"
#include "modules/rtp_rtcp/include/rtp_rtcp.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"
#include "modules/rtp_rtcp/source/rtp_packet_history.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "test/gmock.h"
#include "test/gtest.h"
#include "test/time_controller/simulated_time_controller.h"

namespace webrtc {
namespace {

using ::testing::Field;
using ::testing::NiceMock;
using ::testing::StrictMock;

constexpr Timestamp kStartTime = Timestamp::Millis(123456789);
constexpr int kDefaultPayloadType = 100;
constexpr uint16_t kStartSequenceNumber = 33;
constexpr uint32_t kSsrc = 725242;
constexpr uint32_t kRtxSsrc = 12345;
enum : int {
  kTransportSequenceNumberExtensionId = 1,
};

struct TestConfig {
  explicit TestConfig(bool with_overhead) : with_overhead(with_overhead) {}
  bool with_overhead = false;
};

class MockSendPacketObserver : public SendPacketObserver {
 public:
  MOCK_METHOD(void, OnSendPacket, (uint16_t, int64_t, uint32_t), (override));
};

class MockTransportFeedbackObserver : public TransportFeedbackObserver {
 public:
  MOCK_METHOD(void, OnAddPacket, (const RtpPacketSendInfo&), (override));
  MOCK_METHOD(void,
              OnTransportFeedback,
              (const rtcp::TransportFeedback&),
              (override));
};

class MockStreamDataCountersCallback : public StreamDataCountersCallback {
 public:
  MOCK_METHOD(void,
              DataCountersUpdated,
              (const StreamDataCounters& counters, uint32_t ssrc),
              (override));
};

class FieldTrialConfig : public WebRtcKeyValueConfig {
 public:
  FieldTrialConfig() : overhead_enabled_(false) {}
  ~FieldTrialConfig() override {}

  void SetOverHeadEnabled(bool enabled) { overhead_enabled_ = enabled; }

  std::string Lookup(absl::string_view key) const override {
    if (key == "WebRTC-SendSideBwe-WithOverhead") {
      return overhead_enabled_ ? "Enabled" : "Disabled";
    }
    return "";
  }

 private:
  bool overhead_enabled_;
};

struct TransmittedPacket {
  TransmittedPacket(rtc::ArrayView<const uint8_t> data,
                    const PacketOptions& packet_options,
                    RtpHeaderExtensionMap* extensions)
      : packet(extensions), options(packet_options) {
    EXPECT_TRUE(packet.Parse(data));
  }
  RtpPacketReceived packet;
  PacketOptions options;
};

class TestTransport : public Transport {
 public:
  explicit TestTransport(RtpHeaderExtensionMap* extensions)
      : total_data_sent_(DataSize::Zero()), extensions_(extensions) {}
  bool SendRtp(const uint8_t* packet,
               size_t length,
               const PacketOptions& options) override {
    total_data_sent_ += DataSize::Bytes(length);
    last_packet_.emplace(rtc::MakeArrayView(packet, length), options,
                         extensions_);
    return true;
  }

  bool SendRtcp(const uint8_t*, size_t) override { RTC_CHECK_NOTREACHED(); }

  absl::optional<TransmittedPacket> last_packet() { return last_packet_; }

 private:
  DataSize total_data_sent_;
  absl::optional<TransmittedPacket> last_packet_;
  RtpHeaderExtensionMap* const extensions_;
};

}  // namespace

class RtpSenderEgressTest : public ::testing::TestWithParam<TestConfig> {
 protected:
  RtpSenderEgressTest()
      : time_controller_(kStartTime),
        clock_(time_controller_.GetClock()),
        transport_(&header_extensions_),
        packet_history_(clock_, /*enable_rtx_padding_prioritization=*/true),
        sequence_number_(kStartSequenceNumber) {
    trials_.SetOverHeadEnabled(GetParam().with_overhead);
  }

  std::unique_ptr<RtpSenderEgress> CreateRtpSenderEgress() {
    return std::make_unique<RtpSenderEgress>(DefaultConfig(), &packet_history_);
  }

  RtpRtcp::Configuration DefaultConfig() {
    RtpRtcp::Configuration config;
    config.clock = clock_;
    config.outgoing_transport = &transport_;
    config.local_media_ssrc = kSsrc;
    config.rtx_send_ssrc = kRtxSsrc;
    config.fec_generator = nullptr;
    config.event_log = &mock_rtc_event_log_;
    config.send_packet_observer = &send_packet_observer_;
    config.rtp_stats_callback = &mock_rtp_stats_callback_;
    config.transport_feedback_callback = &feedback_observer_;
    config.populate_network2_timestamp = false;
    config.field_trials = &trials_;
    return config;
  }

  std::unique_ptr<RtpPacketToSend> BuildRtpPacket(bool marker_bit,
                                                  int64_t capture_time_ms) {
    auto packet = std::make_unique<RtpPacketToSend>(&header_extensions_);
    packet->SetSsrc(kSsrc);
    packet->ReserveExtension<AbsoluteSendTime>();
    packet->ReserveExtension<TransmissionOffset>();
    packet->ReserveExtension<TransportSequenceNumber>();

    packet->SetPayloadType(kDefaultPayloadType);
    packet->set_packet_type(RtpPacketMediaType::kVideo);
    packet->SetMarker(marker_bit);
    packet->SetTimestamp(capture_time_ms * 90);
    packet->set_capture_time_ms(capture_time_ms);
    packet->SetSequenceNumber(sequence_number_++);
    return packet;
  }

  std::unique_ptr<RtpPacketToSend> BuildRtpPacket() {
    return BuildRtpPacket(/*marker_bit=*/true, clock_->CurrentTime().ms());
  }

  GlobalSimulatedTimeController time_controller_;
  Clock* const clock_;
  NiceMock<MockRtcEventLog> mock_rtc_event_log_;
  StrictMock<MockStreamDataCountersCallback> mock_rtp_stats_callback_;
  NiceMock<MockSendPacketObserver> send_packet_observer_;
  NiceMock<MockTransportFeedbackObserver> feedback_observer_;
  RtpHeaderExtensionMap header_extensions_;
  TestTransport transport_;
  RtpPacketHistory packet_history_;
  FieldTrialConfig trials_;
  uint16_t sequence_number_;
};

TEST_P(RtpSenderEgressTest, TransportFeedbackObserverGetsCorrectByteCount) {
  constexpr size_t kRtpOverheadBytesPerPacket = 12 + 8;
  constexpr size_t kPayloadSize = 1400;
  const uint16_t kTransportSequenceNumber = 17;

  header_extensions_.RegisterByUri(kTransportSequenceNumberExtensionId,
                                   TransportSequenceNumber::kUri);

  const size_t expected_bytes = GetParam().with_overhead
                                    ? kPayloadSize + kRtpOverheadBytesPerPacket
                                    : kPayloadSize;

  EXPECT_CALL(
      feedback_observer_,
      OnAddPacket(AllOf(
          Field(&RtpPacketSendInfo::ssrc, kSsrc),
          Field(&RtpPacketSendInfo::transport_sequence_number,
                kTransportSequenceNumber),
          Field(&RtpPacketSendInfo::rtp_sequence_number, kStartSequenceNumber),
          Field(&RtpPacketSendInfo::length, expected_bytes),
          Field(&RtpPacketSendInfo::pacing_info, PacedPacketInfo()))));

  std::unique_ptr<RtpPacketToSend> packet = BuildRtpPacket();
  packet->SetExtension<TransportSequenceNumber>(kTransportSequenceNumber);
  packet->AllocatePayload(kPayloadSize);

  std::unique_ptr<RtpSenderEgress> sender = CreateRtpSenderEgress();
  sender->SendPacket(packet.get(), PacedPacketInfo());
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutOverhead,
                         RtpSenderEgressTest,
                         ::testing::Values(TestConfig(false),
                                           TestConfig(true)));

}  // namespace webrtc
