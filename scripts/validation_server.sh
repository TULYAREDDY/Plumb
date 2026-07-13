#!/usr/bin/env bash
# scripts/validation_server.sh
# ─────────────────────────────────────────────────────────────────────────
# Starts the LIVE recommendation-validation server: a local dev server that
# lets the dashboard actually run LLVM transforms (-always-inline,
# -loop-vectorize, -tailcallelim) against whatever program you pick or
# upload — not just Plumb's own 3 testcases. See scripts/validation_server.py.
#
# Usage: ./scripts/validation_server.sh [--no-open] [-p PORT]
# ─────────────────────────────────────────────────────────────────────────
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# shellcheck source=scripts/_llvm_env.sh
source "$ROOT/scripts/_llvm_env.sh"

PORT=8420
OPEN_BROWSER=1
while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-open) OPEN_BROWSER=0; shift ;;
    -p) PORT="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--no-open] [-p PORT]"
      exit 0 ;;
    *) shift ;;
  esac
done

# ── locate the built pass (build it if missing, matching run.sh) ───────
find_pass_lib
if [ -z "$PASS_LIB" ]; then
  echo -e "${YEL}pass not built yet — running ./build.sh first…${OFF}"
  "$ROOT/build.sh"
  find_pass_lib
  [ -n "$PASS_LIB" ] || die "build did not produce a loadable pass"
fi

# ── make sure the 3 shipped testcases are analyzed, so the picker isn't empty ──
[ -f "$ROOT/ir/test_arith.O0.ll" ] || "$ROOT/run.sh" --no-open

CLANG_FLAGS_JSON=$(python3 -c "import json,sys; print(json.dumps(sys.argv[1:]))" "${CLANG_FLAGS[@]}")

banner "Starting Plumb live-validation server"
echo -e "  ${B}URL:${OFF}  http://localhost:${PORT}/dashboard.html"
echo -e "  ${B}Stop:${OFF} Ctrl-C"
echo

if [ "$OPEN_BROWSER" = 1 ]; then
  ( sleep 1
    if command -v open >/dev/null 2>&1; then open "http://localhost:${PORT}/dashboard.html"
    elif command -v xdg-open >/dev/null 2>&1; then xdg-open "http://localhost:${PORT}/dashboard.html" >/dev/null 2>&1
    fi ) &
fi

CLANG="$CLANG" OPT="$OPT" PASS_LIB="$PASS_LIB" ROOT="$ROOT" PORT="$PORT" \
    CLANG_FLAGS_JSON="$CLANG_FLAGS_JSON" \
    python3 "$ROOT/scripts/validation_server.py"
