#!/usr/bin/env bash
# One relay session over loopback, which moq_trace/test_trace.sh records: the
# traced relay serves a date publisher and a text subscriber for a few seconds.
#
#   moq_trace/trace_fixture.sh BUILD_DIR WORK_DIR
#
# Every process stops on SIGINT, so each one ends its trace tokens on exit.
set -euo pipefail

build=$1
work=$2
port=$((20000 + RANDOM % 20000))
url="https://localhost:$port/moq-relay"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -keyout "$work/key.pem" -out "$work/cert.pem" -days 1 -subj /CN=localhost \
  -addext subjectAltName=DNS:localhost 2>/dev/null

pids=()
stop() {
  for pid in "${pids[@]}"; do kill -INT "$pid" 2>/dev/null || true; done
  wait "${pids[@]}" 2>/dev/null || true
}
trap stop EXIT

"$build/moxygen/relay/moqrelayserver" -port "$port" -cert "$work/cert.pem" \
  -key "$work/key.pem" >"$work/relay.log" 2>&1 &
pids+=($!)
sleep 1
"$build/moxygen/samples/date/moqdateserver" -relay_url "$url" -insecure \
  -cert "$work/cert.pem" -key "$work/key.pem" >"$work/publisher.log" 2>&1 &
pids=($! "${pids[@]}")
sleep 1
"$build/moxygen/samples/text-client/moqtextclient" -connect_url "$url" -insecure \
  -track_namespace moq-date -track_name date >"$work/subscriber.log" 2>&1 &
pids=($! "${pids[@]}")
sleep 4
