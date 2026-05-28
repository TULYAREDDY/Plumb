# IMPLEMENTATION — LLVM-API specifics

This document covers the **LLVM-side** implementation: which APIs we
use, why, and the build-system gotchas we hit. The *why* of the design
choices lives in `DESIGN.md`; *measured behaviour* lives in
`EVALUATION.md`.

---

## 1. Pass shape

`src/Plumb.cpp` defines a single `FunctionPass` registered
with the legacy pass manager:

```cpp
struct Plumb : public FunctionPass {
    static char ID;
    Plumb() : FunctionPass(ID) {}

    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<LoopInfoWrapperPass>();
        AU.setPreservesAll();
    }

    bool runOnFunction(Function &F) override { /* … */ return false; }
};

char Plumb::ID = 0;

static RegisterPass<Plumb>
    X("plumb",
      "Plumb · LLVM-IR Cost Analysis",
      false,        // CFGOnly  = false (we read instructions)
      true          // IsAnalysis = true (we don't modify IR)
    );
```

`runOnFunction` returns `false` because we never mutate the IR — we
declare ourselves an analysis (`IsAnalysis=true`) and preserve all
analyses (`AU.setPreservesAll()`). This means we can safely sit in
any pass pipeline and the inliner / SROA / etc. don't get invalidated.

---

## 2. Required analyses

| Analysis | Used for |
|---|---|
| `LoopInfoWrapperPass` | `LI.getLoopDepth(BB)` for the depth multiplier; `LI.begin()…LI.end()` for top-level loop count. |

We deliberately do **not** depend on `BlockFrequencyAnalysis`,
`BranchProbabilityAnalysis`, or `ScalarEvolution` — see `DESIGN.md` §3
for the rationale (we want a profile-free, deterministic model).

---

## 3. Instruction classification

`classifyInst(const Instruction&)` returns a `{group, isIndirectCall}`
pair. The classifier is a single switch on `Instruction::getOpcode()`
that buckets opcodes into 9 groups:

| Group | LLVM opcodes |
|---|---|
| `add`     | `Add`, `Sub`, `FAdd`, `FSub`, plus `And/Or/Xor/Shl/Lshr/Ashr` |
| `mul`     | `Mul`, `FMul`, `SDiv`, `UDiv`, `FDiv`, `Rem` variants |
| `memory`  | `Load`, `Store`, `AtomicCmpXchg`, `AtomicRMW`, `Fence` |
| `call`    | `Call`, `Invoke`. `isIndirectCall = (CallBase::getCalledFunction() == nullptr)` |
| `branch`  | `Br`, `Switch`, `IndirectBr` |
| `compare` | `ICmp`, `FCmp` |
| `cast`    | every `CastInst` opcode (`SExt`, `ZExt`, `Trunc`, `BitCast`, `FPExt`, …) |
| `alloca`  | `Alloca` |
| `phi`     | `PHI` |
| `other`   | catch-all (`Ret`, `GetElementPtr`, `Select`, `Unreachable`, …) |

The classifier is intentionally short (~40 LoC) so it's easy to inspect
and re-bucket if a target's cost reality demands it.

### Indirect-call detection

```cpp
if (const CallBase *CB = dyn_cast<CallBase>(&I)) {
    isIndirectCall = (CB->getCalledFunction() == nullptr);
}
```

`getCalledFunction()` returns the callee's `Function*` for a direct
call and `nullptr` for indirect dispatch (function pointers, vtables,
inline asm calls). When the call is indirect, the per-instruction
cost is multiplied by 1.6 (rounded down to int):

```cpp
int effectiveWeight = baseWeight;
if (group == "call" && CR.isIndirectCall)
    effectiveWeight = (int)(baseWeight * 1.6);
long cost = (long)effectiveWeight * depthMul;
```

---

## 4. Per-block cost and CFG bookkeeping

For every `BasicBlock &BB : F` we keep four parallel maps keyed by
`BasicBlock*`:

```cpp
std::map<BasicBlock*, long>        bbCostByPtr;
std::map<BasicBlock*, int>         bbInstByPtr;
std::map<BasicBlock*, unsigned>    bbDepthByPtr;
std::map<BasicBlock*, std::string> bbLabelByPtr;
```

Pointer-keyed maps make the critical-path DP and successor traversal
clean. Labels are derived as `BB.getName().str()` if present,
otherwise `"bb." + std::to_string(index)` so unnamed `-O0` IR still
gets stable names in the JSON.

### Loop depth

```cpp
unsigned loopDepth = LI.getLoopDepth(&BB);
unsigned depthMul  = (loopDepth > 0) ? loopDepth : 1;
```

`getLoopDepth` returns 0 for blocks not in any loop and ≥ 1 inside a
loop. We clamp to ≥ 1 so straight-line code costs the natural per-
instruction weight (not zero).

### Cyclomatic complexity

McCabe's classical formula on the CFG:

```cpp
int N = (int)bbCostByPtr.size();
int M = (N > 0) ? (cfgEdges - N + 2) : 0;   // M = E - N + 2
```

`cfgEdges` is incremented per BB by `BB.getTerminator()->getNumSuccessors()`.
Switch instructions correctly contribute one edge per case, which is
why `test_branchy::categorize` lands at CC = 23 at -O0 (a 13-case
switch + a default ladder).

### Recursion

Direct self-call detection during the per-instruction pass:

```cpp
if (group == "call") {
    if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
        Function *callee = CI->getCalledFunction();
        if (callee && callee == &F) isRecursive = true;
    }
}
```

Indirect recursion (`A → B → A` where one edge is a function pointer)
is **not** detected; this would require interprocedural analysis we've
explicitly declared out of scope.

---

## 5. Critical-path DP

`computeCriticalPath(F, bbCostByPtr)` runs an O(V + E) DP over the
CFG with back-edges removed.

1. **Reverse post-order numbering** — built by a depth-first walk from
   `&F.getEntryBlock()` using a manual stack (no recursion to keep the
   call stack bounded on huge IRs).
2. **Forward edges only** — when relaxing `dp[succ]`, we ignore any
   edge `BB → succ` where `succ` has a smaller RPO number than `BB`
   (that's the back-edge of a loop).
3. **DP relaxation** — for each `BB` in RPO:
   `dp[BB] = cost(BB) + max(dp[pred] for forward predecessors of BB)`
   with `dp[entry] = cost(entry)`.
4. **Backtrack** — find `argmax(dp)`, walk back via the recorded parent
   pointers, reverse the list.

This produces both the path (`std::vector<BasicBlock*>`) and the cost
(`dp[end]`), which we emit to JSON as `criticalPath` (array of labels)
and `criticalPathCost` (integer).

---

## 6. Energy estimate

A second small fixed table (`g_energyPj`) maps each group to a per-op
pJ figure inspired by Horowitz, ISSCC 2014. For each instruction we
add `count * energyPj[group]` (no depth multiplier — the depth
multiplier is for *static* asymptotic cost, energy is a per-op figure).
Per-function energy is summed and emitted as `energyPj` in the JSON;
the metadata block reports `estimatedEnergyPj` (sum across functions).

The energy figures are deliberately decoupled from the user-tunable
`weights.cfg`. The dashboard's live-weight tuner can change the cost
ranking, but the energy ranking stays anchored to a (rough) physical
reality.

---

## 7. Output emission

Three sinks, all from one in-memory `std::vector<FuncInfo> g_funcs`:

* **Terminal** — `errs()` writes the per-function ASCII tables and the
  bar chart inside `runOnFunction`. The final ranking table is printed
  from a small `doFinalization`-style block by checking whether `&F`
  is the last function in the module.
* **CSV** — `g_csvLines` accumulates one row per (function, group)
  during the per-function pass; written once at the end if
  `-plumb-out-file=` was given.
* **JSON** — same idea, but we keep the *structured* `g_funcs` vector
  and serialize it once at the end. JSON strings are escaped via a
  small `jsonEscape()` helper (handles `\`, `"`, control chars).

This "accumulate, then emit at the last function" approach makes the
pass run cleanly under `opt -disable-output` (no module modification)
while still producing module-level artifacts.

---

## 8. Command-line flags

All flags use the `plumb-` prefix:

```cpp
static cl::opt<std::string> WeightFile      ("plumb-weight-file", …);
static cl::opt<int>         HotThreshold    ("plumb-hot-threshold", …);
static cl::opt<int>         InlineThreshold ("plumb-inline-threshold", …);
static cl::opt<std::string> OutFile         ("plumb-out-file", …);
static cl::opt<std::string> JsonFile        ("plumb-json-file", …);
static cl::opt<std::string> RunLabel        ("plumb-run-label", …);
```

### Why the prefix

LLVM 14 itself defines `-inline-threshold` as a built-in `cl::opt` for
its inliner pass. When a plugin registers a `cl::opt` with the same
name, LLVM aborts at `dlopen` time with:

```
inconsistency in registered CommandLine options
```

inside `cl::CommandLineParser::addOption`. Namespacing every flag with
`plumb-` (Plumb) avoids any present or future
collision.

---

## 9. Build-system specifics (`src/CMakeLists.txt`)

### Refusing LLVM ≥ 17

```cmake
if(LLVM_VERSION_MAJOR GREATER_EQUAL 17)
  message(FATAL_ERROR
    "This project targets the LLVM legacy pass manager and needs "
    "LLVM 14–16, but found LLVM ${LLVM_PACKAGE_VERSION}. …")
endif()
```

Better to fail fast with a clear message than dump a wall of template
errors when `FunctionPass` no longer exists.

### macOS plugin linkage — `dynamic_lookup`

This is the most subtle build issue in the project. On macOS, if we
link the plugin against `libLLVM.dylib` directly, then at `opt(1)`
runtime libLLVM is loaded **twice**:

1. once by `opt` itself,
2. once via `dlopen()` of our plugin.

Each copy registers its own copy of every `cl::opt<>` global, which
trips `CommandLineParser::addOption`'s internal sanity check and aborts
the process before our pass even runs.

The fix — leave LLVM symbols **unresolved** at plugin link time and
rely on the dynamic loader to bind them to the copy already inside
`opt`:

```cmake
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
    llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis IRReader Passes)
    target_link_libraries(Plumb PRIVATE ${LLVM_LIBS})
  endif()
endif()
```

Linux uses standard linkage because its `dlopen` semantics
(`RTLD_GLOBAL` merging symbol tables) avoid the double-registration
problem.

### Both C and CXX in `project()`

```cmake
project(Plumb C CXX)
```

LLVM's CMake config invokes `check_include_file()` (a C-language check)
on some versions — particularly the LibEdit probe on LLVM ≥ 17 — and
that fails if only CXX is enabled. Listing both languages costs us
nothing and avoids a confusing CMake configure error on certain
Homebrew installs.

### RTTI

```cmake
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
```

Linux distro LLVM packages and Homebrew LLVM are usually built with
`-fno-rtti`. If we build the plugin with RTTI on, RTTI symbols collide
between the plugin and libLLVM at runtime, causing `dynamic_cast` and
`typeid` to misbehave. Matching libLLVM's setting is the safe default.

---

## 10. Toolchain detection (`scripts/_llvm_env.sh`)

Both `build.sh` and `run.sh` source this helper. It tries, in order:

1. `$LLVM_DIR` if already set and contains `LLVMConfig.cmake`.
2. `llvm-config-{14,15,16}` and `llvm-config@{14,15,16}` on `$PATH`,
   then unsuffixed `llvm-config`. For each candidate, it reads
   `LLVMConfig.cmake` to verify the major version is in {14, 15, 16}.
3. Well-known prefixes — `brew --prefix llvm@{14,15,16}` on macOS,
   `/usr/lib/llvm-{14,15,16}/lib/cmake/llvm` on Linux.

It then picks `clang` and `opt` from the **same prefix** to guarantee
ABI compatibility between the IR producer and the analysis runner.

On macOS, it also exports `-isysroot $(xcrun --show-sdk-path)` into a
`CLANG_FLAGS` array because Homebrew clang doesn't know where the
Xcode SDK headers live without that hint.

---

## 11. Portability notes

* Stock macOS bash is **3.2** (Apple stopped upgrading it for licence
  reasons). We avoid bash 4 features — most importantly `mapfile`. The
  `run.sh` uses a `while read; … done < <(find …)` loop instead.
* The plugin extension differs by platform: `.so` on Linux, `.dylib`
  on macOS. Both scripts probe for either suffix when locating the
  built library.
* `getconf _NPROCESSORS_ONLN` works on both Linux and macOS;
  `sysctl -n hw.ncpu` is the macOS-only fallback. We try both.
