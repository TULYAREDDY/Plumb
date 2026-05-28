#!/usr/bin/env bash
# run.sh — Run the Plumb pass on every testcase, at -O0 and -O2.
# ──────────────────────────────────────────────────────────────────────────
# Produces:
#   ir/<name>.{O0,O2}.ll        LLVM IR
#   results/<name>.{O0,O2}.csv  CSV breakdown
#   results/<name>.{O0,O2}.json structured JSON for the dashboard
#
# Then opens dashboard/dashboard.html in your default browser.
# Use `./run.sh --no-open` to skip the browser launch (useful in CI).
# ──────────────────────────────────────────────────────────────────────────
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
# shellcheck source=scripts/_llvm_env.sh
source "$ROOT/scripts/_llvm_env.sh"

OPEN_DASHBOARD=1
for arg in "$@"; do
  case "$arg" in
    --no-open) OPEN_DASHBOARD=0 ;;
    -h|--help)
      echo "Usage: $0 [--no-open]"
      echo "  --no-open   build & analyse only; do not open the dashboard"
      exit 0 ;;
  esac
done

# ── locate the built pass library ───────────────────────────────────────
PASS_LIB=""
for cand in "$ROOT/build/libPlumb.${PLUGIN_EXT}" \
            "$ROOT/build/libPlumb.so" \
            "$ROOT/build/libPlumb.dylib"; do
  [ -f "$cand" ] && PASS_LIB="$cand" && break
done

if [ -z "$PASS_LIB" ]; then
  echo -e "${YEL}pass not built yet — running ./build.sh first…${OFF}"
  "$ROOT/build.sh"
  for cand in "$ROOT/build/libPlumb.${PLUGIN_EXT}" \
              "$ROOT/build/libPlumb.so" \
              "$ROOT/build/libPlumb.dylib"; do
    [ -f "$cand" ] && PASS_LIB="$cand" && break
  done
  [ -n "$PASS_LIB" ] || die "build did not produce a loadable pass"
fi

WEIGHTS="$ROOT/config/weights.cfg"
[ -f "$WEIGHTS" ] || die "missing weight config: $WEIGHTS"

# ── compile every testcase to IR at -O0 and -O2 ─────────────────────────
banner "Compiling testcases to LLVM IR (-O0 and -O2)"
mkdir -p ir results

# enumerate testcases/*.c — sorted, so demo output is reproducible.
# (avoid bash 4 mapfile so this works on stock macOS bash 3.2 too)
TEST_FILES=()
while IFS= read -r f; do TEST_FILES+=("$f"); done < <(find "$ROOT/testcases" -maxdepth 1 -name '*.c' | sort)
[ "${#TEST_FILES[@]}" -gt 0 ] || die "no testcases found in testcases/"

for src in "${TEST_FILES[@]}"; do
  base=$(basename "$src" .c)
  $CLANG "${CLANG_FLAGS[@]}" -O0 -S -emit-llvm "$src" -o "ir/${base}.O0.ll" 2>/dev/null
  $CLANG "${CLANG_FLAGS[@]}" -O2 -S -emit-llvm "$src" -o "ir/${base}.O2.ll" 2>/dev/null
  echo -e "  ${GREEN}✓${OFF} $(basename "$src")  →  ir/${base}.{O0,O2}.ll"
done

# ── run the pass on each (testcase, opt-level) pair ─────────────────────
banner "Running Plumb analysis"
for src in "${TEST_FILES[@]}"; do
  base=$(basename "$src" .c)
  for lvl in O0 O2; do
    echo -e "${B}── ${base} @ -${lvl} ───────────────────────────────${OFF}"
    "$OPT" -enable-new-pm=0 \
        -load "$PASS_LIB" \
        -plumb \
        -plumb-weight-file="$WEIGHTS" \
        -plumb-hot-threshold=30 \
        -plumb-inline-threshold=20 \
        -plumb-run-label="$lvl" \
        -plumb-out-file="results/${base}.${lvl}.csv" \
        -plumb-json-file="results/${base}.${lvl}.json" \
        -disable-output "ir/${base}.${lvl}.ll" || true
  done
done

# ── open the dashboard ──────────────────────────────────────────────────
DEFAULT_A="results/test_floatmm.O0.json"
DEFAULT_B="results/test_floatmm.O2.json"
[ -f "$DEFAULT_A" ] || DEFAULT_A="results/test_arith.O0.json"
[ -f "$DEFAULT_B" ] || DEFAULT_B="results/test_arith.O2.json"

banner "Done"
echo -e "${GREEN}Generated reports:${OFF}"
ls -1 results/*.json 2>/dev/null | sed 's/^/  /'
echo
echo -e "${B}Dashboard:${OFF}  $ROOT/dashboard/dashboard.html"
echo -e "${B}Suggested:${OFF}  load A = ${DEFAULT_A}"
echo -e "             load B = ${DEFAULT_B}"
echo

if [ "$OPEN_DASHBOARD" = 1 ]; then
  if command -v open >/dev/null 2>&1; then
    open "$ROOT/dashboard/dashboard.html"
  elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$ROOT/dashboard/dashboard.html" >/dev/null 2>&1 &
  else
    echo -e "${YEL}(no browser launcher found — open dashboard/dashboard.html manually)${OFF}"
  fi
fi
