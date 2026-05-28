#!/usr/bin/env bash
# scripts/_llvm_env.sh
# ─────────────────────────────────────────────────────────────────
#  Shared LLVM toolchain detection. Sourced by build.sh and run.sh.
#  Exports:  LLVM_DIR, LLVM_PREFIX, LLVM_BIN, CLANG, OPT,
#            PLATFORM, PLUGIN_EXT, CLANG_FLAGS (array),
#            and the colour helpers B/CYAN/GREEN/YEL/RED/OFF.
#
#  This project targets the LEGACY pass-manager API (FunctionPass,
#  RegisterPass) which was removed in LLVM 17 — so we prefer LLVM 14
#  and accept 15/16 as a fallback. Newer LLVM is rejected by the
#  CMake configure step with a helpful diagnostic.
# ─────────────────────────────────────────────────────────────────

# colour helpers
B="\033[1m"; CYAN="\033[36m"; GREEN="\033[32m"; YEL="\033[33m"; RED="\033[31m"; OFF="\033[0m"

banner() {
  echo
  echo -e "${CYAN}┌──────────────────────────────────────────────────────────────┐${OFF}"
  echo -e "${CYAN}│${OFF}  ${B}$1${OFF}"
  echo -e "${CYAN}└──────────────────────────────────────────────────────────────┘${OFF}"
}

die() { echo -e "${RED}✗ $1${OFF}" >&2; exit 1; }

# ── platform ─────────────────────────────────────────────────────
UNAME_S=$(uname -s)
case "$UNAME_S" in
  Linux*)   PLATFORM=linux;   PLUGIN_EXT=so    ;;
  Darwin*)  PLATFORM=macos;   PLUGIN_EXT=dylib ;;
  *)        PLATFORM=other;   PLUGIN_EXT=so    ;;
esac

# ── version policy ──────────────────────────────────────────────
LLVM_VERSION_PREF=(14 15 16)

is_supported_version() {
  local major="${1%%.*}"
  case "$major" in 14|15|16) return 0 ;; *) return 1 ;; esac
}

read_llvm_version() {
  awk '/set\(LLVM_PACKAGE_VERSION/ {gsub(/[")(]/,""); print $2; exit}' \
      "$1/LLVMConfig.cmake" 2>/dev/null
}

find_llvm() {
  if [ -n "$LLVM_DIR" ] && [ -f "$LLVM_DIR/LLVMConfig.cmake" ]; then return 0; fi

  local candidates=()
  for v in "${LLVM_VERSION_PREF[@]}"; do
    candidates+=("llvm-config-$v" "llvm-config@$v")
  done
  candidates+=("llvm-config")
  for binary in "${candidates[@]}"; do
    if command -v "$binary" >/dev/null 2>&1; then
      local cand="$($binary --prefix)/lib/cmake/llvm"
      if [ -f "$cand/LLVMConfig.cmake" ]; then
        local ver; ver="$(read_llvm_version "$cand")"
        if is_supported_version "$ver"; then export LLVM_DIR="$cand"; return 0; fi
      fi
    fi
  done

  local PROBE=()
  if [ "$PLATFORM" = macos ]; then
    if command -v brew >/dev/null 2>&1; then
      for v in "${LLVM_VERSION_PREF[@]}"; do
        local p; p="$(brew --prefix "llvm@$v" 2>/dev/null || true)"
        [ -n "$p" ] && PROBE+=("$p/lib/cmake/llvm")
      done
      local pu; pu="$(brew --prefix llvm 2>/dev/null || true)"
      [ -n "$pu" ] && PROBE+=("$pu/lib/cmake/llvm")
    fi
    PROBE+=(
      /opt/homebrew/opt/llvm@14/lib/cmake/llvm
      /opt/homebrew/opt/llvm@15/lib/cmake/llvm
      /opt/homebrew/opt/llvm@16/lib/cmake/llvm
      /usr/local/opt/llvm@14/lib/cmake/llvm
      /usr/local/opt/llvm/lib/cmake/llvm
      /opt/homebrew/opt/llvm/lib/cmake/llvm
    )
  else
    PROBE+=(
      /usr/lib/llvm-14/lib/cmake/llvm
      /usr/lib/llvm-15/lib/cmake/llvm
      /usr/lib/llvm-16/lib/cmake/llvm
      /usr/share/llvm-14/cmake
    )
  fi
  for c in "${PROBE[@]}"; do
    if [ -f "$c/LLVMConfig.cmake" ]; then
      local ver; ver="$(read_llvm_version "$c")"
      if is_supported_version "$ver"; then export LLVM_DIR="$c"; return 0; fi
    fi
  done
  for c in "${PROBE[@]}"; do
    if [ -f "$c/LLVMConfig.cmake" ]; then export LLVM_DIR="$c"; return 0; fi
  done
  return 1
}

if ! find_llvm; then
  echo
  echo -e "${RED}LLVM development files not found.${OFF}"
  echo
  if [ "$PLATFORM" = macos ]; then
    echo -e "${YEL}Install on macOS:${OFF}"
    echo "    brew install llvm@14"
    echo "    export LLVM_DIR=\$(brew --prefix llvm@14)/lib/cmake/llvm"
  else
    echo -e "${YEL}Install on Ubuntu/Debian:${OFF}"
    echo "    sudo apt install llvm-14 llvm-14-dev clang-14 cmake"
    echo "    export LLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm"
  fi
  exit 1
fi

# ── pick clang/opt from the same prefix ─────────────────────────
LLVM_PREFIX="$(dirname "$(dirname "$(dirname "$LLVM_DIR")")")"
LLVM_BIN="$LLVM_PREFIX/bin"

pick_tool() {
  if [ -x "$LLVM_BIN/$1" ]; then echo "$LLVM_BIN/$1"; return; fi
  for s in -18 -17 -16 -15 -14 ""; do
    if command -v "$1$s" >/dev/null 2>&1; then echo "$1$s"; return; fi
  done
  echo "$1"
}

CLANG=${CLANG:-$(pick_tool clang)}
OPT=${OPT:-$(pick_tool opt)}

command -v "$CLANG" >/dev/null 2>&1 || die "clang not found (tried $CLANG)"
command -v "$OPT"   >/dev/null 2>&1 || die "opt not found (tried $OPT)"

# macOS clang needs the SDK path for libc headers
CLANG_FLAGS=()
if [ "$PLATFORM" = macos ] && command -v xcrun >/dev/null 2>&1; then
  SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
  [ -n "$SDK_PATH" ] && CLANG_FLAGS+=( -isysroot "$SDK_PATH" )
fi

export LLVM_DIR LLVM_PREFIX LLVM_BIN CLANG OPT PLATFORM PLUGIN_EXT
