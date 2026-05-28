#!/usr/bin/env bash
# benchmarks/run_bench.sh
# ─────────────────────────────────────────────────────────────────
#  Run Plumb across every C/C++ program in the cloned LLVM test-suite,
#  at -O0 and -O2, and write one JSON per (program, opt-level) pair.
#  This is what populates benchmarks/results/.
#
#  Usage:    ./benchmarks/run_bench.sh           # full run
#            ./benchmarks/run_bench.sh -j 8      # parallelism
#            ./benchmarks/run_bench.sh -p Stanford   # only one suite
# ─────────────────────────────────────────────────────────────────
set -uo pipefail
# We deliberately DON'T set -e: Plumb is run on every program, and a failure
# on one shouldn't abort the whole sweep. We track failures per-program.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_DIR="$ROOT/benchmarks"
SUITE="$BENCH_DIR/llvm-test-suite/SingleSource/Benchmarks"
IR_DIR="$BENCH_DIR/ir"
OUT_DIR="$BENCH_DIR/results"
LOG="$BENCH_DIR/run.log"

# shellcheck source=../scripts/_llvm_env.sh
source "$ROOT/scripts/_llvm_env.sh"

# locate the built pass
PASS_LIB=""
for cand in "$ROOT/build/libPlumb.${PLUGIN_EXT}" \
            "$ROOT/build/libPlumb.so" \
            "$ROOT/build/libPlumb.dylib"; do
  [ -f "$cand" ] && PASS_LIB="$cand" && break
done
if [ -z "$PASS_LIB" ]; then
  echo "↻ pass not built — running ./build.sh"
  "$ROOT/build.sh"
  for cand in "$ROOT/build/libPlumb.${PLUGIN_EXT}" \
              "$ROOT/build/libPlumb.so" \
              "$ROOT/build/libPlumb.dylib"; do
    [ -f "$cand" ] && PASS_LIB="$cand" && break
  done
fi

if [ ! -d "$SUITE" ]; then
  die "test-suite not cloned yet — run ./benchmarks/fetch.sh first"
fi

# parse args
JOBS=1
PATTERN=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -j) JOBS="$2"; shift 2 ;;
    -p) PATTERN="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [-j N] [-p NAME-PATTERN]"
      echo "  -j N        run N programs in parallel (default 1)"
      echo "  -p PATTERN  only run programs whose path matches PATTERN"
      exit 0 ;;
    *) shift ;;
  esac
done

mkdir -p "$IR_DIR" "$OUT_DIR"
: > "$LOG"

# enumerate programs (single .c/.cpp file each — that's what SingleSource
# means; we skip .h's and any non-source).
PROGRAMS=()
while IFS= read -r p; do
  [ -n "$PATTERN" ] && [[ "$p" != *"$PATTERN"* ]] && continue
  PROGRAMS+=("$p")
done < <(find "$SUITE" -type f \( -name '*.c' -o -name '*.cpp' \) | sort)

TOTAL=${#PROGRAMS[@]}
echo "⌁ running Plumb on $TOTAL programs from LLVM test-suite (jobs=$JOBS)…"

OK=0; FAIL=0; SKIP=0
START=$(date +%s)

run_one() {
  local src="$1"
  local rel="${src#$SUITE/}"
  local base="${rel%.*}"
  base="${base//\//__}"     # flatten path for filesystem-friendly names
  local lvl
  for lvl in O0 O2; do
    local ir="$IR_DIR/${base}.${lvl}.ll"
    local json="$OUT_DIR/${base}.${lvl}.json"

    # compile to IR. Polybench programs need:
    #   -I to find polybench.h (lives at SingleSource/.../Polybench/utilities)
    #   -DFP_ABSTOLERANCE / -DPOLYBENCH_DUMP_ARRAYS / -DSMALL_DATASET
    # These are the standard flags from the test-suite's CMakeLists; we
    # apply them unconditionally — non-Polybench programs ignore them.
    if ! "$CLANG" "${CLANG_FLAGS[@]}" \
            -I "$SUITE/Polybench/utilities" \
            -DFP_ABSTOLERANCE=1e-5 \
            -DPOLYBENCH_DUMP_ARRAYS \
            -DSMALL_DATASET \
            -ffp-contract=off \
            -"$lvl" -S -emit-llvm "$src" -o "$ir" \
            -w 2>>"$LOG"; then
      echo "skip-ir   $rel  @ -$lvl" >> "$LOG"
      return 1
    fi

    # run pass
    if ! "$OPT" -enable-new-pm=0 \
            -load "$PASS_LIB" \
            -plumb \
            -plumb-weight-file="$ROOT/config/weights.cfg" \
            -plumb-hot-threshold=30 \
            -plumb-inline-threshold=20 \
            -plumb-run-label="$lvl" \
            -plumb-json-file="$json" \
            -disable-output "$ir" >/dev/null 2>>"$LOG"; then
      echo "skip-pass $rel  @ -$lvl" >> "$LOG"
      return 1
    fi
  done
  return 0
}

# Simple parallel job runner using xargs.
export -f run_one
export CLANG OPT PASS_LIB SUITE IR_DIR OUT_DIR LOG ROOT
# CLANG_FLAGS is an array; flatten for export.
export CLANG_FLAGS_STR="${CLANG_FLAGS[*]:-}"

i=0
for src in "${PROGRAMS[@]}"; do
  i=$((i+1))
  rel="${src#$SUITE/}"
  printf "[%3d/%d] %-60s " "$i" "$TOTAL" "$rel"
  if run_one "$src"; then
    printf "✓\n"
    OK=$((OK+1))
  else
    printf "✗ (see benchmarks/run.log)\n"
    FAIL=$((FAIL+1))
  fi
done

END=$(date +%s)
echo
echo "──────────────────────────────────────────"
echo "  ok      : $OK"
echo "  failed  : $FAIL"
echo "  elapsed : $((END - START)) s"
echo "  results : $OUT_DIR"
echo "  log     : $LOG"
