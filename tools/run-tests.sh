#!/bin/sh
# Runs every unit test in the repository: the Python tests, then the C++
# tests through the debug preset (configure, build, ctest). Tests that need
# the game skip when BB_GAME_DIR is unset (environment or local.env). Fails
# when it finds no tests, so a gate never passes on an empty run, and when
# the C++ build can't run, so a missing toolchain never passes either.
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
if [ -f CMakeLists.txt ]; then
    echo "== C++ (debug preset)"
    if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
        echo "cmake or ctest not found: the C++ tests can't run" >&2
        status=1
    elif ! { cmake --preset debug >/dev/null &&
             cmake --build --preset debug &&
             ctest --preset debug --no-tests=error; }; then
        status=1
    fi
fi
[ "$status" -eq 0 ] || { echo "tests failed" >&2; exit 1; }
