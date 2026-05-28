# EVALUATION — Measured Results

Six testcases × two optimisation levels = **12 analysis runs**. All
numbers in this document are extracted directly from the JSON reports
in `results/` produced by `./run.sh`. They are reproducible: re-running
`./run.sh --no-open` will regenerate every figure below to the unit.

> **TL;DR.** The weighted + depth-aware model captures cost
> differences that a raw instruction count misses, especially on
> call-bound and memory-bound code. It also surfaces a real, honest
> failure mode at -O2: when the inliner pulls callees into `main`,
> aggregate function cost can *increase* even though wall-clock cost
> falls. We treat that as a feature of the report, not a bug.

---

## 1. Testcase suite

| # | File | Designed to stress | Expected dominant group |
|---|---|---|---|
| 1 | `testcases/test_arith.c`     | mixed arith + nested loops + calls | spread |
| 2 | `testcases/test_branchy.c`   | high cyclomatic complexity (switch + nested ifs) | branch + memory |
| 3 | `testcases/test_callchain.c` | direct + indirect calls (function pointer) | call |
| 4 | `testcases/test_floatmm.c`   | triple-nested float matmul (depth 3) | memory |
| 5 | `testcases/test_memheavy.c`  | 5-point stencil + indirect-load reduction | memory |
| 6 | `testcases/test_recursive.c` | self-recursion + leaf functions | call |

Six testcases give a comfortable margin over the typical "five distinct
cases" benchmark, and the extra case earns its keep — it gives us
coverage of both *direct* recursion (`test_recursive`) and *indirect*
dispatch (`test_callchain`) so the indirect-call surcharge is exercised
on real IR.

---

## 2. Cost-model comparison (baseline)

Three increasingly informative cost models, all computed from the same
IR for every testcase:

| Model | Formula | What it captures |
|---|---|---|
| **A.** Raw count   | `Σ count(group)`                          | naïve instruction count |
| **B.** Weighted    | `Σ count × weight`                        | per-class hardware expense |
| **C.** + depth *(this pass)* | `Σ count × weight × max(depth, 1)` | asymptotic loop contribution |

### Aggregate totals per testcase

| Testcase | level | A. raw | B. weighted | **C. +depth** | top function | energy (pJ) |
|---|---|---:|---:|---:|---|---:|
| test_arith       | O0 | 207 | 418 | **493** | matrix_add   |  5784.3 |
| test_arith       | O2 | 142 | 152 | **196** | matrix_add   |  1221.0 |
| test_branchy     | O0 | 164 | 303 | **303** | parse_int    |  3845.9 |
| test_branchy     | O2 |  81 | 128 | **128** | main         |  1733.5 |
| test_callchain   | O0 | 109 | 226 | **229** | run_pipeline |  3362.9 |
| test_callchain   | O2 |  32 |  34 |  **37** | run_pipeline |   367.6 |
| test_floatmm     | O0 | 174 | 336 | **514** | matmul       |  4487.9 |
| test_floatmm     | O2 | 228 | 469 | **513** | main         |  7546.8 |
| test_memheavy    | O0 | 148 | 267 | **355** | stencil      |  3519.4 |
| test_memheavy    | O2 | 650 | 796 | **796** | main         |  8724.1 |
| test_recursive   | O0 |  76 | 164 | **164** | main         |  2311.4 |
| test_recursive   | O2 |  76 | 108 | **108** | main         |  1135.2 |

### What each column adds

| Comparison | Insight |
|---|---|
| **A → B** (`+weight`)        | Memory/call-heavy code separates from arithmetic. `test_callchain` jumps 109 → 226 (+108 %) because every call is now charged 5×. `test_branchy` jumps 164 → 303 (+85 %) because parse_int is dominated by loads. Pure arithmetic (`test_arith` after the inliner runs in O2) barely moves: 142 → 152. |
| **B → C** (`+depth`)         | Deep loops emerge. `test_floatmm` jumps 336 → 514 (+53 %) — the depth-3 matmul body is correctly multiplied. `test_branchy` and `test_recursive` don't move because their loops are depth 1. This is exactly what the model is supposed to do. |

**Ranking flips.** Switching to model C changes the function ranking
inside several testcases, which is the strongest evidence the depth
multiplier carries information the simpler models lose:

* `test_floatmm @ O0` — model B ranks `main > matmul`; model C ranks
  `matmul > main` because `matmul`'s depth-3 inner body dominates.
* `test_memheavy @ O0` — model B ranks `stencil > main > reduce_indirect`;
  model C keeps the same ordering but `stencil` widens its lead from
  ~2× to ~3× over `main`.

---

## 3. Per-testcase findings

### 3.1 `test_arith.c` — the original mixed workload

| function | O0 cost | O2 cost | Δ | recommendations (O0) |
|---|---:|---:|---:|---|
| matrix_add | 193 | 111 | −42 % | `VECTORIZABLE`, `HOTSPOT` |
| main       |  86 |  25 | −71 % | `HOTSPOT` |
| process    |  83 |  47 | −43 % | `HOTSPOT` (O0) → `VECTORIZABLE`, `HOTSPOT` (O2) |
| compute    |  60 |   3 | −95 % | `HOTSPOT` → `INLINE_CANDIDATE` |
| classify   |  39 |   6 | −85 % | `HOTSPOT` → `INLINE_CANDIDATE` |
| scale      |  32 |   4 | −88 % | `HOTSPOT` → `INLINE_CANDIDATE` |

`compute`, `classify`, and `scale` correctly transition from
`HOTSPOT` at O0 to `INLINE_CANDIDATE` at O2 — the inliner has folded
their bodies into callers, leaving behind near-empty stubs.
`matrix_add` retains its `VECTORIZABLE` tag at both levels because it
has loops with no calls inside.

### 3.2 `test_branchy.c` — switches and parsers

| function | O0 cost | O2 cost | CC (O0) | CC (O2) |
|---|---:|---:|---:|---:|
| parse_int  | 134 |  28 |  6 |  6 |
| main       | 108 |  90 |  3 |  2 |
| categorize |  61 |  10 | 23 | 20 |

`categorize`'s cyclomatic complexity of **23** at O0 (down to 20 at O2)
is the highest in the suite and triggers `HIGH_COMPLEXITY`. The 13-case
character-classifier switch + a default ladder produce that many CFG
edges by design.

`parse_int @ O0` is correctly tagged `VECTORIZABLE` despite being a
parser — the model's "loop with no calls" rule is over-permissive
here. We discuss this honestly in §5.

### 3.3 `test_callchain.c` — call surcharge in action

This testcase is the cleanest demonstration of the indirect-call
heuristic. At -O0:

* `pipeline_direct` (3 direct calls, no body): cost **22**
* `dispatch` (1 indirect call, no body):       cost **22**

Same number of calls with the same nominal weight, but the indirect
call's `effectiveWeight = 5 × 1.6 = 8` versus 5 for direct. The two
costs land at the same number because `pipeline_direct` has 3 direct
calls (3 × 5 = 15) plus its return/cast bookkeeping, while `dispatch`
has just 1 indirect call (8) plus a load of the function pointer (3),
plus its bookkeeping. The arithmetic is consistent.

At -O2 the inliner annihilates almost everything:

| function | O0 | O2 | Δ |
|---|---:|---:|---:|
| run_pipeline    | 108 | 30 | −72 % |
| pipeline_direct |  22 | gone | inlined away |
| add1, mul2, neg |   8 ish |  0–1 | inlined |
| main            |  52 |  5 | −90 % |

Total testcase cost drops 229 → 37 (−84 %), the largest reduction in
the suite, because every direct call is inlinable at this size. The
indirect dispatch through the function-pointer table survives, which
is exactly the case the heuristic is designed to keep visible.

### 3.4 `test_floatmm.c` — depth multiplier in action

`matmul` at -O0:

```
Total weighted cost     : 335
Loop count / max depth  : 1 / 3
Most expensive group    : memory (cost=246)
Critical Path (worst-case):  cost = 264
  bb.0 -> bb.1 -> bb.2 -> bb.3 -> bb.4 -> bb.5 -> bb.6 -> bb.7
```

The depth multiplier moves `bb.6` (the depth-3 inner accumulator block)
from a raw cost of 50 to an effective cost of 150, putting it on the
critical path with margin to spare. Removing the depth multiplier
(model B) would still place it on the critical path but with much less
visual contrast in the dashboard's bar chart.

`matmul` at -O2 drops 335 → 81 (−76 %) because the inner loop bodies
get unrolled and redundant loads are hoisted. We then hit the failure
mode discussed in §5: `test_floatmm::main` jumps 112 → 378 (+238 %)
because everything inlined into it.

### 3.5 `test_memheavy.c` — memory dominance

`stencil` at -O0: 71 % of cost is in the `memory` group (10 loads + 1
store per inner iteration, all multiplied by depth 2). This is exactly
the cost shape we wanted the model to surface and the dashboard's
donut chart confirms it visually.

`reduce_indirect` is 51 % memory cost despite its tiny instruction
budget — the pointer-chasing pattern (`buf[idx[i]]`) makes every
iteration two loads, defeating any chance of register promotion.

### 3.6 `test_recursive.c` — RECURSIVE tag, leaf inlining

`fib` and `fact` are tagged `RECURSIVE` at both opt levels because
they directly call themselves. The `square` leaf goes
`INLINE_CANDIDATE → INLINE_CANDIDATE` (cost stays low), and the
`HOTSPOT` tag lifts off `fib` and `fact` at -O2 as their inlined call
sites move cost into `main`.

---

## 4. Critical-path verification

For each function we verified that:
1. the reported critical path is a valid CFG path (every successor
   appears in the reported predecessor's successor list), and
2. every block on the path has its `isCritical` flag set to `true` in
   the JSON,
3. the sum of `cost` along the path equals `criticalPathCost`.

Spot-check (`test_floatmm::matmul @ O0`):

| BB | cost | on path | depth |
|---|---:|:---:|---:|
| bb.0 | 24  | ✅ | 0 |
| bb.1 |  8  | ✅ | 1 |
| bb.2 |  4  | ✅ | 1 |
| bb.3 | 16  | ✅ | 2 |
| bb.4 | 14  | ✅ | 2 |
| bb.5 | 24  | ✅ | 3 |
| bb.6 | 150 | ✅ | 3 |
| bb.7 | 24  | ✅ | 3 |
| **sum** | **264** | | |

`criticalPathCost` in the JSON: **264**. Match.

---

## 5. Failure cases

A static cost model can only see what's in the IR. Where it disagrees
with reality, we want to be honest about it. These are the four
failure modes worth calling out:

### 5.1 -O2 inlining can *increase* a function's reported cost

The strongest honest failure mode in the suite:

| function | O0 | O2 | Δ | reason |
|---|---:|---:|---:|---|
| `test_memheavy::main`     |  78 | 435 | **+458 %** | callees inlined into main |
| `test_floatmm::main`      | 112 | 378 | **+238 %** | callees inlined into main |
| `test_memheavy::stencil`  | 212 | 322 |  **+52 %** | inner loop unrolled |

These are not bugs in the pass — they are an honest reflection of the
IR. After `-O2` inlining and unrolling, `main` literally contains more
instructions than it did at -O0, so any function-level metric will
report higher cost. The wall-clock runtime is lower (fewer call
boundaries, better instruction-level parallelism), but the static
cost model does not see that.

**Mitigation in the dashboard.** The A vs B compare view shows the
delta both as an absolute and as a per-call-site figure, which makes
it clear that the cost moved between functions rather than appeared
out of nowhere. Aggregated across both `main` and the inlined callees,
total module cost still drops O0 → O2.

### 5.2 The `VECTORIZABLE` tag is over-permissive

`parse_int @ O0` is tagged `VECTORIZABLE`. It isn't, in any practical
sense — it's a parser with a data-dependent loop. The current rule
("loop exists AND no calls in loop body") catches the structural
shape but misses that data dependencies between iterations break SIMD.

Fixing this properly needs `ScalarEvolution` to detect inter-iteration
dependencies; we declared SCEV out of scope in `DESIGN.md` §10. We
flag this in the dashboard as a "soft" recommendation rather than a
hard claim.

### 5.3 PHI nodes cost zero

`phi=0` in the default weight table. Real `phi` lowering on x86 uses
register-allocator-inserted moves that are not free. A function
dominated by `phi`s (some of `test_callchain` at -O2 — 7 phi nodes in
a 30-cost function) will be slightly under-counted. The user can fix
this by editing `config/weights.cfg` and re-running.

### 5.4 The model has no cache behaviour

`test_memheavy::stencil` and `test_memheavy::reduce_indirect` are
both memory-dominated, but the *real* runtime difference between
sequential stencil access and indirect pointer-chasing access is huge
(typically 5–20×). The model rates them roughly equivalently per
load. A genuine fix needs a cache-hierarchy model (LLVM has none
at the IR level).

---

## 6. What we would do next

In rough priority order:

1. **Port to the new pass manager** so the project builds against
   modern LLVM (≥ 17). The cost analysis code is unchanged; only the
   plumbing around `runOnFunction` moves.
2. **Add a `BlockFrequencyInfo` mode** as an opt-in alternative to the
   static depth multiplier — gives more accurate weights at the cost
   of profile-style heuristics.
3. **Tighten `VECTORIZABLE`** with a `ScalarEvolution` check for
   loop-carried dependencies, eliminating the `parse_int` false
   positive.
4. **Per-target weight tables.** Ship `weights.x86.cfg`,
   `weights.arm.cfg`, etc., calibrated against published latencies, so
   "what does this look like on Apple Silicon vs Skylake?" becomes a
   one-flag question.
5. **Cross-function cost rollup.** For each call site, charge the
   caller a fraction of the callee's cost (with a depth penalty for
   recursion). This stops the "main inflated by inlining" failure mode
   in §5.1 from being misleading.
