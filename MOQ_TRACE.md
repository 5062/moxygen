# MoQ trace instrumentation

This fork adds the [moq-trace](https://github.com/5062/moq-trace) toolkit's
`moq_trace:*` object tracepoints to moxygen's relay, and builds it on the
[5062/mvfst](https://github.com/5062/mvfst) fork, whose `quic_trace:*` hooks
record the QUIC packets, STREAM frames, and UDP system calls that carry the
objects. It is a measurement fork, not intended for upstream submission.

The branch starts at `694e3e25` (2026-10-10). The relay speaks draft 16 to the
bench peers, which `-versions 16` selects.

## What the instrumentation emits

MoQ object lifecycles, one RX lifecycle per inbound object and one TX lifecycle
per subscriber copy:

| Call site | Emission |
| --- | --- |
| `MoQObjectStreamCodec::onIngress`, subgroup object header | header start and end times and stream offsets, reported through `onObjectHeaderTrace` |
| `ObjectStreamCallback::onObjectBegin`, `onObjectStatus` | object start, header parse phase, payload read phase for the first fragment, and the create phase |
| `ObjectStreamCallback::onObjectPayload` | per later fragment, a payload read phase and a frame commit phase |
| the consumer call in each of them | the forwarder's fan-out, recorded as notify; object end with the call's outcome once the object is complete |
| `ObjectStreamCallback::reset` | object end, `reset`, for an object the stream ended under |
| `MoQForwarder::cloneForSubscriber` | clone span, handed to the subscriber's consumer |
| `StreamPublisherImpl::object`, `beginObject`, `publishStatus` | copy start, delivery wait and clone phases, and the copy work that notify excludes |
| `StreamPublisherImpl::objectPayload` | a later fragment's clone phase |
| `StreamPublisherImpl::writeCurrentObject`, `writeToStream` | header encode, payload write, and transport call phases; copy end once its last byte is queued |
| `StreamPublisherImpl::reset` | copy end, `expired` for a delivery timeout and `reset` otherwise |

The transport lifecycles come from mvfst; its `MOQ_TRACE.md` lists them.

Status objects, such as end of group, are objects: each gets an RX lifecycle,
and a TX lifecycle per subscriber the forwarder writes it to. Fetch streams are
not traced. A copy is traced only when its inbound object was, which the
thread-local `trace::currentInbound` carries from the inbound consumer call to
the subscribers' publishers.

## Synchronous delivery

moxygen hands each object to its subscribers inside the inbound stream's read
callback. `MoQForwarder` clones the payload and calls every subscriber's
consumer before the inbound consumer call returns.

- The relay stores nothing before forwarding, so create runs from the read
  callback until the consumer call, where the object becomes readable.
- The consumer call is the notify phase, minus each copy's work from the start
  of its lifecycle until its publisher returns, which belongs to that copy.
  `moq_trace::FanOut` and `moq_trace::CopyWork` from the toolkit's C++ facade
  record it.
- A copy's delivery wait runs from the start of the fan-out until its clone, so
  it includes the copies written for the subscribers served before it.
- The transport call phase only queues bytes on the mvfst stream. Packets are
  built and sent later from mvfst's write loop, which the packet lifecycles
  record.

## Identity and fragmentation contract

- One logical group is allocated per inbound subgroup stream, and each object
  on it takes the next frame, so every copy of an object carries the inbound
  object's logical identity.
- An object starts at the first attempt to parse its header, when the header's
  first bytes are readable. A header split across reads is parsed again from
  its start once more bytes arrive, and its object includes that wait.
- Stream offsets include the WebTransport stream prefix, the stream type and
  the session ID, which is the CONNECT stream's ID, so they match the byte
  ranges mvfst records for the stream.
- Every session takes its connection ID from mvfst's
  `TransportInfo::quicTraceConnectionId`, so objects join the packets of their
  connection.
- A fragment or a partial write is a repeated phase inside its lifecycle, not a
  separate lifecycle.

## Build

```sh
MOQ_TRACE_PREFIX=/path/to/moq-trace/target/install nix develop --command moq_trace/build.sh
TRACE=0 nix develop --command moq_trace/build.sh
```

`MOQ_TRACE_PREFIX` names the toolkit's C++ install. The traced build fetches
mvfst from 5062/mvfst at the revision in `moq_trace/mvfst-rev.txt`, or uses the
checkout that `MVFST_SOURCE_DIR` names. `MVFST_MOQ_TRACE` changes struct layouts
in mvfst headers, so the standalone CMake applies the trace definitions and the
toolkit facade to mvfst, proxygen, and moxygen alike. `TRACE=0` compiles the
hooks out and builds upstream mvfst. `MOXYGEN_BUILD_DIR` moves the build tree,
so the two configurations keep separate artifacts.

## Running

The relay reads datagrams through folly's read callback by default. The traced
build patches folly (`moq_trace/folly-read-dispatcher.patch`, applied by the
standalone CMake when `MOXYGEN_MOQ_TRACE` is on) so those reads go through the
socket's netops dispatcher, where mvfst times them; the patch changes nothing
without a dispatcher override. The traced relay therefore keeps stock moxygen's
read path, and the workspace profile does not pass `-recvmmsg`, which selects
mvfst's own batched `recvmmsg` read instead.

## Verification

```sh
nix develop --command moq_trace/test.sh
nix develop --command moq_trace/test_trace.sh
```

`moq_trace/test.sh` builds and runs `MoQTests` in the same configuration and
passes its options to the test binary. The trace tests in `MoQCodecTest` check
that object offsets count every stream byte however the stream is split into
reads, and that an object whose header is split across reads starts at its
first header bytes.

`moq_trace/test_trace.sh` checks the mvfst fork's transport hooks the way the
Quinn and Google QUICHE forks check theirs. It runs the traced relay with
`moqdateserver` publishing and `moqtextclient` subscribing over loopback
(`moq_trace/trace_fixture.sh`) under LTTng, and checks the events with the
toolkit's `moq-trace verify`: balanced lifecycles and phases, queue phases on
their socket boundaries, per-frame phases, connection paths, and no abandoned
packet. It needs `MOQ_TRACE_PREFIX`, LTTng, OpenSSL, and the `moq-trace` command,
which the moq-trace development shell provides; from the moq-workspace,
`just test-trace` runs it with the other stacks' checks. The standalone build
links `moqdateserver` against folly's manual executor, which upstream's CMake
leaves out.
