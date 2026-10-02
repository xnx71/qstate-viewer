#!/bin/bash
# Build + run the proposal layout oracle against a Qubic core checkout.
# usage: p02_build_oracle.sh <core-root> <out-dir>
set -e
CORE="$1"; OUT="$2"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/overlay"
# patched copy of the impl header (single change in a function body; g++ 13 lacks P2280)
sed '971s/pv\.maxVotes/ProposalVotingType::maxVotes/' "$CORE/src/qpi/impl/qpi_proposals_impl.h" > "$OUT/overlay/qpi_proposals_impl_patched.h"
diff "$CORE/src/qpi/impl/qpi_proposals_impl.h" "$OUT/overlay/qpi_proposals_impl_patched.h" > "$OUT/overlay/patch.diff" || true
g++ -std=c++20 -mavx2 -mbmi2 -fno-access-control -Wno-invalid-offsetof -w \
    -I "$OUT/overlay" -I "$CORE/src" -I "$CORE" "$HERE/p02_oracle_proposals.cpp" -o "$OUT/oracle" 2> "$OUT/gcc.log" || { echo "COMPILE FAILED, see $OUT/gcc.log"; grep -m 20 error "$OUT/gcc.log"; exit 1; }
"$OUT/oracle" > "$OUT/oracle.out"
echo "ok: $OUT/oracle.out ($(wc -l < "$OUT/oracle.out") lines)"
