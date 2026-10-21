#!/usr/bin/env bash
set -euo pipefail

mkdir -p "$SANITIZER_LOG_DIR"
for binary in build-sanitizers/OfxRawHost build-sanitizers/OfxRawHost.ofx; do
  symbols="$SANITIZER_LOG_DIR/$(basename "$binary").symbols"
  nm "$binary" > "$symbols"
  grep -q __asan_init "$symbols"
  grep -q __ubsan_handle_ "$symbols"
done

# Use the same compiler/flags and inherited sanitizer settings as the full suite.
# Deliberate word splitting supplies the separate flags from the workflow.
"$CXX" $SANITIZER_FLAGS -std=c++17 -g -O1 .github/sanitizers/probe.cpp \
  -o "$SANITIZER_LOG_DIR/probe"

expect_failure() {
  local mode="$1" diagnostic="$2" status=0
  local log="$SANITIZER_LOG_DIR/probe-$mode.log"
  "$SANITIZER_LOG_DIR/probe" "$mode" > "$log" 2>&1 || status=$?
  cat "$log"
  if [[ "$status" -eq 0 ]] || ! grep -q "$diagnostic" "$log" || \
      ! grep -q 'probe.cpp:' "$log"; then
    echo "::error::Sanitizer probe '$mode' did not fail with a symbolized diagnostic"
    exit 1
  fi
}

expect_failure address 'AddressSanitizer: heap-use-after-free'
expect_failure overflow 'runtime error: signed integer overflow'
expect_failure shift 'runtime error: left shift of negative value'
expect_failure leak 'LeakSanitizer: detected memory leaks'
