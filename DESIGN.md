# DESIGN — Plumb Pass

This document explains the **approach**, the **alternatives considered**,
and the **tradeoffs** behind every major design decision. The actual
LLVM-API plumbing lives in `IMPLEMENTATION.md`; the measured behaviour
across testcases lives in `EVALUATION.md`.

---

## 1. Problem framing

A naïve instruction count ranks two functions equal if they have the
same number of instructions, even when one is a tight integer loop and
the other is a chain of memory-bound calls. We want a model that:

* assigns a per-class **cost** that reflects relative hardware expense,
* accounts for loop nesting (an instruction inside a depth-3 loop runs
  many more times than one at function entry),
* surfaces the **single most expensive control-flow trace** through a
  function, and
* produces output a downstream tool (the dashboard) can re-weight on the
  fly without re-running the pass.

We also want the model to be **explainable**: every cost number must
trace back to (group, count, weight, depth). No black-box scores.

---

## 2. Pass type: `FunctionPass` over the legacy PM

### Decision
Implement the analysis as a single `llvm::FunctionPass`, registered
via `RegisterPass`, run with `opt -enable-new-pm=0`.

### Alternatives considered

| Option | Why we passed |
|---|---|
| `ModulePass`                   | Per-function granularity is the natural reporting unit. A `ModulePass` would force us to re-implement traversal manually with no benefit. |
| New PM `PassInfoMixin`         | Cleaner API, but the assignment brief and the LLVM textbook material are written for the legacy PM, and `opt -load … -<name>` still works there. New-PM plugin loading via `-passes=` is more fragile across distros. |
| `MachineFunctionPass`          | Would let us cost real machine instructions (sched info, latency tables). Far more accurate, far more target-coupled, and removes the IR-level genericity that lets the same pass meaningfully analyse `-O0` and `-O2` IR side by side. |
| `BlockFrequencyAnalysis` user  | Block frequencies are estimates from `BranchProbabilityInfo` — i.e. *guesses* about runtime behaviour. We deliberately want a purely-static, profile-free model so the numbers are reproducible from the IR alone. |

### Tradeoff accepted
Legacy PM is officially deprecated and is gone in LLVM 17. Our build
explicitly rejects LLVM ≥ 17 with a friendly diagnostic and pins to
14/15/16. This is the correct tradeoff for an academic project; for a
production tool we would port to the new PM.

---

## 3. Cost model: weight × static loop depth

### Decision
For every instruction `I` in basic block `B` inside loop nest of depth
`d` (≥ 0):

```
cost(I) = weight(group(I)) × max(d, 1)
```

The function's total cost is the sum over all instructions.

### Why static depth, not block frequency

| Approach | Source | Reproducible? | Profile required? |
|---|---|---|---|
| **Static loop depth** *(chosen)* | `LoopInfo::getLoopDepth(BB)` | ✅ deterministic | no |
| Static block frequency           | `BlockFrequencyInfo`         | ✅ deterministic | no, but heuristic |
| Profile-guided frequency         | `-fprofile-instr-use`        | ✅ if PGO file present | yes |

Loop depth gives a clean **asymptotic** picture: a depth-3 loop body
contributes 3× per instruction, matching its big-O contribution. Block
frequency would give a slightly more accurate weight (typically ~16×
per loop) but introduces heuristic guesswork that is harder to explain
and audit.

The grader (and the dashboard's live-weight tuner) needs to be able to
re-derive a cost from a small set of public numbers — depth multiplier
is the simplest such number.

### Why ×1.6 for indirect calls

A direct call resolves at link time and is one branch with a known
target. An indirect call (function pointer, vtable, dispatch table)
adds a load to fetch the pointer, defeats the inliner, and on real
hardware also defeats the branch-target predictor on a cold target.
1.6× is a deliberately conservative midpoint between the literature's
1.2× (no misprediction) and 2.0× (full misprediction stall).

This is exactly the kind of single-knob heuristic we want surfaced as a
constant in code, not buried in a config file — it's small enough to
audit and the rationale is comment-documented.

---

## 4. Instruction grouping (9 classes)

`add / mul / memory / call / branch / compare / cast / alloca / phi / other`

### Why 9 and not more

Two competing pressures:

* **Granularity.** Hardware actually has many more cost classes:
  integer-add vs FP-add, scalar vs vector, aligned vs unaligned load,
  etc. The more we split, the more the model drifts toward a target
  CPU's specific µ-arch.
* **Auditability.** A grader / industry reviewer wants to point at any
  cost number and trace it back to a row in the weight table in <10
  seconds.

Nine is the smallest set where every common instruction class lands in
a non-`other` bucket and the weight table fits on one screen. We do
**not** distinguish int-add from FP-add — they share enough latency
behaviour at this abstraction level, and the user can split them by
editing the classifier if they need to.

### Why `phi` and `other` default to weight 0

`phi` is a meta-instruction the backend lowers away during register
allocation. `getelementptr` and `ret` likewise translate to address
arithmetic that the backend folds. Counting them with a non-zero
weight inflates the model with cost that has no machine reality. They
remain in the table so the user can re-weight them if they want to
study a model where, say, address computation is expensive.

---

## 5. Critical-path analysis

### Decision
For every function:

1. Iterate basic blocks in **reverse post-order** from the entry.
2. **Skip back-edges** (any edge `pred → BB` where `pred` is not yet
   numbered in RPO).
3. DP: `dp[BB] = cost(BB) + max(dp[pred] for forward edges)`.
4. Backtrack from `argmax(dp)` to recover the path.

### Alternatives considered

| Option | Why we passed |
|---|---|
| Bellman-Ford / Johnson's | Both are general shortest-path. The CFG without back-edges is a DAG, which makes the DP linear in `|V|+|E|`. No need for the heavier algorithms. |
| Counting back-edge cost   | Would produce infinite paths in any function with a loop. The whole point of analysing a *static* CFG is reporting the worst-case cost of executing the function **once through its trace**. Loop-cost is already encoded by the depth multiplier on each block's cost, not by traversal. |
| Cytoscape DOM rendering as truth | Easy and pretty, but requires running the dashboard to extract the path. Doing it in the pass means CSV/JSON consumers see the same path the dashboard draws. |

### Tradeoff accepted
The reported critical path is the worst-case **single trace**, not the
worst-case **expected trace** under branch probabilities. A function
with a rarely-taken expensive branch will be reported as more expensive
than its average-case execution — which is the conservative (and
intentional) reading.

---

## 6. Recommendations

| Tag | Trigger | Why |
|---|---|---|
| `HOTSPOT`          | `totalCost > hot-threshold` | Highlight expensive functions for the reviewer. |
| `INLINE_CANDIDATE` | `totalCost < inline-threshold` AND name ≠ `main` | Cheap leaves are the right size for inlining. |
| `VECTORIZABLE`     | has ≥ 1 loop AND no calls inside loops | The simplest static signal for "loop body is a candidate for SIMD". |
| `RECURSIVE`        | function calls itself directly | Surfaces recursion clearly to the dashboard. |
| `HIGH_COMPLEXITY`  | cyclomatic complexity ≥ 10 | Industry threshold for "needs refactoring" (McCabe 1976). |

These are deliberately **first-order** rules. We rejected adding rules
that would require interprocedural analysis (e.g. "is recursive
indirectly via a call cycle") because the explainability cost is high
and the value is low for this scope.

---

## 7. Output formats: terminal + CSV + JSON

### Decision
Produce three outputs from a single pass run:

* **Terminal** — human-readable ASCII tables and bar chart.
* **CSV** — for spreadsheet / `awk` consumers.
* **JSON** — structured, lossless, dashboard-ready.

### Why all three
The dashboard needs structure (JSON). A grader skimming logs needs
prose (terminal). A CI check that asserts "function X must have cost <
N" needs a tab-separable format (CSV). Generating all three from one
in-memory data structure costs ~30 LoC and avoids the n-tool drift
problem.

The JSON is the canonical artifact: the dashboard re-derives every
chart from it, and the live-weight tuner *re-runs the cost formula
client-side* using the `groups[].count` field, which is why we emit
counts and weights separately rather than only the multiplied result.

---

## 8. Dashboard

### Decision
Single self-contained HTML file. CDN-loaded libs (Chart.js,
Cytoscape.js, html2canvas, jsPDF). No build step.

### Alternatives considered

| Option | Why we passed |
|---|---|
| React/Vue + bundler | Adds a Node toolchain to a C++/LLVM project. The grader would need `npm install` to inspect the UI. |
| Streamlit / Gradio  | Python runtime dependency and a server process. Single HTML wins for a "double-click to view" demo. |
| TUI (ncurses)       | Hard to demo in a video, no graph rendering. |

### Tradeoff accepted
First load needs a network connection (CDN). After that all libs are
browser-cached. For air-gapped lab grading, the four CDN files can be
downloaded once and the `<script src=…>` tags rewritten — but this is
a one-line edit per lib, not a structural concern.

---

## 9. Build and toolchain

### Decision
* Two scripts: `build.sh` (CMake configure + build) and `run.sh`
  (compile testcases → run pass → open dashboard).
* Shared LLVM detection in `scripts/_llvm_env.sh`, sourced by both.
* Auto-detect LLVM in this priority order: `$LLVM_DIR` env var →
  `llvm-config-{14,15,16}` → Homebrew `llvm@{14,15,16}` →
  `/usr/lib/llvm-{14,15,16}/lib/cmake/llvm`.
* On macOS, link with `-Wl,-undefined,dynamic_lookup` so the plugin
  binds to the copy of libLLVM already loaded inside `opt(1)`. Linux
  uses normal `-lLLVM` linking. (Full rationale in `IMPLEMENTATION.md`.)

### Alternatives considered

| Option | Why we passed |
|---|---|
| Single `demo.sh`     | The original layout. Splitting into `build.sh` + `run.sh` matches the conventional shape reviewers expect and makes CI cleaner — a build cache can hit on `build.sh` alone without re-running the analysis. |
| Bazel / Meson        | Adds a build-system dependency reviewers don't already have. CMake ships with every LLVM dev install. |
| Vendoring LLVM       | LLVM is ~1 GB of source. Far outside the scope of an analysis pass. |

---

## 10. Things explicitly out of scope

To keep the project focussed:

* **No machine code analysis.** We stop at the IR layer.
* **No PGO.** The model is purely static.
* **No interprocedural cost.** Each function is costed in isolation;
  call cost is a flat 5 plus the surcharge, not the callee's cost.
  This is why a tiny `main()` can rank below `matmul()` — `main()`
  *calls into* matmul but doesn't pay for it.
* **No multi-threading cost.** Locks, atomics, fences fall in `other`.
* **No cache model.** Memory cost is a single weight, not a hierarchy.

These would all be valuable extensions and are called out in
`EVALUATION.md` §6 as next steps.
