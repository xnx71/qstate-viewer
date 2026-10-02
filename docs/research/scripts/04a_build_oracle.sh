#!/bin/bash
# Build + run the g++ layout oracle for contracts 1..15 against a Qubic core checkout and compare it with the Python
# reference (04a_contract_layouts.py).  The oracle includes the REAL headers; only one line of
# src/qpi/impl/qpi_proposals_impl.h is patched in an overlay copy (g++ 13 lacks P2280: "pv.maxVotes" inside a
# member-function body is rejected as "use of this in a constant expression"; irrelevant for layout).
#
# usage: 04a_build_oracle.sh <version: v1.303.2|HEAD> <core-root> <out-dir>
set -e
VER="$1"; CORE="$2"; OUT="$3"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/overlay"
sed '971s/pv\.maxVotes/ProposalVotingType::maxVotes/' "$CORE/src/qpi/impl/qpi_proposals_impl.h" > "$OUT/overlay/qpi_proposals_impl_patched.h"
python3 "$HERE/04a_contract_layouts.py" --json "$OUT/layouts.json" --emit-oracle "$VER" "$OUT/oracle.cpp" > "$OUT/python.log"
g++ -std=c++20 -mavx2 -mbmi2 -fno-access-control -Wno-invalid-offsetof -w \
    -I "$OUT/overlay" -I "$CORE/src" -I "$CORE" "$OUT/oracle.cpp" -o "$OUT/oracle" 2> "$OUT/gcc.log" \
    || { echo "COMPILE FAILED, see $OUT/gcc.log"; grep -m 20 error "$OUT/gcc.log"; exit 1; }
"$OUT/oracle" > "$OUT/oracle.out"
python3 "$HERE/04a_contract_layouts.py" --json "$OUT/layouts.json" --check-oracle "$VER" "$OUT/oracle.out" | grep -i "oracle\|mismatch\|problem"
