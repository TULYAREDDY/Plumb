cmake_minimum_required(VERSION 3.13)

project(WeightedInstFreq)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig from: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})

# ⚠️ REMOVE add_definitions(${LLVM_DEFINITIONS})

add_library(WeightedInstFreq MODULE WeightedInstFreq.cpp)

# ✅ Force dynamic linking to LLVM shared lib
target_link_libraries(WeightedInstFreq PRIVATE LLVM-14)

