#!/bin/bash
# Build + run the fixture generator against a Qubic core checkout.
# usage: p02_build_fixtures.sh <core-root> <out-dir>
set -e
CORE="$1"; OUT="$2"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/overlay"
sed '971s/pv\.maxVotes/ProposalVotingType::maxVotes/' "$CORE/src/qpi/impl/qpi_proposals_impl.h" > "$OUT/overlay/qpi_proposals_impl_patched.h"
g++ -std=c++20 -O1 -mavx2 -mbmi2 -fno-access-control -Wno-invalid-offsetof -w \
    -I "$OUT/overlay" -I "$CORE/src" -I "$CORE" "$HERE/p02_make_fixtures.cpp" -o "$OUT/make_fixtures" 2> "$OUT/gcc_fixtures.log" || { echo "COMPILE FAILED, see $OUT/gcc_fixtures.log"; grep -m 20 -E "error|undefined" "$OUT/gcc_fixtures.log"; exit 1; }
"$OUT/make_fixtures" "$OUT"
ls -la "$OUT"/fixture*
