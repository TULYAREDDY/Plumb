# benchmarks/

Plumb on **real LLVM code** — not just the six toy testcases under [`testcases/`](../testcases/).

This directory runs Plumb across the official [LLVM test-suite](https://github.com/llvm/llvm-test-suite)'s `SingleSource/Benchmarks` subset (Stanford, Misc, Polybench, Shootout) and aggregates the output into a single signed report.

## Headline numbers

83 programs · 1,165 functions · 166 analysis runs · ~20 s end-to-end on Apple Silicon.

See [**SUMMARY.md**](SUMMARY.md) for the full report — top-15 hottest programs, cost-class dominance, per-suite breakdown, and the in-the-wild rate of the -O2 inlining failure mode.

## Files

| File | Purpose |
|---|---|
| [`fetch.sh`](fetch.sh)         | sparse-clones LLVM test-suite (~30 MB of source) |
| [`run_bench.sh`](run_bench.sh) | compiles every program to IR, runs Plumb at -O0 and -O2 |
| [`analyze.py`](analyze.py)     | aggregates `results/*.json` → `SUMMARY.md` |
| [`SUMMARY.md`](SUMMARY.md)     | the actual report (auto-generated, but committed) |
| `llvm-test-suite/`             | cloned upstream (gitignored) |
| `ir/` · `results/`             | per-program IR and JSON outputs (gitignored) |

## Run it

```bash
# from repo root
./build.sh                       # build Plumb (if not already built)
./benchmarks/fetch.sh            # one-time sparse clone
./benchmarks/run_bench.sh        # ~20 s sweep
python3 benchmarks/analyze.py    # regenerate SUMMARY.md
```

`run_bench.sh` accepts:

```bash
./benchmarks/run_bench.sh -p Polybench   # only Polybench programs
./benchmarks/run_bench.sh -j 4           # parallel (currently sequential)
```

## Why this matters

A static cost model that hasn't been run on real code is a hypothesis, not a tool. The 83-program sweep is what lets Plumb claim:

- it agrees with intuition on canonical kernels (Jacobi, FFT, Polybench `kernel_*` functions all surface as hot)
- the documented failure modes in [EVALUATION.md §5](../EVALUATION.md#5-failure-cases) actually occur in the wild, and at what rate (e.g. -O2-inflation: 4% of programs)
- median cost reduction at -O2 across 83 programs is 56%, with a long tail of 70-95% reductions on tight numerical kernels

All numbers in `SUMMARY.md` are produced from the JSON in `results/` — re-running the harness regenerates the report end-to-end.
