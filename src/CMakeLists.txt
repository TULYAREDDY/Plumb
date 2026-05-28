cmake_minimum_required(VERSION 3.13)
# Enable BOTH C and CXX — LLVM's CMake config invokes
# check_include_file() (a C-language check) on some versions
# (LibEdit probe on LLVM ≥ 17), which fails if only CXX is enabled.
project(Plumb C CXX)

# ── Find LLVM ────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig from: ${LLVM_DIR}")

# This pass uses the LEGACY pass-manager API (FunctionPass, RegisterPass).
# That API was removed in LLVM 17. Refuse to build against newer LLVM
# with a clear message instead of dumping a wall of template errors.
if(LLVM_VERSION_MAJOR GREATER_EQUAL 17)
  message(FATAL_ERROR
    "This project targets the LLVM legacy pass manager and needs LLVM 14–16, "
    "but found LLVM ${LLVM_PACKAGE_VERSION}.\n"
    "Install LLVM 14 and point CMake at it:\n"
    "  brew install llvm@14   # macOS\n"
    "  export LLVM_DIR=$(brew --prefix llvm@14)/lib/cmake/llvm\n"
    "  rm -rf build && ./demo.sh")
endif()

# ── Build flags ──────────────────────────────────────────────
# LLVM 14 builds fine as C++14 or C++17.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Match LLVM's RTTI / exceptions setting; Linux LLVM packages are
# usually built with -fno-rtti, Homebrew on macOS is the same.
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

# ── Build the loadable pass module ───────────────────────────
add_library(Plumb MODULE Plumb.cpp)

target_include_directories(Plumb SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(Plumb PRIVATE ${LLVM_DEFINITIONS_LIST})

# ── Link to LLVM ─────────────────────────────────────────────
# IMPORTANT: on macOS we must NOT link the plugin against libLLVM.dylib.
# Doing so causes libLLVM to be loaded twice at runtime (once by opt(1),
# once by our dlopen), which double-registers all cl::opt<> globals and
# aborts inside `CommandLineParser::addOption`.
#
# Instead we leave LLVM symbols unresolved at link time and rely on
# -Wl,-undefined,dynamic_lookup so they bind to the copy already loaded
# inside opt(1) at dlopen time.
#
# Linux/Ubuntu does not have this problem (RTLD_GLOBAL semantics merge
# symbol tables), so we link normally there.
if(APPLE)
  set_target_properties(Plumb PROPERTIES
    LINK_FLAGS "-Wl,-undefined,dynamic_lookup"
    SUFFIX ".dylib")
else()
  if(TARGET LLVM)
    target_link_libraries(Plumb PRIVATE LLVM)
  elseif(TARGET LLVM-${LLVM_VERSION_MAJOR})
    target_link_libraries(Plumb PRIVATE LLVM-${LLVM_VERSION_MAJOR})
  else()
    llvm_map_components_to_libnames(LLVM_LIBS
      Core Support Analysis IRReader Passes)
    target_link_libraries(Plumb PRIVATE ${LLVM_LIBS})
  endif()
endif()
