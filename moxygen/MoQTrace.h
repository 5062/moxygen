/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// MoQ object trace hooks for the moq_trace LTTng providers. Everything here
// compiles away unless MOXYGEN_MOQ_TRACE is defined, which the
// MOXYGEN_MOQ_TRACE CMake option does.

#if defined(MOXYGEN_MOQ_TRACE)

#include <moq_trace/trace.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace moxygen::trace {

// The inbound object whose bytes this thread is handing to the relay.
//
// moxygen forwards cut-through: the receiving session calls its subgroup
// consumer, and MoQForwarder calls every subscriber's consumer synchronously
// inside that call, on the one EventBase that runs every relay session. A copy
// written inside that call therefore belongs to the object published here.
// A copy written outside it, such as one served later from the cache, has no
// inbound object and is not traced.
struct InboundObject {
  // Shared by the inbound object and every outbound copy of it.
  moq_trace::LogicalId logicalId;
  // When the relay began handing the object to its consumers, on the trace
  // clock. Each copy's delivery wait runs from here until its clone starts.
  uint64_t readyNs{0};
};

inline thread_local const InboundObject* currentInbound = nullptr;

// Publishes an inbound object to the copies written while it is in scope.
// Scopes nest, so a consumer that forwards to another relay session restores
// the outer object when it returns.
class ScopedInboundObject {
 public:
  explicit ScopedInboundObject(const InboundObject* object)
      : previous_(std::exchange(currentInbound, object)) {}
  ~ScopedInboundObject() {
    currentInbound = previous_;
  }
  ScopedInboundObject(const ScopedInboundObject&) = delete;
  ScopedInboundObject& operator=(const ScopedInboundObject&) = delete;

 private:
  const InboundObject* previous_;
};

// The forwarder's reference clone of a payload for the one subscriber it is
// about to call. Clone timing is copy-specific work, but the clone happens in
// MoQForwarder before the subscriber's consumer, which owns the copy's trace,
// runs. The forwarder records the span here and the consumer takes it.
struct CloneSpan {
  uint64_t startNs{0};
  uint64_t endNs{0};
};

inline thread_local std::optional<CloneSpan> pendingClone;

// The encoded size of a minimal QUIC variable-length integer.
inline uint64_t varintSize(uint64_t value) {
  return value < (1ULL << 6) ? 1
      : value < (1ULL << 14) ? 2
      : value < (1ULL << 30) ? 4
                             : 8;
}

// Whether any object event would be recorded, so hooks read no clock when
// tracing is off.
inline bool objectTraceEnabled() {
  return moq_trace_moq_object_start_enabled() ||
      moq_trace_moq_object_end_enabled() ||
      moq_trace_moq_object_phase_enabled();
}

// Clone a payload for one subscriber, recording the span for its consumer.
// The caller clears pendingClone after the consumer returns, so a consumer
// that does not take it cannot leave it for the next subscriber.
template <typename Payload, typename CloneFn>
Payload tracedClone(const Payload& payload, CloneFn&& clone) {
  if (currentInbound == nullptr || !moq_trace_moq_object_phase_enabled()) {
    return clone(payload);
  }
  const uint64_t startNs = quic_trace::now_ns();
  auto copy = clone(payload);
  pendingClone = CloneSpan{startNs, quic_trace::now_ns()};
  return copy;
}

} // namespace moxygen::trace

#endif // MOXYGEN_MOQ_TRACE
