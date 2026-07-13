#!/usr/bin/env python3
"""
scripts/_plumb_lib.py
────────────────────────────────────────────────────────────────────────
Shared LLVM-transform + IR-text helpers used by both
scripts/validate_recommendations.py (the fixed 5-tag case study on
Plumb's own test suite, shipped as documented evidence in
EVALUATION.md §6) and scripts/validation_server.py (live, per-function
validation for whatever program is currently loaded in the dashboard —
not just those 3 testcases). One implementation, two callers, so a fix
here can't silently diverge between the fixed case study and the live
path.
"""
import json
import os
import re
import subprocess

OPT = None
PASS_LIB = None
WEIGHTS = None


def configure(opt, pass_lib, weights):
    global OPT, PASS_LIB, WEIGHTS
    OPT, PASS_LIB, WEIGHTS = opt, pass_lib, weights


DEFINE_RE = re.compile(r'^define [^\n]*@(\w+)\([^\n]*\) (#\d+) \{', re.M)
ATTR_RE = re.compile(r'^attributes (#\d+) = \{ (.*) \}$', re.M)
VECTOR_TYPE_RE = re.compile(r'<\s*\d+\s*x\s')

# Named `opt` pass pipelines, shared verbatim between
# validate_recommendations.py's fixed case study and
# validation_server.py's live per-function validation, so the two can't
# silently diverge on what "the real transform for tag X" means.
ALWAYS_INLINE_ONLY = ["-always-inline"]
INLINE_CLEANUP_BASE = ["-always-inline", "-mem2reg", "-instcombine", "-adce"]
INLINE_CLEANUP_DCE = ["-internalize", "-internalize-public-api-list=main", "-globaldce"]
VECTORIZE_PASSES = ["-mem2reg", "-loop-rotate", "-loop-vectorize"]
TAILCALLELIM_PASSES = ["-tailcallelim"]


def inline_cleanup_passes(has_main_fn):
    """-always-inline plus the cleanup passes -O2's inliner always runs
    alongside it. -internalize/-globaldce additionally need a `main` to
    treat as the one true entry point — skipped for IR that doesn't
    define one, rather than risking internalizing (and stripping)
    genuinely external functions it doesn't actually have."""
    return INLINE_CLEANUP_BASE + (INLINE_CLEANUP_DCE if has_main_fn else [])


def read(path):
    with open(path) as f:
        return f.read()


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def opt_run(pass_args, in_path, out_path):
    cmd = [OPT, "-enable-new-pm=0"] + pass_args + ["-S", in_path, "-o", out_path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out_path):
        raise RuntimeError(
            f"opt failed (exit {r.returncode}): {' '.join(cmd)}\n{r.stderr}")
    return read(out_path)


def plumb_run(in_path, json_path, label, hot=30, inline=20):
    cmd = [
        OPT, "-enable-new-pm=0", "-load", PASS_LIB, "-plumb",
        f"-plumb-weight-file={WEIGHTS}",
        f"-plumb-hot-threshold={hot}",
        f"-plumb-inline-threshold={inline}",
        f"-plumb-run-label={label}",
        f"-plumb-json-file={json_path}",
        "-disable-output", in_path,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not os.path.exists(json_path):
        raise RuntimeError(
            f"Plumb pass produced no JSON (exit {r.returncode}): {' '.join(cmd)}\n{r.stderr}")
    return json.load(open(json_path))


def strip_blocking_attrs(text):
    """Remove `noinline`/`optnone` from every attribute group. clang -O0
    marks every function with both, which blocks every transform pass —
    confirmed live: -inline/-loop-vectorize/-tailcallelim are all no-ops
    until these are stripped."""
    def repl(m):
        return f"attributes {m.group(1)} = {{ " + \
            m.group(2).replace("noinline ", "").replace("optnone ", "") + " }"
    return ATTR_RE.sub(repl, text)


def mark_alwaysinline(text, fn_names):
    """Retarget only `fn_names` to a new attribute group carrying
    `alwaysinline` (minus noinline/optnone); every other function keeps
    its original attributes untouched. This makes `-always-inline` a
    surgical experiment — only the flagged functions get inlined, unlike
    a blanket `-inline`, which (confirmed live) also folds unrelated
    functions into main via its own cost heuristic."""
    fn_to_group = {m.group(1): m.group(2) for m in DEFINE_RE.finditer(text)}
    groups = {m.group(1): m.group(2) for m in ATTR_RE.finditer(text)}
    if fn_names[0] not in fn_to_group:
        raise ValueError(f"mark_alwaysinline: function {fn_names[0]!r} not found in IR")
    base_group = fn_to_group[fn_names[0]]
    base_attrs = groups[base_group]
    # Every target must share fn_names[0]'s original attribute group, since
    # base_attrs is only ever read from that one group — if a target
    # function has different attributes, silently reusing base_attrs would
    # retarget it to the wrong (unrelated) attribute set without error.
    mismatched = [f for f in fn_names[1:] if fn_to_group.get(f) != base_group]
    if mismatched:
        raise ValueError(
            f"mark_alwaysinline: {mismatched} not in the same attribute group "
            f"as {fn_names[0]!r} ({base_group}) — base_attrs would be wrong for them")
    new_attrs = "alwaysinline " + base_attrs.replace("noinline ", "").replace("optnone ", "")
    new_id = "#900"
    text += f"\nattributes {new_id} = {{ {new_attrs} }}\n"

    def repl(m):
        name, grp = m.group(1), m.group(2)
        if name in fn_names:
            return m.group(0).replace(grp + " {", new_id + " {", 1)
        return m.group(0)
    return DEFINE_RE.sub(repl, text)


def function_body(text, fn_name):
    m = re.search(r'^define [^\n]*@%s\(' % re.escape(fn_name), text, re.M)
    if not m:
        return ""
    end = text.index("\n}", m.start())
    return text[m.start():end]


def has_function(text, fn_name):
    return re.search(r'^define [^\n]*@%s\(' % re.escape(fn_name), text, re.M) is not None


def call_count(text, fn_name):
    """Calls to fn_name anywhere in `text` (module-wide) — used to check
    "does ANY caller still reach this function"."""
    return len(re.findall(r'call\s+[^\n]*@%s\b' % re.escape(fn_name), text))


def self_call_count(text, fn_name):
    """Calls to fn_name from WITHIN fn_name's own body only — i.e. true
    self-/recursive calls. Scoping to the function body matters: a
    function called once from main shouldn't count that as a
    "self-call"."""
    return call_count(function_body(text, fn_name), fn_name)


def count_vector_instructions(body_text):
    """Count IR *instructions* using a vector type, not raw regex matches
    — a single load/store instruction can mention its vector type twice
    (once for the loaded/stored value, once for the pointer-to-vector
    operand), so matching per line (one IR instruction per line in .ll
    text) avoids double-counting."""
    return sum(1 for line in body_text.splitlines() if VECTOR_TYPE_RE.search(line))


def func_json(report, name):
    for f in report["functions"]:
        if f["name"] == name:
            return f
    return None
