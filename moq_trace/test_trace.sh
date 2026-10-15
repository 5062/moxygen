#!/usr/bin/env bash
# Capture the traced relay's fixture under LTTng and check it with the
# toolkit's `moq-trace verify`, as the Quinn and Google QUICHE forks check
# theirs. The relay is the server end of mvfst, which carries the QUIC hooks.
#
#   moq_trace/test_trace.sh
#
# Needs MOQ_TRACE_PREFIX as moq_trace/build.sh does, LTTng, OpenSSL, and the
# moq-trace command on PATH, which the moq-workspace checkout's development shell
# provides. The fixture, moq_trace/trace_fixture.sh, runs a date publisher and
# a text subscriber through the relay over loopback.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
if ! command -v moq-trace >/dev/null 2>&1; then
  echo "moq-trace must be on PATH; run from the moq-workspace development shell" >&2
  exit 1
fi
"$here/build.sh" moqrelayserver moqdateserver moqtextclient
build=${MOXYGEN_BUILD_DIR:-$here/../_build}

mkdir -p "$here/artifacts"
output=$(mktemp -d "$here/artifacts/test-trace.XXXXXX")
echo "Trace evidence: $output"
moq-trace verify "$output" --run "$here/trace_fixture.sh $build $output"
