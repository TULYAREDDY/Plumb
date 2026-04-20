# Weighted Instruction Frequency LLVM Pass

## Overview

This project implements a custom LLVM analysis pass to compute the weighted instruction frequency of programs at the Intermediate Representation (IR) level. The pass analyzes each function, classifies instructions into predefined categories, and computes a cost model based on configurable weights. The objective is to provide a static approximation of computational expense and identify potential performance bottlenecks.

---

## Motivation

Traditional instruction counting does not reflect the actual computational cost of a program, as different instruction types have varying execution overheads. For example, memory operations and function calls are generally more expensive than arithmetic instructions. This project introduces a weighted model that assigns relative costs to instruction categories, enabling a more meaningful performance analysis and aiding in hotspot detection.

---

## Architecture

The system follows a standard LLVM-based analysis pipeline:

1. Source code is compiled into LLVM IR using Clang.
2. The LLVM IR is processed using the `opt` tool.
3. The custom pass is dynamically loaded as a shared library.
4. The pass analyzes functions, basic blocks, and instruction types.
5. Results are generated in both terminal output and CSV format.

The pass operates using the legacy pass manager to ensure compatibility with the chosen LLVM version.

---

## Tech Stack

* C++ (LLVM Pass Implementation)
* LLVM Framework (Version 14)
* Clang (Frontend for IR generation)
* CMake (Build system)
* Linux (Development environment)

---

## How to Run

### Build the Pass

```bash
mkdir build
cd build
cmake -DLLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm ..
make
```

### Generate LLVM IR

```bash
clang -O0 -S -emit-llvm test.c -o test.ll
```

### Execute the Pass

```bash
opt-14 -enable-new-pm=0 \
-load ./libWeightedInstFreq.so \
-weighted-inst-freq \
-weight-file=../weights.cfg \
-hot-threshold=30 \
-out-file=results.csv \
-disable-output ../test.ll
```

---

## Output

### Terminal Output

* Instruction classification by category
* Count, weight, and computed cost
* Contribution percentage of each category
* Per-basic-block cost distribution
* Function-level summaries
* Hotspot warnings based on threshold

### CSV Output

The CSV file contains structured data in the following format:

```
function,group,count,weight,cost,pct
```

This output can be used for further analysis or visualization.

---

## Project Structure

```
weighted-inst-freq/
├── WeightedInstFreq.cpp
├── CMakeLists.txt
├── test.c
├── weights.cfg
├── README.md
└── .gitignore
```

---

## Key Features

* Instruction classification into meaningful categories
* Configurable weight-based cost model
* Function-level and basic block-level analysis
* Hotspot detection using threshold-based evaluation
* Structured CSV output for downstream processing
* Compatibility with LLVM legacy pass manager
