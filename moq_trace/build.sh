#!/usr/bin/env bash
# Build moqrelayserver in this checkout, optionally with the MoQ trace hooks.
#
#   nix develop --command moq_trace/build.sh
#
# The traced build finds the moq-trace toolkit's C++ install, the prefix holding
# lib/cmake/moq_trace, through MOQ_TRACE_PREFIX. TRACE=0 compiles the hooks out,
# which is the configuration an ordinary moxygen checkout builds.
#
# The binary lands in _build/moxygen/relay/moqrelayserver. MOXYGEN_BUILD_DIR
# moves the build tree, which is how the traced and untraced configurations keep
# their compiled artifacts instead of rebuilding each other.
#
# Folly, fizz, wangle, mvfst, and proxygen are fetched at the revisions in
# build/deps/github_hashes on the first configure of a build tree. A traced
# build takes mvfst from the 5062/mvfst fork that carries its quic_trace hooks,
# at the revision in moq_trace/mvfst-rev.txt, or from the checkout of that fork
# that MVFST_SOURCE_DIR names.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
build=${MOXYGEN_BUILD_DIR:-$here/_build}

options=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DBUILD_TESTS=OFF
  -DBUILD_MOQTEST=OFF
)
if [ "${TRACE:-1}" = 0 ]; then
  options+=(-DMOXYGEN_MOQ_TRACE=OFF)
else
  if [ -z "${MOQ_TRACE_PREFIX:-}" ]; then
    echo "MOQ_TRACE_PREFIX must name the moq-trace C++ install" >&2
    exit 1
  fi
  # The facade headers are inputs of every traced object, so a reinstalled
  # toolkit rebuilds them through the include dependencies CMake tracks.
  options+=(-DMOXYGEN_MOQ_TRACE=ON -DCMAKE_PREFIX_PATH="$MOQ_TRACE_PREFIX")
  if [ -n "${MVFST_SOURCE_DIR:-}" ]; then
    options+=(-DFETCHCONTENT_SOURCE_DIR_MVFST="$MVFST_SOURCE_DIR")
  fi
fi

cmake -B "$build" -S "$here/standalone" "${options[@]}"
exec cmake --build "$build" --target moqrelayserver
