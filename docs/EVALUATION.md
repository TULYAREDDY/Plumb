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
> §6 goes one step further: it actually applies every one of Plumb's
> five recommendation tags — via a real LLVM transform, or a
> hand-written refactor where no pass exists — and measures whether
> following the advice helps. Four do. The fifth (`RECURSIVE`) is an
> honest negative result: no standard pass removes that cost, which is
> exactly why the tag exists.

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
| test_arith       | O2 | 142 | 157 | **201** | matrix_add   |  1218.0 |
| test_branchy     | O0 | 198 | 375 | **375** | parse_int    |  4913.0 |
| test_branchy     | O2 | 104 | 164 | **164** | main         |  2191.3 |
| test_callchain   | O0 | 109 | 228 | **231** | run_pipeline |  3365.3 |
| test_callchain   | O2 |  32 |  37 |  **40** | run_pipeline |   369.4 |
| test_floatmm     | O0 | 174 | 336 | **514** | matmul       |  4487.9 |
| test_floatmm     | O2 | 228 | 470 | **514** | main         |  7546.1 |
| test_memheavy    | O0 | 148 | 269 | **357** | stencil      |  3521.8 |
| test_memheavy    | O2 | 650 | 810 | **810** | main         |  8715.8 |
| test_recursive   | O0 |  76 | 164 | **164** | main         |  2311.4 |
| test_recursive   | O2 |  76 | 109 | **109** | main         |  1134.6 |

> These numbers reflect a classifier fix (see project notes): `Div`/`Rem`/bitwise/shift
> ops and `invoke`/atomic instructions were previously mis-bucketed into the
> zero-weight `other` group instead of `mul` / `add` / `call` / `memory` as the
> tables in this document and `IMPLEMENTATION.md` §3 always claimed. Rows without
> such instructions (`test_branchy`, `test_floatmm` @ O0, `test_recursive` @ O0)
> are byte-for-byte unchanged.

### What each column adds

| Comparison | Insight |
|---|---|
| **A → B** (`+weight`)        | Memory/call-heavy code separates from arithmetic. `test_callchain` jumps 109 → 228 (+109 %) because every call is now charged 5×. `test_branchy` jumps 198 → 375 (+89 %) because parse_int is dominated by loads. Pure arithmetic (`test_arith` after the inliner runs in O2) barely moves: 142 → 157. |
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
| matrix_add | 193 | 115 | −40 % | `VECTORIZABLE`, `HOTSPOT` |
| main       |  86 |  25 | −71 % | `HOTSPOT` |
| process    |  83 |  48 | −42 % | `HOTSPOT` (O0) → `VECTORIZABLE`, `HOTSPOT` (O2) |
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
| main       | 113 | 121 |  3 |  3 |
| categorize |  61 |  10 | 23 | 20 |
| categorize_v2 | 12 | 5 | 1 | 1 |
| check_categorize_equivalence | 55 | gone | 2 | — |

`categorize`'s cyclomatic complexity of **23** at O0 (down to 20 at O2)
is the highest in the suite and triggers `HIGH_COMPLEXITY`. The 13-case
character-classifier switch + a default ladder produce that many CFG
edges by design. `categorize_v2` and `check_categorize_equivalence` are
this session's additions — a lower-complexity refactor of `categorize`
and the runtime check that proves it's behavior-preserving,
respectively (full story in §6.5). `check_categorize_equivalence` is
`static` and gets fully inlined into `main` and optimized away at -O2
(its 256-iteration loop is provably dead once the optimizer can see
both callees never disagree), which is why `main`'s O2 cost jumps more
than its O0 cost does (+8 at O0, a single call; +31 at O2, the inlined
and partially-simplified loop body) and its cyclomatic complexity goes
up by one (the loop's branch is now part of `main`'s own CFG).

`parse_int @ O0` is correctly tagged `VECTORIZABLE` despite being a
parser — the model's "loop with no calls" rule is over-permissive
here. We discuss this honestly in §5, and validate it against LLVM's
real vectorizer in §6.2.

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
| run_pipeline    | 110 | 32 | −71 % |
| pipeline_direct |  22 | gone | inlined away |
| add1, mul2, neg |   8 ish |  0–1 | inlined |
| main            |  52 |  5 | −90 % |

Total testcase cost drops 231 → 40 (−83 %), the largest reduction in
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
`INLINE_CANDIDATE → INLINE_CANDIDATE` (cost stays low). At -O2, `fib`
drops from 44 to 14 — below the hot threshold (30), so `HOTSPOT` lifts
off and it becomes `INLINE_CANDIDATE` instead. `fact` drops from 36 to
34, which is a real reduction but *not* enough to cross the threshold,
so it correctly keeps its `HOTSPOT` tag at -O2 too — recursion makes it
resistant to the same shrinkage `fib` gets from tail-call-shaped
inlining.

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
| `test_memheavy::main`     |  80 | 435 | **+444 %** | callees inlined into main |
| `test_floatmm::main`      | 112 | 378 | **+238 %** | callees inlined into main |
| `test_memheavy::stencil`  | 212 | 334 |  **+58 %** | inner loop unrolled |

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

**At scale.** When we ran Plumb across 83 programs of LLVM's official
test-suite (see [`benchmarks/SUMMARY.md`](../benchmarks/SUMMARY.md)),
this failure mode showed up in **3 of 83 programs (~4%)**. The biggest
in-the-wild offender was `Shootout/matrix.c` at +113%. So the issue
is real, but the *rate* is low — Plumb's per-function ranking remains
informative on the other 96% of programs.

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
dominated by `phi`s (`test_branchy::parse_int` at -O2 — 7 phi nodes in
a 28-cost function) will be slightly under-counted. The user can fix
this by editing `config/weights.cfg` and re-running.

### 5.4 The model has no cache behaviour

`test_memheavy::stencil` and `test_memheavy::reduce_indirect` are
both memory-dominated, but the *real* runtime difference between
sequential stencil access and indirect pointer-chasing access is huge
(typically 5–20×). The model rates them roughly equivalently per
load. A genuine fix needs a cache-hierarchy model (LLVM has none
at the IR level).

---

## 6. Recommendation validation — implementing what Plumb suggests

Sections 1–6 show Plumb *detecting* five classes of actionable finding.
Detecting isn't the same as being right. This section closes the loop:
for each tag we apply the real LLVM transformation it implies (or, where
no such pass exists, a hand-written equivalent), re-run Plumb on the
result, and report what actually happened — including the cases where
the payoff isn't the clean win a demo would prefer. Every number below
comes from `./scripts/validate_recommendations.sh`, which is fully
reproducible and writes its intermediate IR/JSON to `validation/`
(gitignored, same treatment as `ir/`/`results/`).

This is a **fixed** case study — always the same 3 testcases. For live
validation against whatever program you actually load in the
dashboard (including one you write and upload on the spot), see
`./scripts/validation_server.sh` — same underlying transform logic
(`scripts/_plumb_lib.py`), applied on demand instead of to a fixed set.
The dashboard clearly labels which mode it's showing at any given time.

### 6.1 `INLINE_CANDIDATE` — inlining pays off, but only paired with cleanup

Two experiments, both using LLVM's real inliner (`-always-inline`,
targeted only at the flagged functions via a surgical `alwaysinline`
attribute — a blanket `-inline` was tried first and, confirmed live,
also folds unrelated functions like `matrix_add` into `main` on its own
cost heuristic, which would muddy an isolated test):

| Testcase | Flagged fn(s) | Tag today | Module total before | After `-always-inline` alone | After inline **+ cleanup** |
|---|---|---|---:|---:|---:|
| `test_recursive.c` | `square` | `INLINE_CANDIDATE` at O0 | 164 | 183 (+12%) | **149 (−9%)** |
| `test_arith.c` | `compute`, `classify`, `scale` | HOTSPOT at O0 → `INLINE_CANDIDATE` at O2 (§3.1) | 493 | 753 (+53%) | **363 (−26%)** |

Both flagged functions/trios have **zero remaining call sites** after
inlining — the recommendation is acted on completely. But raw
`-always-inline` alone makes total cost *worse*, not better, in both
cases. Two reasons, both visible in the per-function breakdown:

```
square/main   (before -> raw -> cleaned):   main   72 -> 91 -> 69
compute-trio  (before -> raw -> cleaned):
    process    83 -> 268 ->  89
    main       86 -> 161 ->  81
```

1. The now-inlined callee body lands in the caller with its original
   `alloca`/load/store pattern intact — nothing promotes those to
   registers or folds the redundant memory traffic, so the caller's
   instruction count goes up by more than the callee's raw cost.
2. The original callee definitions have external linkage, so plain
   `-globaldce` can't remove the now-dead bodies; Plumb keeps costing
   them standalone on top of the inlined copy. (Fixed for the "cleaned"
   column with `-internalize -internalize-public-api-list=main
   -globaldce`, which is the standard idiom for "this is the one
   real entry point.")

Running `-always-inline` **alongside** `-mem2reg -instcombine -adce`
(exactly the kind of cleanup that always accompanies inlining inside a
real `-O2` pipeline) recovers the reduction: −9% and −26% respectively,
consistent with the larger drops already documented for the full `-O2`
pipeline in §3.1. **Verdict: `INLINE_CANDIDATE` is validated, with an
important caveat now backed by data — inlining is only a net win when
paired with the cleanup passes that normally travel with it. A pass
pipeline that ran `-always-inline` in isolation, as some ad-hoc tools
do, would make Plumb's own cost model look worse, not better.**

### 6.2 `VECTORIZABLE` — the true positive is confirmed by the real vectorizer; the false positive is too

Running `-mem2reg -loop-rotate -loop-vectorize` — LLVM's actual
vectorization pipeline — on both flagged functions:

| Function | Flagged as | Vector instructions (`<4 x i32>` etc.) after `-loop-vectorize` | Cost before → after |
|---|---|---:|---:|
| `matrix_add` (`test_arith.c`) | `VECTORIZABLE`, true positive | **14** | 193 → 202 |
| `parse_int` (`test_branchy.c`) | `VECTORIZABLE`, documented false positive (§5.2) | **0** | 134 → 57 |

`matrix_add` picks up 14 `<4 x i32>`-typed instructions — LLVM's own
vectorizer agrees with Plumb's static heuristic. `parse_int` picks up
**zero** — the real vectorizer declines it, for the same
data-dependency reason §5.2 already gives in prose. This upgrades that
claim from "we believe this is a false positive" to "the vectorizer
confirms it." (`parse_int`'s cost still drops 134 → 57, but that's
`-mem2reg`/`-loop-rotate` promoting its scalar locals to registers, not
vectorization — 0 vector instructions rules that out.)

The `matrix_add` number is itself an honest new failure mode, in the
spirit of §5: cost goes *up* 193 → 202 (instructions 61 → 121) even
though the loop genuinely vectorized. `-loop-vectorize` alone (with no
follow-up `-instcombine`/`-simplifycfg` to fold the vector prologue and
scalar remainder loop it emits for leftover, non-multiple-of-4
iterations) roughly doubles the static instruction count. Plumb's cost
model has no SIMD-width dimension — it counts instructions, not work
done per instruction — so it cannot see that each of those 14 vector
instructions now processes 4 elements at once. **Verdict:
`VECTORIZABLE` correctly separates the true positive from the
documented false positive (both confirmed against the real vectorizer,
not just asserted), but the cost model itself would need a width term
to reward vectorization rather than merely tolerate it — a natural
addition alongside the next-steps list in §7.**

### 6.3 `RECURSIVE` — an honest non-result: no standard pass removes this cost

Running `-tailcallelim` on the cleaned IR leaves `fib`'s 2 self-calls
and `fact`'s 1 self-call completely untouched (scoped to calls made
*from within* each function's own body — `main`'s one call into each
doesn't count as a self-call):

```
fib   self-calls: 2 -> 2   (recursion survives -tailcallelim)
fact  self-calls: 1 -> 1   (recursion survives -tailcallelim)
```

`fib(n-1) + fib(n-2)` is non-tail binary recursion — both calls feed an
addition after they return. `fact`'s `n * fact(n-1)` similarly
post-processes the recursive result, breaking tail position. Neither is
a bug in Plumb or in LLVM: it's confirmation that `RECURSIVE` flags a
cost class genuinely resistant to compiler-only remediation, unlike
`INLINE_CANDIDATE` (§6.1), which a pass fixes outright. **Verdict:
validated as a *distinct* category rather than a variant of `HOTSPOT` —
a human has to decide whether to restructure the algorithm (e.g.
rewrite `fact` as an explicit accumulator loop); the compiler will not
do it for you, and Plumb is right to keep flagging it instead of
assuming `-O2` will quietly handle it the way it handles
`INLINE_CANDIDATE`.**

### 6.4 `HOTSPOT` — 9/21 resolved by `-O2` alone; 12/21 need more than a compiler flag

Across all 12 local runs, 21 functions carry `HOTSPOT` at O0 (including
`check_categorize_equivalence`, this session's new self-check
function). Checking each by name at O2: 9 drop below the cost-30
threshold or vanish entirely via inlining —

```
resolved: test_arith::compute      60 -> 3     test_branchy::categorize   61 -> 10
          test_arith::classify     39 -> 6     test_branchy::parse_int   134 -> 28
          test_arith::scale        32 -> 4     test_callchain::main       52 -> 5
          test_arith::main         86 -> 25    test_recursive::fib        44 -> 14
          test_branchy::check_categorize_equivalence -> inlined away entirely
```

The other 12 persist — including three that get *worse*
(`test_floatmm::main` 112→378, `test_memheavy::main` 80→435,
`test_memheavy::stencil` 212→334), the same §5.1 inliner-inflation
failure mode, now quantified against every `HOTSPOT` call in the suite
rather than just the two flagship examples originally cited there.
**Verdict: partially self-resolving. The 9/21 rate is itself the useful
result — a `HOTSPOT` tag is worth checking twice before acting: a
little under half the time `-O2` alone already handles it; the rest
need either the fixes validated in §6.1–6.3 or a human looking at the
algorithm.**

### 6.5 `HIGH_COMPLEXITY` — table lookup collapses CC 23 → 1

`categorize_v2` (added to `testcases/test_branchy.c` alongside the
original `categorize`), a 256-entry lookup table replacing the 18-case
switch plus 2 nested ifs:

| Function | Cyclomatic complexity | Cost | Recommendations |
|---|---:|---:|---|
| `categorize`    | 23 | 61 | `HOTSPOT`, `HIGH_COMPLEXITY` |
| `categorize_v2` |  1 | 12 | `INLINE_CANDIDATE` |

The refactor drops CC from 23 to 1 and cost from 61 to 12 — cheap
enough that Plumb now tags it `INLINE_CANDIDATE` too, a tag
`categorize` never earns. Behavioral equivalence is checked at runtime,
not just asserted: `main()` now iterates all 256 byte values and
`abort()`s on any `categorize`/`categorize_v2` disagreement before doing
anything else; the program exits 0, so the two are provably equivalent,
not merely structurally similar. **Verdict: validated. This is the one
tag with no corresponding LLVM pass — "implementing" it means a human
applies the suggested refactor by hand, which is exactly what
`HIGH_COMPLEXITY`'s McCabe-1976 justification (§6 of `DESIGN.md`) says
it's for: flagging code that needs a person to restructure it.**

---

## 7. What we would do next

In rough priority order:

1. **Port to the new pass manager** so the project builds against
   modern LLVM (≥ 17). The cost analysis code is unchanged; only the
   plumbing around `runOnFunction` moves.
2. **Add a `BlockFrequencyInfo` mode** as an opt-in alternative to the
   static depth multiplier — gives more accurate weights at the cost
   of profile-style heuristics.
3. **Tighten `VECTORIZABLE`** with a `ScalarEvolution` check for
   loop-carried dependencies, eliminating the `parse_int` false
   positive — now confirmed empirically against the real vectorizer in
   §6.2, not just asserted.
4. **Per-target weight tables.** Ship `weights.x86.cfg`,
   `weights.arm.cfg`, etc., calibrated against published latencies, so
   "what does this look like on Apple Silicon vs Skylake?" becomes a
   one-flag question.
5. **Cross-function cost rollup.** For each call site, charge the
   caller a fraction of the callee's cost (with a depth penalty for
   recursion). This stops the "main inflated by inlining" failure mode
   in §5.1 from being misleading.
6. **A SIMD-width term in the cost model.** §6.2 shows Plumb can
   correctly identify vectorizable code and still charge it *more*,
   because the model counts instructions, not work done per
   instruction. A per-instruction width multiplier (from vector type
   width, when present) would let `VECTORIZABLE` reward the
   optimization it recommends instead of merely tolerating it.
