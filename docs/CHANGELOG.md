# CHANGELOG — bug fixes and their validation

This is Plumb's audit trail: every real bug found in the pass, the
scripts, or the dashboard, each with a reproducible before/after — not
just asserted. `EVALUATION.md` documents measured *behavior*; this
document is where the *defects found along the way* live, so the
former stays focused on what the tool does rather than what was wrong
with it at some point in its history.

§1–2 were found by manual review; §3 is the result of a multi-agent
audit (6 independent reviewers, one per subsystem, every finding
adversarially re-verified — refuted unless independently confirmed
against the real code and data — before being trusted) run specifically
to answer "does this actually work, end to end." 20 findings survived
verification; all 20 are fixed below, several sharing one root cause.

---

## 1. Classifier silently dropped Div/Rem/bitwise/shift/`invoke`/atomics

**Problem.** `classifyInst()` in `src/Plumb.cpp` only handled a subset of
the opcodes the group tables in `EVALUATION.md`, `README.md`, and
`IMPLEMENTATION.md` §3 claim to classify. `UDiv/SDiv/FDiv/URem/SRem/FRem`
(documented as `mul`), `And/Or/Xor/Shl/LShr/AShr` (documented as `add`),
`InvokeInst` (documented as `call`), and `AtomicCmpXchg/AtomicRMW/Fence`
(documented as `memory`) all fell through to the catch-all `other` group —
weight 0 by default — so they were silently invisible to the cost model.

**Reproduction (before the fix).** A 5-line C function with one division,
one modulo, one bitwise-and, and one shift:

```c
int f(int a, int b) {
    int x = a / b; int y = a % b; int z = a & b; int w = a << 2;
    return x + y + z + w;
}
```

Before the fix, all four instructions landed in `other` at cost 0:

```
| add      |  3    |   1     | 3      |    5%      |
| memory   |  17   |   3     | 51     |    85%     |
| alloca   |  6    |   1     | 6      |    9%      |
| other    |  5    |   0     | 0      |    0%      |   <- div/mod/and/shl hiding here
```

**After the fix**, the same IR through the corrected classifier:

```
| add      |  5    |   1     | 5      |    7%      |   <- +2 (the `and`, `shl`)
| mul      |  2    |   2     | 4      |    6%      |   <- new group, the `sdiv`, `srem`
| memory   |  17   |   3     | 51     |    77%     |
| alloca   |  6    |   1     | 6      |    9%      |
| other    |  1    |   0     | 0      |    0%      |   <- just the `ret`
```

**Regression validation.** Rebuilt the pass and re-ran it against every
existing artifact that depends on `classifyInst()`, to confirm the fix
doesn't silently break anything it was already getting right:

1. **6 local testcases × 2 opt levels (12 runs).** Re-ran `./run.sh`.
   4 of 12 runs were byte-for-byte identical to the pre-fix JSON
   (`test_branchy` O0/O2, `test_floatmm` O0, `test_recursive` O0) — proof
   the fix is a no-op wherever the mis-classified opcodes don't occur.
   The other 8 runs shifted by small, fully-explained amounts (traced to
   a `srem` in `test_callchain`/`test_memheavy`, and optimizer-introduced
   shifts/bitwise ops at `-O2`). All 12 JSON outputs still parse and the
   critical-path invariant (`Σ cost along path == criticalPathCost`) still
   holds (re-verified per `EVALUATION.md` §4's method).
2. **83-program LLVM test-suite sweep (166 runs, 1,165 functions).**
   Re-cloned via `benchmarks/fetch.sh`, re-ran `benchmarks/run_bench.sh`
   (83/85 programs still compile — same 2 pre-existing skips as before the
   fix), and regenerated `benchmarks/SUMMARY.md`. Median -O2 reduction
   moved 56.0% → 54.7% (this bug was under-counting cost, so real cost is
   slightly higher than previously reported); the flagship example
   (`matmul`: 335 → 81, -76%) is untouched since it contains none of the
   previously-mis-bucketed opcodes.
3. Propagated every changed number through `EVALUATION.md`, `README.md`,
   and `benchmarks/SUMMARY.md` so the shipped docs match what `./run.sh`
   and the benchmark scripts actually produce today — a doc that
   disagrees with a one-command reproduction is itself a defect.

## 2. Dashboard's live weight tuner didn't refresh recommendation badges

**Problem.** `recompute()` in `dashboard/dashboard.html` re-derived cost,
energy, and percentages when a weight slider moved, but never rebuilt
`f.recommendations[]` — so `HOTSPOT` / `INLINE_CANDIDATE` badges stayed
pinned to whatever the original `-O0`/`-O2` run computed, even though the
feature's entire pitch (per `README.md`) is exploring "what if `memory`
was free?".

**Validation.** Traced every call site that reads `recommendations[]`
(`refreshHeatmap()`, the compare-mode diff card, the recommendations
table) — all read the array directly, none re-derive it, confirming the
staleness was total, not partial. `f.isHotspot` was being recomputed
correctly but was dead code (`grep isHotspot dashboard.html` shows it's
only ever assigned, never read) — a second symptom of the same gap.
Fixed `recompute()` to rebuild `INLINE_CANDIDATE`/`HOTSPOT` from the
re-weighted total against `raw.metadata.{inlineThreshold,hotThreshold}`,
while preserving the structural tags (`VECTORIZABLE`, `RECURSIVE`,
`HIGH_COMPLEXITY`) that don't depend on weight, in the pass's own
ordering. Syntax-validated the whole inline script with `node --check`
after the change.

## 3. Multi-agent audit — 20 findings, all fixed

Two of these are core-pass correctness bugs at the same level as §1;
the rest are script/dashboard/doc defects. All are validated by
re-running the affected artifact, not just re-reading the diff.

### 3.1 `classifyInst()` silently dropped `fneg` (unary float negation) into `other`

**Problem.** Same bug *class* as §1, missed by that fix: `fneg`
(the IR form of source-level `-x` on `float`/`double`) is an
`llvm::UnaryOperator`, a **sibling** of `BinaryOperator`, not a
subclass — so `dyn_cast<BinaryOperator>(&I)` returns null for it and
it fell through to the zero-weight `other` bucket instead of `add`
(where `FAdd`/`FSub` already live, and where `fneg` — the modern
lowering that replaced `fsub -0.0, x` — belongs).

**Reproduction.** `double negate(double x) { return -x; }` — before the
fix, its `fneg` instruction cost 0 and vanished into `other`; after,
`opt`'s own report shows it correctly charged to `add` (weight 1).

**Regression validation.**
1. **Local suite:** none of the 6 testcases contain `fneg` — confirmed
   by `grep -c fneg ir/*.ll` — so all 12 local runs are byte-for-byte
   identical before/after this fix.
2. **83-program benchmark sweep:** 20 of 83 programs contain 135
   `fneg` instructions total. Re-ran the full sweep; costs shifted by
   small, fully-attributable amounts (e.g. `cftmdl`: 3177 → 3181,
   `kernel_deriche`: 1579 → 1587) — median -O2 reduction unchanged at
   54.7% (a shift this small doesn't move a median over 83 programs),
   and the top-3 hottest-program ranking is unchanged. Numbers
   propagated into `README.md`'s "Top-3 hottest" box.

### 3.2 Critical-path search picked the wrong "worst-case" block on cost ties

**Problem.** `computeCriticalPath()`'s DP used `0` as both "no
predecessor found yet" *and* a legitimate dp value, so a real
predecessor with `dp == 0` was indistinguishable from "none" —
collapsing the reconstructed path to a single block whenever every
block's cost was 0 (e.g. a custom all-zero weight file, a documented,
supported "what if X was free" workflow per `EVALUATION.md` §5.3 and
`DESIGN.md` §3). A second, related bug in the same function: the final
"pick the highest-dp block" loop broke ties by `Function`'s raw
iteration order (effectively arbitrary), not by which path is actually
longer/deeper.

**Reproduction.** `matmul` (`test_floatmm.c`) has a real 8-block
critical path (`bb.0` → … → `bb.7`, documented in `EVALUATION.md` §4).
Re-running Plumb with every weight set to 0:

| | critical path length |
|---|---:|
| before fix | **1** block (`['bb.0']`) |
| after fix  | **8** blocks (`['bb.0', ..., 'bb.7']`), matching the real path |

**Fix.** Sentinel changed from `0` to `-1` for "no predecessor yet"
(cost can never be negative — `loadWeights()` now also clamps negative
weight-file entries to 0, closing the one way a real dp value could
have gone negative and broken this invariant). Path length is now
tracked alongside dp and used as an explicit tie-breaker when picking
the end block, so a tie is resolved toward the longer, more informative
trace.

**Regression validation.** Re-ran all 12 local + 166 benchmark runs at
default (non-tied) weights: every `criticalPath`/`criticalPathCost`
pair is byte-for-byte identical to before this fix (`matmul`'s path and
cost of 264 in `EVALUATION.md` §4 unchanged) — confirms the fix only
changes behavior in the tied-cost edge case, never in normal operation.
The `Σ cost along path == criticalPathCost` invariant
(`EVALUATION.md` §4's method) re-verified across all 178 runs.

### 3.3 This session's own regression: the `categorize_v2` self-check inflated `main`'s cost

**Problem.** The behavioral-equivalence check added for `EVALUATION.md`
§6.5 (`categorize` vs `categorize_v2`) was originally written as a loop
directly inside `test_branchy.c`'s `main()`. That's the same function
whose cost numbers are quoted throughout `EVALUATION.md` — the
256-iteration verification loop nearly doubled `main`'s reported cost
(108 → 163 at O0), silently invalidating the "unaffected" claim made
when that document's other numbers were regression-checked earlier the
same session.

**Fix.** Extracted the check into its own `static void
check_categorize_equivalence(void)`, called once from `main()`. This
is a hand-written harness bug, not a Plumb bug, but it's fixed the same
way a Plumb bug would be: root-caused and validated, not patched over
with updated numbers alone.

**Validation.** `main` now costs 113 at O0 (108 + 5, exactly one
`call` instruction — fully explained) instead of 163. Every number this
regression touched (`EVALUATION.md` §2's aggregate row, §3.2's
per-function table, `README.md`'s testcase table, §6's
`validate_recommendations.py` output) was re-derived from the corrected
`results/test_branchy.*.json` and updated to match, not left stale.

### 3.4 `scripts/validate_recommendations.py` — 4 bugs

| Problem | Fix | Validation |
|---|---|---|
| `call_count()` counted calls to `fib`/`fact` **module-wide**, including `main`'s one non-recursive call — inflating "self-calls" for `EVALUATION.md` §6.3 (3/2 instead of the true 2/1) | Added `self_call_count()`, scoped to the function's own body via `function_body()` | Re-ran §6.3: `fib` 2→2, `fact` 1→1 (was 3→3, 2→2) — corrected in `EVALUATION.md` |
| `VECTOR_TYPE_RE` counted **regex matches**, not instructions — a single `load <4 x i32>, <4 x i32>*` mentions the vector type twice, double-counting | Count matching **lines** (one IR instruction per line in `.ll` text), not raw regex occurrences | `matrix_add`: 20 → 14, matching an independent manual `grep -c` count taken earlier in the same session |
| `opt_run()` printed a failure diagnostic but then unconditionally read the (possibly nonexistent) output file, crashing with an opaque `FileNotFoundError` | Raise a clear `RuntimeError` with the failing command and stderr when `opt` fails or produces no output | Reproduced live (`opt -this-pass-does-not-exist`) — now fails with an actionable message instead of a bare traceback |
| `plumb_run()` didn't check `opt`'s exit status, and `validation/` was never cleared between runs — a failed run could silently return a **stale JSON from a prior successful run** | Same `RuntimeError` guard; `main()` now `shutil.rmtree(validation/)` before every run | Full script re-run confirmed clean; a failed run can no longer masquerade as a successful one |

Also hardened (not a bug in current use, but a silent-wrong-result risk
for future extension): `mark_alwaysinline()` assumed every target
function shares the first target's attribute group — now raises
immediately if that assumption doesn't hold, instead of silently
applying the wrong attributes.

### 3.5 Build/run script robustness — 5 fixes

| Script | Problem | Fix |
|---|---|---|
| `run.sh` | `$CLANG` invoked unquoted — breaks on a toolchain path containing a space | Quoted, matching `benchmarks/run_bench.sh`'s existing convention |
| `run.sh` | Per-`(testcase, opt-level)` pass failures were swallowed by `\|\| true` with no tracking — script printed "Done" and exited 0 even with a partial `results/` | Track failures, print a summary, exit nonzero if any pair failed |
| `scripts/validate_recommendations.sh` | Didn't check `./run.sh`'s exit status — a failed refresh silently fell through to validating stale artifacts | `\|\| die "..."` on the `run.sh` call |
| `benchmarks/run_bench.sh` | Always exited 0 regardless of failure count — a real regression (e.g. 60/83 failing) would look identical to success to the documented `run_bench.sh && analyze.py` one-liner | Exit nonzero if failures exceed the documented baseline of 2 known skips (`PLUMB_MAX_BENCH_FAILURES`, overridable) |
| `scripts/_llvm_env.sh` | `pick_tool()`'s fallback searched newest-version-first regardless of which LLVM major version was actually resolved — could pair `opt` from one LLVM version with `clang` from another on a multi-version Linux system | Try the resolved major version's suffix (e.g. `-14`) first |

Validated: `bash -n` on every changed script, plus a live test of each
new failure path (a deliberately-broken `opt` invocation, and
`PLUMB_MAX_BENCH_FAILURES=1` against the real 2-failure baseline —
confirmed it now exits 1 when it should and 0 in the normal case).

### 3.6 Dashboard — 2 fixes

**Tie-break mismatch.** `recompute()`'s `mostExpensiveGroup`
re-ranking iterated `f.groups` in its fixed JSON emission order
(`add, mul, memory, ...`), while the pass computes the original value
from a `std::map` (alphabetical order). On an exact cost tie the two
disagreed — reproduced with `test_arith.O2.json`'s `scale` (`mul` and
`cast` tied at cost 2): the pass's own JSON says `cast`, the dashboard
said `mul`. Fixed by sorting `f.groups` alphabetically before the
tie-break loop, matching the pass's iteration order exactly.

**Diff-mode recommendations never refreshed.** `refreshAll()` — called
by the weight sliders, `resetWeights()`, and a new file load — always
called `refreshRecs()`, which explicitly no-ops in diff mode (`renderDiffMode()`
owns that panel instead). So touching a slider while in diff mode left
the recommendations panel showing stale pre-change diff cards, even
though every other panel updated correctly. Fixed by making
`refreshAll()` dispatch to `renderDiffMode()` when `STATE.mode ===
'diff'`, so every caller is diff-mode-correct automatically instead of
needing three separate patches.

Validated: `node --check` on the extracted inline script after each
change (both pass).

### 3.7 Stale documentation — 3 fixes

- `benchmarks/README.md` still quoted the **pre-§1-fix** median
  (56%); `benchmarks/SUMMARY.md` and `README.md` already had the
  correct post-fix number (54.7%/55%). Updated to match.
- `EVALUATION.md` §5.2 cited "`DESIGN.md` §10" for why `ScalarEvolution`
  is out of scope — §10 never actually mentioned SCEV. Added the
  missing bullet to `DESIGN.md` §10 instead of removing the citation.
- `IMPLEMENTATION.md`'s recursion-detection snippet still showed
  `dyn_cast<CallInst>`; the real code (post-§1) uses `dyn_cast<CallBase>`
  so that recursion via `invoke` is detected too. Snippet updated to match.
