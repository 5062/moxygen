#!/usr/bin/env bash
# Build and run moxygen's MoQTests, which hold the codec's trace tests, in the
# configuration moq_trace/build.sh selects: traced unless TRACE=0.
#
#   nix develop --command moq_trace/test.sh [gtest option...]
#
# The environment is build.sh's: MOQ_TRACE_PREFIX, MVFST_SOURCE_DIR, and
# MOXYGEN_BUILD_DIR. Options pass through to the test binary, for example
# --gtest_filter='*MoQCodecTest*'.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
build=${MOXYGEN_BUILD_DIR:-$here/_build}

"$here/moq_trace/build.sh" MoQTests
exec "$build/moxygen/test/MoQTests" "$@"
