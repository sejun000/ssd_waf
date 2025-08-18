#!/usr/bin/env python3
"""
plot_metrics.py  –  flexible trace visualizer (delta_* + expressions)
===================================================================
• 여러 trace 로그를 받아 원하는 지표를 꺾은선/산점도로 비교.
• y / x 에 단일 필드명 또는 수식(사칙, sqrt, log 등) 지정 가능.
• delta_<field> 토큰 → 현재 행 − 직전 행.
• --xlabel / --ylabel 로 축 라벨 지정, y‑축 하한은 항상 0.

usage example
-------------
python plot_metrics.py \
    --files run1.log,run2.log \
    --labels FIFO,Greedy \
    --y '(delta_invalidate_blocks*4096) / delta_write_size_to_cache' \
    --ylabel 'ΔInvalidate * 4 KiB / ΔWriteSize' \
    --x write_size_to_cache \
    --xscale GiB \
    --title Trace_Comparison
"""
import argparse
import math
import re
import sys
from pathlib import Path
from typing import Tuple

import matplotlib.pyplot as plt
import numpy as np

# ────────────────────────────────────────────────────────────────────────────────
# constants
TOKEN_RE = re.compile(r"(\w+):\s*([-+]?\d+)")
# byte‑unit scale factors
UNIT = {
    None: 1,
    "B": 1,
    "KB": 1 / 10 ** 3,
    "MB": 1 / 10 ** 6,
    "GB": 1 / 10 ** 9,
    "TB": 1 / 10 ** 12,
    "KiB": 1 / 2 ** 10,
    "MiB": 1 / 2 ** 20,
    "GiB": 1 / 2 ** 30,
    "TiB": 1 / 2 ** 40,
}
# safe math funcs allowed in expressions
SAFE_FUNCS = {k: getattr(math, k) for k in (
    "sqrt",
    "log",
    "log10",
    "exp",
    "pow",
    "sin",
    "cos",
    "tan",
)}

# ────────────────────────────────────────────────────────────────────────────────
# helpers
def parse_line(line: str) -> dict:
    """Convert one log line to dict(field -> int)."""
    return {k: int(v) for k, v in TOKEN_RE.findall(line)}


def eval_expr(expr: str, vars_: dict):
    """Safely eval expression with given variables."""
    return eval(expr, {"__builtins__": None, **SAFE_FUNCS}, vars_)


def collect_xy(path: Path, x_expr: str, y_expr: str) -> Tuple[np.ndarray, np.ndarray]:
    xs, ys = [], []
    prev = None
    with path.open() as f:
        for idx, line in enumerate(f):
            if not "invalidate" in line:
                continue
            cur = parse_line(line)
            if not cur:
                continue

            # build delta_* entries
            if prev is not None:
                for k, v in list(cur.items()):  # list() to avoid dict resize during iter
                    cur[f"delta_{k}"] = v - prev.get(k, 0)
            else:
                for k, v in list(cur.items()):
                    cur[f"delta_{k}"] = None  # first row: undefined

            try:
                x_val = eval_expr(x_expr, cur)
                y_val = eval_expr(y_expr, cur)
            except Exception as e:
                print(f"[warn] {path.name}:{idx+1}: {e}", file=sys.stderr)
                prev = cur
                continue

            # skip points with undefined delta (first row)
            if x_val is None or y_val is None:
                prev = cur
                continue

            xs.append(float(x_val))
            ys.append(float(y_val))
            prev = cur

    return np.asarray(xs), np.asarray(ys)


# ────────────────────────────────────────────────────────────────────────────────
# main

def main():
    ap = argparse.ArgumentParser(description="Plot trace metrics (delta_* & expressions)")
    ap.add_argument("--files", required=True, help="comma-separated log files")
    ap.add_argument("--labels", required=True, help="comma-separated legend labels")
    ap.add_argument("--y", required=True, help="Y-axis expression or field name")
    ap.add_argument("--x", default="write_size_to_cache", help="X-axis expression/field")
    ap.add_argument("--ylabel", default=None, help="Custom Y-axis label")
    ap.add_argument("--xlabel", default=None, help="Custom X-axis label")
    ap.add_argument("--kind", choices=["line", "scatter"], default="line", help="Plot type")
    ap.add_argument("--xscale", choices=list(UNIT.keys()), default=None, help="Scale X bytes")
    ap.add_argument("--yscale", choices=list(UNIT.keys()), default=None, help="Scale Y bytes")
    ap.add_argument("--title", default="Trace Comparison", help="Figure title")
    ap.add_argument("--output", default="output.png", help="Output file name")
    args = ap.parse_args()

    files = [Path(p.strip()) for p in args.files.split(",")]
    labels = [l.strip() for l in args.labels.split(",")]
    if len(files) != len(labels):
        ap.error("files 와 labels 개수가 달라야 합니다.")

    x_factor = UNIT[args.xscale]
    y_factor = UNIT[args.yscale]

    fig, ax = plt.subplots(figsize=(8, 5))

    for fp, lbl in zip(files, labels):
        x, y = collect_xy(fp, args.x, args.y)
        if x.size == 0:
            print(f"[warn] {fp} → no valid data points", file=sys.stderr)
            continue
        x *= x_factor
        y *= y_factor

        if args.kind == "scatter":
            ax.scatter(x, y, s=12, alpha=0.7, label=lbl)
        else:
            order = np.argsort(x)
            ax.plot(x[order], y[order], marker="o", ms=3, label=lbl)

    ax.set_xlabel(args.xlabel or (args.x if args.xscale is None else f"{args.x} ({args.xscale})"))
    ax.set_ylabel(args.ylabel or (args.y if args.yscale is None else f"{args.y} ({args.yscale})"))
    ax.set_title(args.title)

    # y‑axis starts at 0
    ymin, ymax = ax.get_ylim()
    ax.set_ylim(bottom=0, top=max(ymax, 0))

    ax.grid(True, ls="--", alpha=0.5)
    ax.legend()
    plt.tight_layout()
    plt.savefig(args.output)    
    plt.show()


if __name__ == "__main__":
    main()
