#!/usr/bin/env python3
"""
scripts/validate_recommendations.py
────────────────────────────────────────────────────────────────────────
Driven by scripts/validate_recommendations.sh (which exports CLANG, OPT,
PASS_LIB, ROOT). For each of Plumb's 5 recommendation tags, this applies
the real LLVM transformation the tag implies to the flagged function(s),
re-runs Plumb on the result, and reports whether the recommendation
actually paid off — including the cases where it honestly doesn't.

This is the FIXED case study, always run against Plumb's own 3
testcases (test_arith.c/test_branchy.c/test_recursive.c) — its output is
what's documented in EVALUATION.md §6 and reproduced verbatim every time
this script runs. For LIVE validation against whatever program you
actually load in the dashboard, see scripts/validation_server.py, which
shares the same transform logic (scripts/_plumb_lib.py) but runs it
on-demand over HTTP instead of against a fixed set.

No dependency beyond the stdlib (json, re, subprocess) — same convention
as benchmarks/analyze.py, which is the only other Python in this repo.
"""
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plumb_lib as lib

ROOT = os.environ["ROOT"]
OPT = os.environ["OPT"]
PASS_LIB = os.environ["PASS_LIB"]
WEIGHTS = os.path.join(ROOT, "config", "weights.cfg")
IR_DIR = os.path.join(ROOT, "ir")
RESULTS_DIR = os.path.join(ROOT, "results")
VAL_DIR = os.path.join(ROOT, "validation")
DASHBOARD_HTML = os.path.join(ROOT, "dashboard", "dashboard.html")
VALIDATION_BLOCK_RE = re.compile(
    r"/\* VALIDATION_DATA_BEGIN.*?VALIDATION_DATA_END \*/", re.S)

lib.configure(OPT, PASS_LIB, WEIGHTS)
read, write = lib.read, lib.write
opt_run, plumb_run = lib.opt_run, lib.plumb_run
strip_blocking_attrs, mark_alwaysinline = lib.strip_blocking_attrs, lib.mark_alwaysinline
function_body, call_count, self_call_count = lib.function_body, lib.call_count, lib.self_call_count
count_vector_instructions, func_json = lib.count_vector_instructions, lib.func_json


# ─────────────────────────────────────────────────────────────────────
def _inline_experiment(testcase, targets, report_functions):
    src = read(os.path.join(IR_DIR, f"{testcase}.O0.ll"))
    marked_path = _tmp(mark_alwaysinline(src, targets), f"{testcase}_marked.ll")

    # (a) raw -always-inline only — shows cost *moving*, not shrinking,
    # because nothing cleans up the newly-inlined, unpromoted allocas,
    # and the now-dead flagged functions are still counted (external
    # linkage survives plain -globaldce).
    opt_run(lib.ALWAYS_INLINE_ONLY, marked_path,
             os.path.join(VAL_DIR, f"{testcase}.inline_raw.ll"))
    # (b) the same inlining plus the cleanup -O2's pipeline always runs
    # alongside it — this is the version that should actually shrink cost.
    opt_run(
        lib.inline_cleanup_passes(lib.has_function(src, "main")),
        marked_path,
        os.path.join(VAL_DIR, f"{testcase}.inline_cleaned.ll"))

    before = json.load(open(os.path.join(RESULTS_DIR, f"{testcase}.O0.json")))
    after_raw = plumb_run(os.path.join(VAL_DIR, f"{testcase}.inline_raw.ll"),
                           os.path.join(VAL_DIR, f"{testcase}.inline_raw.json"),
                           "inline-raw")
    after_cleaned = plumb_run(os.path.join(VAL_DIR, f"{testcase}.inline_cleaned.ll"),
                               os.path.join(VAL_DIR, f"{testcase}.inline_cleaned.json"),
                               "inline-cleaned")

    remaining_calls = sum(call_count(read(os.path.join(VAL_DIR, f"{testcase}.inline_cleaned.ll")), t)
                           for t in targets)

    result = {
        "flagged": targets,
        "before_total": before["metadata"]["totals"]["totalCost"],
        "after_raw_total": after_raw["metadata"]["totals"]["totalCost"],
        "after_cleaned_total": after_cleaned["metadata"]["totals"]["totalCost"],
        "remaining_calls_to_flagged_fns": remaining_calls,
        "before_functions": {f["name"]: f["totalCost"] for f in before["functions"]},
        "after_raw_functions": {f["name"]: f["totalCost"] for f in after_raw["functions"]},
        "after_cleaned_functions": {f["name"]: f["totalCost"] for f in after_cleaned["functions"]},
    }
    print(f"  before total cost           : {result['before_total']}")
    print(f"  after -always-inline (raw)  : {result['after_raw_total']}  (moved, not shrunk)")
    print(f"  after inline + cleanup      : {result['after_cleaned_total']}")
    print(f"  remaining calls to {targets}: {remaining_calls}")
    print(f"  per-function (before -> raw -> cleaned):")
    for name in report_functions:
        b = result["before_functions"].get(name, "gone")
        r = result["after_raw_functions"].get(name, "gone")
        c = result["after_cleaned_functions"].get(name, "gone")
        print(f"    {name:12s} {b!s:>6} -> {r!s:>6} -> {c!s:>6}")
    return result


def validate_inline_candidate():
    print("\n=== 1a. INLINE_CANDIDATE — square in testcases/test_recursive.c "
          "(tagged INLINE_CANDIDATE at O0 today) ===")
    square = _inline_experiment("test_recursive", ["square"], ["square", "main"])

    print("\n=== 1b. INLINE_CANDIDATE — compute/classify/scale in testcases/test_arith.c "
          "(the functions EVALUATION.md §3.1 already calls out as the textbook "
          "HOTSPOT-at-O0 -> INLINE_CANDIDATE-at-O2 example) ===")
    arith = _inline_experiment("test_arith", ["compute", "classify", "scale"],
                                ["compute", "classify", "scale", "process", "matrix_add", "main"])
    return {"square": square, "arith_trio": arith}


def validate_vectorizable():
    print("\n=== 2. VECTORIZABLE — matrix_add (true positive) vs parse_int (false positive) ===")
    result = {}
    for testcase, fn in [("test_arith", "matrix_add"), ("test_branchy", "parse_int")]:
        src = read(os.path.join(IR_DIR, f"{testcase}.O0.ll"))
        cleaned = strip_blocking_attrs(src)
        vec = opt_run(lib.VECTORIZE_PASSES,
                      _tmp(cleaned, f"{testcase}_clean.ll"),
                      os.path.join(VAL_DIR, f"{testcase}.vectorized.ll"))
        body = function_body(vec, fn)
        vec_insts = count_vector_instructions(body)
        before = json.load(open(os.path.join(RESULTS_DIR, f"{testcase}.O0.json")))
        after = plumb_run(os.path.join(VAL_DIR, f"{testcase}.vectorized.ll"),
                           os.path.join(VAL_DIR, f"{testcase}.vectorized.json"),
                           "vectorized")
        b = func_json(before, fn)
        a = func_json(after, fn)
        result[fn] = {
            "vector_instructions": vec_insts,
            "before_cost": b["totalCost"] if b else None,
            "after_cost": a["totalCost"] if a else None,
        }
        verdict = "VECTORIZED" if vec_insts > 0 else "declined by real vectorizer"
        print(f"  {fn:12s} vector instructions = {vec_insts:3d}  ({verdict})  "
              f"cost {result[fn]['before_cost']} -> {result[fn]['after_cost']}")
    return result


def validate_recursive():
    print("\n=== 3. RECURSIVE — fib / fact in testcases/test_recursive.c ===")
    src = read(os.path.join(IR_DIR, "test_recursive.O0.ll"))
    cleaned = strip_blocking_attrs(src)
    tce = opt_run(lib.TAILCALLELIM_PASSES, _tmp(cleaned, "recursive_clean.ll"),
                  os.path.join(VAL_DIR, "test_recursive.tce.ll"))
    result = {}
    for fn in ["fib", "fact"]:
        before_calls = self_call_count(cleaned, fn)
        after_calls = self_call_count(tce, fn)
        result[fn] = {"before_calls": before_calls, "after_calls": after_calls}
        verdict = "eliminated" if after_calls == 0 else "recursion survives -tailcallelim"
        print(f"  {fn:6s} self-calls: {before_calls} -> {after_calls}  ({verdict})")
    return result


def validate_hotspot():
    print("\n=== 4. HOTSPOT — resolution rate across the 12 local O0/O2 runs ===")
    testcases = sorted({f.rsplit(".", 2)[0] for f in os.listdir(RESULTS_DIR) if f.endswith(".json")})
    resolved, persisted = [], []
    for tc in testcases:
        o0_path = os.path.join(RESULTS_DIR, f"{tc}.O0.json")
        o2_path = os.path.join(RESULTS_DIR, f"{tc}.O2.json")
        if not (os.path.exists(o0_path) and os.path.exists(o2_path)):
            continue
        o0 = json.load(open(o0_path))
        o2 = json.load(open(o2_path))
        o2_by_name = {f["name"]: f for f in o2["functions"]}
        for f in o0["functions"]:
            if "HOTSPOT" not in f.get("recommendations", []):
                continue
            key = f"{tc}::{f['name']}"
            o2f = o2_by_name.get(f["name"])
            if o2f is None:
                resolved.append((key, "inlined away entirely"))
            elif "HOTSPOT" not in o2f.get("recommendations", []):
                resolved.append((key, f"{f['totalCost']} -> {o2f['totalCost']}"))
            else:
                persisted.append((key, f"{f['totalCost']} -> {o2f['totalCost']}"))
    total = len(resolved) + len(persisted)
    print(f"  {len(resolved)}/{total} O0 hotspots resolved by -O2 alone; "
          f"{len(persisted)}/{total} persist")
    for k, v in resolved:
        print(f"    resolved  : {k:35s} {v}")
    for k, v in persisted:
        print(f"    persists  : {k:35s} {v}")
    return {"resolved": resolved, "persisted": persisted, "total": total}


def validate_high_complexity():
    print("\n=== 5. HIGH_COMPLEXITY — categorize vs categorize_v2 in test_branchy.c ===")
    report = json.load(open(os.path.join(RESULTS_DIR, "test_branchy.O0.json")))
    orig = func_json(report, "categorize")
    v2 = func_json(report, "categorize_v2")
    result = {
        "categorize_cc": orig["cyclomaticComplexity"],
        "categorize_cost": orig["totalCost"],
        "categorize_v2_cc": v2["cyclomaticComplexity"],
        "categorize_v2_cost": v2["totalCost"],
        "categorize_recs": orig["recommendations"],
        "categorize_v2_recs": v2["recommendations"],
    }
    print(f"  categorize    CC={result['categorize_cc']:3d}  cost={result['categorize_cost']:3d}  "
          f"recs={result['categorize_recs']}")
    print(f"  categorize_v2 CC={result['categorize_v2_cc']:3d}  cost={result['categorize_v2_cost']:3d}  "
          f"recs={result['categorize_v2_recs']}")
    return result


def _tmp(text, name):
    path = os.path.join(VAL_DIR, name)
    write(path, text)
    return path


def build_dashboard_payload(summary):
    """Trim the full summary down to exactly what dashboard.html's
    renderValidationSection() reads — keeps the embedded blob small and
    the dashboard's expected shape explicit in one place."""
    ic, vec, rec, hot, hc = (summary["inline_candidate"], summary["vectorizable"],
                              summary["recursive"], summary["hotspot"], summary["high_complexity"])
    keep = lambda d, keys: {k: d[k] for k in keys}
    return {
        "inline_candidate": {
            name: keep(ic[name], ("before_total", "after_raw_total", "after_cleaned_total"))
            for name in ("square", "arith_trio")
        },
        "vectorizable": {
            name: keep(vec[name], ("vector_instructions", "before_cost", "after_cost"))
            for name in ("matrix_add", "parse_int")
        },
        "recursive": {
            name: keep(rec[name], ("before_calls", "after_calls"))
            for name in ("fib", "fact")
        },
        "hotspot": {"resolved": hot["resolved"], "persisted": hot["persisted"], "total": hot["total"]},
        "high_complexity": keep(hc, ("categorize_cc", "categorize_cost",
                                      "categorize_v2_cc", "categorize_v2_cost")),
    }


def update_dashboard(summary):
    """Re-embed VALIDATION_DATA into dashboard.html so the dashboard's
    static fallback data (shown only when the live validation_server.py
    isn't running) never goes stale relative to what this script just
    measured — the same reproducibility bar the rest of this project
    holds docs to."""
    if not os.path.exists(DASHBOARD_HTML):
        return
    html = read(DASHBOARD_HTML)
    if not VALIDATION_BLOCK_RE.search(html):
        print("  ! dashboard.html has no VALIDATION_DATA_BEGIN/END markers — "
              "skipping dashboard update", file=sys.stderr)
        return
    block = (
        "/* VALIDATION_DATA_BEGIN — auto-generated by scripts/validate_recommendations.py\n"
        "   from validation/summary.json. DO NOT HAND-EDIT this block — re-run\n"
        "   ./scripts/validate_recommendations.sh to refresh it. */\n"
        f"const VALIDATION_DATA = {json.dumps(build_dashboard_payload(summary), indent=2)};\n"
        "/* VALIDATION_DATA_END */"
    )
    write(DASHBOARD_HTML, VALIDATION_BLOCK_RE.sub(lambda _m: block, html, count=1))
    print("  dashboard/dashboard.html VALIDATION_DATA refreshed")


def main():
    # Start clean every run — a stale JSON left over from a prior failed
    # run must never be silently re-read as if it were fresh output.
    if os.path.isdir(VAL_DIR):
        shutil.rmtree(VAL_DIR)
    os.makedirs(VAL_DIR, exist_ok=True)
    summary = {
        "inline_candidate": validate_inline_candidate(),
        "vectorizable": validate_vectorizable(),
        "recursive": validate_recursive(),
        "hotspot": validate_hotspot(),
        "high_complexity": validate_high_complexity(),
    }
    write(os.path.join(VAL_DIR, "summary.json"), json.dumps(summary, indent=2))
    print(f"\nFull machine-readable summary: validation/summary.json")
    update_dashboard(summary)


if __name__ == "__main__":
    main()
