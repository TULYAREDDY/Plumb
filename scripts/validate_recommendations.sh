#!/usr/bin/env bash
# scripts/validate_recommendations.sh
# ─────────────────────────────────────────────────────────────────────────
# Plumb doesn't just detect HOTSPOT / INLINE_CANDIDATE / VECTORIZABLE /
# RECURSIVE / HIGH_COMPLEXITY — this script actually APPLIES the real LLVM
# transformation (or, where none exists, a hand-written refactor) each tag
# implies, re-runs Plumb on the result, and reports whether the
# recommendation actually helped. Every number printed here is what got
# written into EVALUATION.md §6 — re-run this to reproduce them.
#
# Produces (all gitignored, like ir/ and results/):
#   validation/*.ll     intermediate IR for each transformation
#   validation/*.json    Plumb re-analysis of the transformed IR
#
# Usage: ./scripts/validate_recommendations.sh
# ─────────────────────────────────────────────────────────────────────────
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# shellcheck source=scripts/_llvm_env.sh
source "$ROOT/scripts/_llvm_env.sh"

# ── make sure ir/ and results/ are fresh (also builds the pass if needed) ──
banner "Refreshing ir/ and results/ via ./run.sh"
"$ROOT/run.sh" --no-open || die "./run.sh failed — ir/results/ may be stale or incomplete, refusing to validate against them"

# ── locate the built pass library ───────────────────────────────────────
find_pass_lib
[ -n "$PASS_LIB" ] || die "pass library still missing after ./run.sh"

mkdir -p "$ROOT/validation"

banner "Applying and validating Plumb's 5 recommendation tags"
OPT="$OPT" PASS_LIB="$PASS_LIB" ROOT="$ROOT" \
    python3 "$ROOT/scripts/validate_recommendations.py"
