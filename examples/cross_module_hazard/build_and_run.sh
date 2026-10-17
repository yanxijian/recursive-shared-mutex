#!/bin/sh
# Build + run the cross-module hazard demo on Linux/macOS.
#   ./build_and_run.sh            (uses $CXX, default c++)
#   CXX=clang++ ./build_and_run.sh
# The point: the shared library and the host each get their own thread_local
# slot table, so a shared lock taken in one module cannot be released in the
# other, and the host's second lock_shared() reaches the backend again.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${RSM_REPO:-/path/to/recursive-shared-mutex}
INC="$REPO/include"
OUT="$HERE/out-unix"
CXX=${CXX:-c++}
mkdir -p "$OUT"
cd "$OUT"

$CXX -O2 -std=c++17 -fPIC -fvisibility=hidden -I"$INC" -shared "$HERE/dll_side.cpp" -o librsm_hazard.so
$CXX -O2 -std=c++17 -I"$INC" "$HERE/exe_side.cpp" -o hazard -L"$OUT" -lrsm_hazard -Wl,-rpath,"$OUT"
./hazard
