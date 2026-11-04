#!/bin/sh
# Runs every unit test in the repository. Tests that need the game skip
# when BB_GAME_DIR is unset (environment or local.env).
#
# Usage: tools/run-tests.sh
set -eu
cd "$(dirname "$0")/.."
status=0
for dir in tools/*/; do
    ls "$dir"test_*.py >/dev/null 2>&1 || continue
    echo "== $dir"
    python3 -m unittest discover -s "$dir" || status=1
done
[ "$status" -eq 0 ] || { echo "tests failed" >&2; exit 1; }
