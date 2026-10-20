/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <quic/logging/QLogger.h>
#include <quic/logging/QLoggerConstants.h>

#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace moxygen {

// A qlog the relay writes for the bench analysis: packets, recovery metrics,
// and losses as JSON-SEQ in one file. The worker thread only appends a
// fixed-size record; a writer thread formats them, so the qlog costs the
// relay little of the time it measures.
class RelayQLogSink {
 public:
  enum class Kind : uint8_t { Received, Sent, Metrics, Lost };

  struct Record {
    Kind kind;
    // 0 for a short header, else 1 + the long header type.
    uint8_t packetType{0};
    uint32_t connection{0};
    int64_t timeUs{0};
    // Received and Sent: the packet number. Metrics: latest, minimum, and
    // smoothed RTT in microseconds, congestion window, bytes in flight.
    // Lost: the number of packets declared lost.
    uint64_t values[5]{};
  };
  static constexpr uint64_t kAbsent = ~uint64_t{0};

  explicit RelayQLogSink(const std::string& path)
      : file_(std::fopen(path.c_str(), "w")) {
    if (file_ == nullptr) {
      throw std::runtime_error(fmt::format("cannot open qlog {}", path));
    }
    // Events carry steady_clock time, which is CLOCK_MONOTONIC on Linux, so
    // the qlog starts at monotonic zero.
    std::fputs(
        "\x1e{\"qlog_version\":\"0.3\",\"qlog_format\":\"JSON-SEQ\","
        "\"title\":\"moxygen relay monotonic_start_ns=0\",\"trace\":{}}\n",
        file_);
    thread_ = std::thread([this] { run(); });
  }

  ~RelayQLogSink() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_one();
    thread_.join();
    std::fclose(file_);
  }

  uint32_t addConnection(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    names_.push_back(std::move(name));
    return names_.size() - 1;
  }

  void push(const Record& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(record);
  }

 private:
  void run() {
    std::vector<Record> batch;
    std::vector<std::string> names;
    fmt::memory_buffer out;
    bool stopping = false;
    while (!stopping) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        // The worker never signals a record, so it pays no wakeup.
        wake_.wait_for(
            lock, std::chrono::milliseconds(10), [this] { return stopping_; });
        stopping = stopping_;
        batch.swap(pending_);
        names.insert(names.end(), names_.begin() + names.size(), names_.end());
      }
      for (const auto& record : batch) {
        format(out, record, names[record.connection]);
      }
      std::fwrite(out.data(), 1, out.size(), file_);
      out.clear();
      batch.clear();
    }
  }

  static void format(
      fmt::memory_buffer& out,
      const Record& record,
      const std::string& name) {
    auto time = record.timeUs;
    fmt::format_to(
        std::back_inserter(out),
        "\x1e{{\"time\":{}.{:03},",
        time / 1000,
        time % 1000);
    const auto* v = record.values;
    switch (record.kind) {
      case Kind::Received:
      case Kind::Sent:
        fmt::format_to(
            std::back_inserter(out),
            "\"name\":\"transport:packet_{}\",\"group_id\":\"{}\",\"data\":"
            "{{\"header\":{{\"packet_type\":\"{}\",\"packet_number\":{}}}}}}}\n",
            record.kind == Kind::Received ? "received" : "sent",
            name,
            packetType(record.packetType),
            v[0]);
        break;
      case Kind::Metrics:
        fmt::format_to(
            std::back_inserter(out),
            "\"name\":\"recovery:metrics_updated\",\"group_id\":\"{}\",\"data\":"
            "{{\"latest_rtt\":{}.{:03},\"min_rtt\":{}.{:03},"
            "\"smoothed_rtt\":{}.{:03}",
            name,
            v[0] / 1000,
            v[0] % 1000,
            v[1] / 1000,
            v[1] % 1000,
            v[2] / 1000,
            v[2] % 1000);
        if (v[3] != kAbsent) {
          fmt::format_to(
              std::back_inserter(out), ",\"congestion_window\":{}", v[3]);
        }
        if (v[4] != kAbsent) {
          fmt::format_to(
              std::back_inserter(out), ",\"bytes_in_flight\":{}", v[4]);
        }
        fmt::format_to(std::back_inserter(out), "}}}}\n");
        break;
      case Kind::Lost:
        fmt::format_to(
            std::back_inserter(out),
            "\"name\":\"recovery:packet_lost\",\"group_id\":\"{}\",\"data\":"
            "{{\"lost_packets\":{}}}}}\n",
            name,
            v[0]);
        break;
    }
  }

  static const char* packetType(uint8_t type) {
    if (type == 0) {
      return "1RTT";
    }
    switch (quic::LongHeader::Types(type - 1)) {
      case quic::LongHeader::Types::Initial:
        return "initial";
      case quic::LongHeader::Types::ZeroRtt:
        return "0RTT";
      case quic::LongHeader::Types::Handshake:
        return "handshake";
      case quic::LongHeader::Types::Retry:
        return "retry";
    }
    return "unknown";
  }

  std::FILE* file_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Record> pending_;
  std::vector<std::string> names_;
  bool stopping_{false};
  std::thread thread_;
};

// One connection's qlog. A connection is named by the client's original
// destination ID, which mvfst passes to setDcid; events before it are dropped.
class RelayQLogger : public quic::QLogger {
 public:
  RelayQLogger(
      quic::VantagePoint vantagePoint,
      std::shared_ptr<RelayQLogSink> sink)
      : quic::QLogger(vantagePoint, quic::kHTTP3ProtocolType),
        sink_(std::move(sink)) {}

  void setDcid(quic::Optional<quic::ConnectionId> id) override {
    if (id.has_value() && !connection_.has_value()) {
      dcid = *id;
      connection_ = sink_->addConnection(id->hex());
    }
  }
  void setScid(quic::Optional<quic::ConnectionId> id) override {
    if (id.has_value()) {
      scid = *id;
    }
  }

  void addPacket(const quic::RegularQuicPacket& packet, uint64_t) override {
    addPacket(RelayQLogSink::Kind::Received, packet.header);
  }
  void addPacket(const quic::RegularQuicWritePacket& packet, uint64_t)
      override {
    addPacket(RelayQLogSink::Kind::Sent, packet.header);
  }
  void addMetricUpdate(
      std::chrono::microseconds latestRtt,
      std::chrono::microseconds mrtt,
      std::chrono::microseconds srtt,
      std::chrono::microseconds,
      quic::Optional<std::chrono::microseconds>,
      quic::Optional<uint64_t> congestionWindow,
      quic::Optional<uint64_t> bytesInFlight,
      quic::Optional<uint64_t>,
      quic::Optional<uint64_t>,
      quic::Optional<uint64_t>,
      quic::Optional<uint32_t>) override {
    push(
        {.kind = RelayQLogSink::Kind::Metrics,
         .values = {
             uint64_t(latestRtt.count()),
             uint64_t(mrtt.count()),
             uint64_t(srtt.count()),
             congestionWindow.value_or(RelayQLogSink::kAbsent),
             bytesInFlight.value_or(RelayQLogSink::kAbsent)}});
  }
  void addPacketsLost(quic::PacketNum, uint64_t, uint64_t lostPackets)
      override {
    push({.kind = RelayQLogSink::Kind::Lost, .values = {lostPackets}});
  }

  // The analysis reads none of these.
  void addPacket(const quic::VersionNegotiationPacket&, uint64_t, bool)
      override {}
  void addPacket(const quic::RetryPacket&, uint64_t, bool) override {}
  void addConnectionClose(std::string, std::string, bool, bool) override {}
  void addTransportSummary(const TransportSummaryArgs&) override {}
  void addCongestionMetricUpdate(
      uint64_t,
      uint64_t,
      std::string,
      std::string,
      std::string) override {}
  void addBandwidthEstUpdate(uint64_t, std::chrono::microseconds) override {}
  void addAppLimitedUpdate() override {}
  void addAppUnlimitedUpdate() override {}
  void addPacingMetricUpdate(uint64_t, std::chrono::microseconds) override {}
  void addPacingObservation(std::string, std::string, std::string) override {}
  void addAppIdleUpdate(std::string, bool) override {}
  void addPacketDrop(size_t, std::string) override {}
  void addDatagramReceived(uint64_t) override {}
  void addLossAlarm(quic::PacketNum, uint64_t, uint64_t, std::string)
      override {}
  void addTransportStateUpdate(std::string) override {}
  void addPacketBuffered(quic::ProtectionType, uint64_t) override {}
  void addCongestionStateUpdate(
      quic::Optional<std::string>,
      std::string,
      quic::Optional<std::string>,
      quic::Optional<uint64_t>) override {}
  void addStreamStateUpdate(
      quic::StreamId,
      std::string,
      quic::Optional<std::chrono::milliseconds>) override {}
  void addConnectionMigrationUpdate(bool) override {}
  void addPathValidationEvent(bool) override {}
  void addPriorityUpdate(
      quic::StreamId,
      quic::PriorityQueue::PriorityLogFields) override {}
  void addL4sWeightUpdate(double, uint32_t, uint32_t) override {}
  void addNetworkPathModelUpdate(
      uint64_t,
      uint64_t,
      uint64_t,
      std::chrono::microseconds,
      uint64_t,
      std::chrono::microseconds) override {}

 private:
  void addPacket(RelayQLogSink::Kind kind, const quic::PacketHeader& header) {
    const auto* longHeader = header.asLong();
    push(
        {.kind = kind,
         .packetType = uint8_t(
             longHeader ? 1 + uint8_t(longHeader->getHeaderType()) : 0),
         .values = {header.getPacketSequenceNum()}});
  }

  void push(RelayQLogSink::Record record) {
    if (!connection_.has_value()) {
      return;
    }
    record.connection = *connection_;
    record.timeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    sink_->push(record);
  }

  std::shared_ptr<RelayQLogSink> sink_;
  std::optional<uint32_t> connection_;
};

} // namespace moxygen
