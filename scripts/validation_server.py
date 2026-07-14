#!/usr/bin/env python3
"""
scripts/validation_server.py
────────────────────────────────────────────────────────────────────────
Local dev server for LIVE recommendation validation. The dashboard's
"Recommendation Validation" section calls this, over HTTP, to actually
run LLVM transforms against whatever program is currently loaded — not
just Plumb's own 3 testcases. Deliberately separate from
validate_recommendations.py, which is the fixed, always-reproducible
case study that ships as documented evidence in EVALUATION.md §6; this
server is the live, exploratory counterpart, sharing the same transform
logic via scripts/_plumb_lib.py.

stdlib only (http.server, json, subprocess) — same convention as every
other Python file in this repo. Single-threaded on purpose: opt
subprocesses share scratch file paths, and this is a local single-user
dev tool, not a production service — concurrent requests just queue.

Env (set by scripts/validation_server.sh): ROOT, CLANG, OPT, PASS_LIB,
PORT, CLANG_FLAGS_JSON.
"""
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plumb_lib as lib

ROOT = os.environ["ROOT"]
CLANG = os.environ["CLANG"]
OPT = os.environ["OPT"]
PASS_LIB = os.environ["PASS_LIB"]
WEIGHTS = os.path.join(ROOT, "config", "weights.cfg")
IR_DIR = os.path.join(ROOT, "ir")
RESULTS_DIR = os.path.join(ROOT, "results")
UPLOADS_DIR = os.path.join(ROOT, "uploads")
SCRATCH_DIR = os.path.join(UPLOADS_DIR, "_scratch")
DASHBOARD_DIR = os.path.join(ROOT, "dashboard")
CLANG_FLAGS = json.loads(os.environ.get("CLANG_FLAGS_JSON", "[]"))
PORT = int(os.environ.get("PORT", "8420"))

lib.configure(OPT, PASS_LIB, WEIGHTS)

# Start clean — stale scratch IR from a previous session should never be
# read back as if it were this session's output.
if os.path.isdir(SCRATCH_DIR):
    shutil.rmtree(SCRATCH_DIR)
os.makedirs(UPLOADS_DIR, exist_ok=True)
os.makedirs(SCRATCH_DIR, exist_ok=True)


# ── locating & listing analyzed programs ───────────────────────────────
def ll_and_json_paths(testcase, level):
    """Prefer a session upload over a shipped testcase of the same name."""
    for ll_dir, json_dir in [(UPLOADS_DIR, UPLOADS_DIR), (IR_DIR, RESULTS_DIR)]:
        ll = os.path.join(ll_dir, f"{testcase}.{level}.ll")
        js = os.path.join(json_dir, f"{testcase}.{level}.json")
        if os.path.exists(ll) and os.path.exists(js):
            return ll, js
    return None, None


def list_testcases():
    seen = {}
    for ir_dir, results_dir, source in [(IR_DIR, RESULTS_DIR, "builtin"),
                                         (UPLOADS_DIR, UPLOADS_DIR, "upload")]:
        if not os.path.isdir(ir_dir):
            continue
        for fname in sorted(os.listdir(ir_dir)):
            m = re.match(r"^(.+)\.(O0|O2)\.ll$", fname)
            if not m:
                continue
            name, level = m.group(1), m.group(2)
            json_path = os.path.join(results_dir, f"{name}.{level}.json")
            if not os.path.exists(json_path):
                continue
            report = json.load(open(json_path))
            seen[(source, name, level)] = {
                "testcase": name, "level": level, "source": source,
                "functions": [
                    {"name": f["name"], "recommendations": f.get("recommendations", []),
                     "totalCost": f["totalCost"]}
                    for f in report["functions"]
                ],
            }
    return [seen[k] for k in sorted(seen)]


def safe_name(filename):
    base = re.sub(r"\.(c|cc|cpp|cxx)$", "", filename, flags=re.I)
    base = re.sub(r"[^A-Za-z0-9_]+", "_", base).strip("_") or "upload"
    known = set()
    for d in (IR_DIR, UPLOADS_DIR):
        if os.path.isdir(d):
            known |= {re.sub(r"\.(O0|O2)\.ll$", "", f) for f in os.listdir(d) if f.endswith(".ll")}
    candidate, n = base, 1
    while candidate in known:
        n += 1
        candidate = f"{base}_{n}"
    return candidate


def compile_and_analyze(name, source_text):
    src_path = os.path.join(UPLOADS_DIR, f"{name}.c")
    lib.write(src_path, source_text)
    out = {}
    for level, flag in [("O0", "-O0"), ("O2", "-O2")]:
        ll_path = os.path.join(UPLOADS_DIR, f"{name}.{level}.ll")
        cmd = [CLANG] + CLANG_FLAGS + [flag, "-S", "-emit-llvm", src_path, "-o", ll_path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(ll_path):
            raise RuntimeError(f"clang failed compiling at {flag}:\n{r.stderr}")
        json_path = os.path.join(UPLOADS_DIR, f"{name}.{level}.json")
        out[level] = lib.plumb_run(ll_path, json_path, level)
    return out


# ── per-tag live validators — same transforms as validate_recommendations.py,
#    generalized to any (testcase, level, function) instead of a fixed set ──
def _scratch_tag(testcase, level, function):
    return f"{testcase}_{level}_{function}_{int(time.time() * 1000)}"


def label_report(report, label):
    """Deep-copy a Plumb JSON and set metadata.runLabel for dashboard A/B pills."""
    out = json.loads(json.dumps(report))
    out.setdefault("metadata", {})["runLabel"] = label
    return out


def diff_functions(before, after):
    b = {f["name"]: f["totalCost"] for f in before["functions"]}
    a = {f["name"]: f["totalCost"] for f in after["functions"]}
    changed = []
    for name in sorted(set(b) | set(a)):
        bv, av = b.get(name), a.get(name)
        if bv != av:
            changed.append({"name": name, "before": bv, "after": av})
    return changed


def validate_inline_candidate(testcase, level, function):
    ll_path, json_path = ll_and_json_paths(testcase, level)
    src = lib.read(ll_path)
    tag = _scratch_tag(testcase, level, function)
    marked_path = os.path.join(SCRATCH_DIR, f"{tag}_marked.ll")
    lib.write(marked_path, lib.mark_alwaysinline(src, [function]))

    raw_ll = os.path.join(SCRATCH_DIR, f"{tag}_raw.ll")
    lib.opt_run(lib.ALWAYS_INLINE_ONLY, marked_path, raw_ll)

    cleaned_ll = os.path.join(SCRATCH_DIR, f"{tag}_cleaned.ll")
    lib.opt_run(lib.inline_cleanup_passes(lib.has_function(src, "main")), marked_path, cleaned_ll)

    before = json.load(open(json_path))
    after_raw = lib.plumb_run(raw_ll, os.path.join(SCRATCH_DIR, f"{tag}_raw.json"), "inline-raw")
    after_cleaned = lib.plumb_run(cleaned_ll, os.path.join(SCRATCH_DIR, f"{tag}_cleaned.json"), "inline-cleaned")

    return {
        "tag": "INLINE_CANDIDATE",
        "function": function,
        "before_total": before["metadata"]["totals"]["totalCost"],
        "after_raw_total": after_raw["metadata"]["totals"]["totalCost"],
        "after_cleaned_total": after_cleaned["metadata"]["totals"]["totalCost"],
        "remaining_calls": lib.call_count(lib.read(cleaned_ll), function),
        "changed_functions": diff_functions(before, after_cleaned),
        "compare_available": True,
        "before_report": label_report(before, f"Before · {function}"),
        "after_report": label_report(after_cleaned, f"After inline+cleanup · {function}"),
    }


def validate_vectorizable(testcase, level, function):
    ll_path, json_path = ll_and_json_paths(testcase, level)
    src = lib.read(ll_path)
    tag = _scratch_tag(testcase, level, function)
    clean_path = os.path.join(SCRATCH_DIR, f"{tag}_clean.ll")
    lib.write(clean_path, lib.strip_blocking_attrs(src))
    vec_path = os.path.join(SCRATCH_DIR, f"{tag}_vec.ll")
    vec_ir = lib.opt_run(lib.VECTORIZE_PASSES, clean_path, vec_path)
    vec_insts = lib.count_vector_instructions(lib.function_body(vec_ir, function))

    before = json.load(open(json_path))
    after = lib.plumb_run(vec_path, os.path.join(SCRATCH_DIR, f"{tag}_vec.json"), "vectorized")
    b, a = lib.func_json(before, function), lib.func_json(after, function)
    return {
        "tag": "VECTORIZABLE",
        "function": function,
        "vector_instructions": vec_insts,
        "before_cost": b["totalCost"] if b else None,
        "after_cost": a["totalCost"] if a else None,
        "verdict": "vectorized" if vec_insts > 0 else "declined",
        "compare_available": True,
        "before_report": label_report(before, f"Before · {function}"),
        "after_report": label_report(after, f"After vectorize · {function}"),
    }


def validate_recursive(testcase, level, function):
    ll_path, json_path = ll_and_json_paths(testcase, level)
    src = lib.read(ll_path)
    tag = _scratch_tag(testcase, level, function)
    cleaned = lib.strip_blocking_attrs(src)
    clean_path = os.path.join(SCRATCH_DIR, f"{tag}_clean.ll")
    lib.write(clean_path, cleaned)
    tce_path = os.path.join(SCRATCH_DIR, f"{tag}_tce.ll")
    tce_ir = lib.opt_run(lib.TAILCALLELIM_PASSES, clean_path, tce_path)
    before_calls = lib.self_call_count(cleaned, function)
    after_calls = lib.self_call_count(tce_ir, function)
    before = json.load(open(json_path))
    after = lib.plumb_run(tce_path, os.path.join(SCRATCH_DIR, f"{tag}_tce.json"), "tailcallelim")
    return {
        "tag": "RECURSIVE",
        "function": function,
        "before_calls": before_calls,
        "after_calls": after_calls,
        "verdict": "eliminated" if after_calls == 0 else "survives",
        "compare_available": True,
        "before_report": label_report(before, f"Before · {function}"),
        "after_report": label_report(after, f"After -tailcallelim · {function}"),
    }


def validate_hotspot(testcase, level, function):
    other_level = "O2" if level == "O0" else "O0"
    _, json_path = ll_and_json_paths(testcase, level)
    other_ll, other_json = ll_and_json_paths(testcase, other_level)
    if not other_json:
        raise RuntimeError(f"no {other_level} analysis available for {testcase} to compare against")
    this_report = json.load(open(json_path))
    other_report = json.load(open(other_json))
    this_f = lib.func_json(this_report, function)
    other_f = lib.func_json(other_report, function)
    resolved = other_f is None or "HOTSPOT" not in other_f.get("recommendations", [])
    return {
        "tag": "HOTSPOT",
        "function": function,
        "this_level": level, "other_level": other_level,
        "this_cost": this_f["totalCost"] if this_f else None,
        "other_cost": other_f["totalCost"] if other_f else "gone (inlined away entirely)",
        "resolved": resolved,
        "compare_available": True,
        "before_report": label_report(this_report, level),
        "after_report": label_report(other_report, other_level),
    }


def validate_high_complexity(testcase, level, function):
    return {
        "tag": "HIGH_COMPLEXITY",
        "function": function,
        "automatable": False,
        "compare_available": False,
        "message": ("No LLVM pass rewrites a switch/if-ladder into a lower-complexity "
                    "equivalent — that's a manual, code-specific refactor. See "
                    "EVALUATION.md §6.5 for a worked example (categorize → "
                    "categorize_v2, CC 23 → 1). This tag can't be validated "
                    "automatically for an arbitrary function."),
    }


VALIDATORS = {
    "INLINE_CANDIDATE": validate_inline_candidate,
    "VECTORIZABLE": validate_vectorizable,
    "RECURSIVE": validate_recursive,
    "HOTSPOT": validate_hotspot,
    "HIGH_COMPLEXITY": validate_high_complexity,
}


# ── HTTP server ──────────────────────────────────────────────────────
class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DASHBOARD_DIR, **kwargs)

    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(length) or b"{}")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/":
            self.send_response(302)
            self.send_header("Location", "/dashboard.html")
            self.end_headers()
            return
        if parsed.path == "/api/testcases":
            try:
                self._send_json({"testcases": list_testcases()})
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return
        if parsed.path == "/api/results":
            qs = urllib.parse.parse_qs(parsed.query)
            testcase, level = qs.get("testcase", [""])[0], qs.get("level", [""])[0]
            _, json_path = ll_and_json_paths(testcase, level)
            if not json_path:
                self._send_json({"error": f"no analysis found for {testcase}.{level}"}, 404)
                return
            self._send_json(json.load(open(json_path)))
            return
        super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        try:
            if parsed.path == "/api/upload":
                body = self._read_json_body()
                name = safe_name(body.get("filename", "upload.c"))
                result = compile_and_analyze(name, body["source"])
                self._send_json({"testcase": name, "O0": result["O0"], "O2": result["O2"]})
                return
            if parsed.path == "/api/validate":
                body = self._read_json_body()
                testcase, level, function, tag = (
                    body["testcase"], body["level"], body["function"], body["tag"])
                validator = VALIDATORS.get(tag)
                if not validator:
                    self._send_json({"error": f"unknown tag {tag}"}, 400)
                    return
                ll_path, json_path = ll_and_json_paths(testcase, level)
                if not ll_path:
                    self._send_json({"error": f"no analysis found for {testcase}.{level}"}, 404)
                    return
                report = json.load(open(json_path))
                f = lib.func_json(report, function)
                if not f:
                    self._send_json({"error": f"function {function} not found in {testcase}.{level}"}, 404)
                    return
                if tag != "HIGH_COMPLEXITY" and tag not in f.get("recommendations", []):
                    self._send_json({"error": f"{function} is not currently flagged {tag}"}, 400)
                    return
                self._send_json(validator(testcase, level, function))
                return
            self._send_json({"error": "not found"}, 404)
        except Exception as e:
            self._send_json({"error": str(e)}, 500)

    def log_message(self, fmt, *args):
        sys.stderr.write("[validation-server] " + (fmt % args) + "\n")


def main():
    with http.server.HTTPServer(("127.0.0.1", PORT), Handler) as httpd:
        print(f"[validation-server] listening on http://127.0.0.1:{PORT}")
        print(f"[validation-server] open: http://127.0.0.1:{PORT}/dashboard.html")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[validation-server] stopped")


if __name__ == "__main__":
    main()
