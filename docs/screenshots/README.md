# Demo screenshots

Add screenshots here for the project demo. Recommended set:

| Filename                | Capture |
|---|---|
| `01-terminal.png`         | full terminal output of `./run.sh --no-open` |
| `02-dashboard-overview.png` | dashboard with `results/test_floatmm.O0.json` loaded as A |
| `03-donut-chart.png`      | the per-group donut for `matmul` |
| `04-function-ranking.png` | the horizontal bar chart of functions by cost |
| `05-bb-heatmap.png`       | the basic-block heatmap with critical-path cells outlined |
| `06-cfg-graph.png`        | the Cytoscape CFG graph with the critical path glowing red |
| `07-weight-tuner.png`     | charts re-rendering after dragging the `memory` slider |
| `08-compare-O0-O2.png`    | side-by-side A vs B with `test_floatmm.O2.json` as B |
| `09-failure-case.png`     | `test_memheavy.O2.json` showing main's inflated cost (the §5.1 failure case) |

To capture on macOS: ⇧ ⌘ 4, drag the area, drop into this folder.
On Linux: `gnome-screenshot -a -f docs/screenshots/<name>.png`.
