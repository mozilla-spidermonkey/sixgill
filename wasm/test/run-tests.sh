#!/bin/bash
#
# Run the wasm/test/ test suite. Each wasm/test/test-*.mjs exercises the
# compiled wasm decoders (xdbToText/xdbToJson) and compares them against the
# native tools.
#
# Requires: node, the node build of the wasm module
# (wasm/test/xdb-json-wasm.node.mjs, built by `make wasm-test`), bin/xdbfind,
# and a populated test database (produced by the main test suite). An optional
# database path may be given and is forwarded to each test.
#
# Usage: wasm/test/run-tests.sh [xdb.sqlite]

set -eo pipefail

# node:sqlite is behind --experimental-sqlite on some node versions and stable
# on others; detect which so this works on both.

if node -e 'import("node:sqlite").then(m=>0,m=>1).then(r=>process.exit(r))'; then
  function run_node () { node "$@"; }
else
  function run_node () { node --experimental-sqlite "$@"; }
fi

tests=(wasm/test/test-*.mjs)
if [ ${#tests[@]} -eq 0 ]; then
  echo "no wasm tests found" >&2
  exit 1
fi

fails=0
for t in "${tests[@]}"; do
  name="$(basename "$t")"
  echo "=== wasm test: $name ==="
  if ! run_node "$t" "$@"; then
    fails=$((fails + 1))
  fi
done

if [ "$fails" -ne 0 ]; then
  echo "wasm tests: $fails test file(s) failed." >&2
  exit 1
fi
echo "wasm tests: all passed."
