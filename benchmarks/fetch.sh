#!/usr/bin/env bash
# benchmarks/fetch.sh
# ─────────────────────────────────────────────────────────────────
#  Sparse-clone the SingleSource subset of LLVM's official test-suite
#  into benchmarks/llvm-test-suite/. We don't need the whole repo (it's
#  ~1 GB); SingleSource is the part where each program is a single .c
#  or .cpp file, which is exactly what Plumb wants to eat.
#
#  Run once. Idempotent — re-running just `git pull`s.
# ─────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
TARGET="$ROOT/llvm-test-suite"

if [ -d "$TARGET/.git" ]; then
  echo "⟲ updating existing checkout at $TARGET"
  git -C "$TARGET" pull --ff-only
  exit 0
fi

echo "⌁ sparse-cloning llvm/llvm-test-suite (SingleSource subset only)…"
mkdir -p "$TARGET"

git -C "$TARGET" init -q
git -C "$TARGET" remote add origin https://github.com/llvm/llvm-test-suite.git
git -C "$TARGET" config core.sparseCheckout true

# Only fetch the small-self-contained subset — about 60 programs total
# across these three groups.
SPARSE="$TARGET/.git/info/sparse-checkout"
{
  echo 'SingleSource/Benchmarks/Stanford/'
  echo 'SingleSource/Benchmarks/Misc/'
  echo 'SingleSource/Benchmarks/Polybench/'
  echo 'SingleSource/Benchmarks/Shootout/'
  echo 'README*'
  echo 'LICENSE*'
} > "$SPARSE"

# Shallow + filter to keep this fast.
git -C "$TARGET" fetch --depth 1 --filter=blob:none origin main
git -C "$TARGET" checkout main

echo "✓ checked out → $TARGET"
echo
echo "programs found:"
find "$TARGET/SingleSource/Benchmarks" -name '*.c' -o -name '*.cpp' | wc -l
