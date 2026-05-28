#!/usr/bin/env bash
# build.sh — Build the Plumb LLVM pass.
# ─────────────────────────────────────────────────────────────────
# Detects an LLVM 14/15/16 install, configures CMake, and produces
#   build/libPlumb.{so|dylib}
# Run ./run.sh afterwards to execute the pass on the testcases.
# ─────────────────────────────────────────────────────────────────
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
# shellcheck source=scripts/_llvm_env.sh
source "$ROOT/scripts/_llvm_env.sh"

echo -e "${YEL}toolchain:${OFF}"
echo "  LLVM_DIR = $LLVM_DIR"
echo "  clang    = $CLANG  ($("$CLANG" --version | head -1))"
echo "  opt      = $OPT"
echo "  platform = $PLATFORM (.${PLUGIN_EXT})"

banner "Building Plumb pass"
mkdir -p build
cd build
if [ ! -f Makefile ] || [ ! -f CMakeCache.txt ]; then
  cmake -DLLVM_DIR="$LLVM_DIR" "$ROOT/src" >/dev/null
fi
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
make -j"$JOBS"

PASS_LIB=""
for cand in "$ROOT/build/libPlumb.${PLUGIN_EXT}" \
            "$ROOT/build/libPlumb.so" \
            "$ROOT/build/libPlumb.dylib"; do
  [ -f "$cand" ] && PASS_LIB="$cand" && break
done
[ -n "$PASS_LIB" ] || die "build failed: pass library not produced"

cd "$ROOT"
echo
echo -e "${GREEN}✓ pass built:${OFF} $PASS_LIB"
echo -e "${B}next:${OFF} ./run.sh"
