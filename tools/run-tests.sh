#!/bin/sh
# Runs every unit test in the repository. Tests that need the game skip
# when BB_GAME_DIR is unset (environment or local.env). Fails when it
# finds no tests, so a gate never passes on an empty run.
# TODO: add ctest here once the CMake build exists (M2).
#
# Usage: tools/run-tests.sh
set -eu
cd "$(dirname "$0")/.."
dirs="$(find tools src -name 'test_*.py' -not -path '*/.*' 2>/dev/null |
    xargs -r -n1 dirname | sort -u)"
[ -n "$dirs" ] || { echo "no tests found" >&2; exit 1; }
status=0
for dir in $dirs; do
    echo "== $dir"
    python3 -m unittest discover -s "$dir" -p 'test_*.py' || status=1
done
[ "$status" -eq 0 ] || { echo "tests failed" >&2; exit 1; }
