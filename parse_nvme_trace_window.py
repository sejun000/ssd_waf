#!/usr/bin/env python3
"""
Convert trace-cmd raw nvme_setup_cmd output into a compact CSV trace,
optionally keeping only a relative-time window and writing a summary.

Output CSV format:
    relative_time,RW,sector,nr_sector,bit8
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


LINE_RE = re.compile(
    r"""
    ^.*?
    \]\s+
    (?P<ts>\d+\.\d+):
    \s+nvme_setup_cmd:\s+
    .*?disk=(?P<disk>\S+)
    .*?opcode=(?P<opcode>\d+)
    .*?cdw10=ARRAY\[(?P<cdw>[^\]]+)\]
    """,
    re.VERBOSE,
)


@dataclass
class Counts:
    rows: int = 0
    zero: int = 0
    one: int = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert trace-cmd .dat into compact CSV with optional time window."
    )
    parser.add_argument("input_dat", help="Input trace-cmd .dat file")
    parser.add_argument(
        "-o",
        "--output",
        help="Output CSV path (default: <input>.trace)",
    )
    parser.add_argument(
        "--summary-out",
        help="Optional summary output path",
    )
    parser.add_argument(
        "--disk",
        default="nvme2n1",
        help="Only keep events for this disk (default: nvme2n1)",
    )
    parser.add_argument(
        "--start-seconds",
        type=float,
        default=0.0,
        help="Keep events starting at this relative time (default: 0)",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=None,
        help="Keep events up to this relative time (default: no upper bound)",
    )
    return parser.parse_args()


def decode_line(line: str, disk_filter: str, base_ts: float | None):
    match = LINE_RE.match(line)
    if not match or match.group("disk") != disk_filter:
        return None, base_ts

    opcode = int(match.group("opcode"))
    if opcode == 1:
        rw = "W"
    elif opcode == 2:
        rw = "R"
    else:
        return None, base_ts

    ts = float(match.group("ts"))
    if base_ts is None:
        base_ts = ts
    rel_ts = ts - base_ts

    raw_bytes = [part.strip() for part in match.group("cdw").split(",")]
    if len(raw_bytes) != 24:
        return None, base_ts

    try:
        cdw = [int(part, 16) for part in raw_bytes]
    except ValueError:
        return None, base_ts

    slba = int.from_bytes(bytes(cdw[0:8]), byteorder="little", signed=False)
    cdw12 = int.from_bytes(bytes(cdw[8:12]), byteorder="little", signed=False)
    cdw13 = int.from_bytes(bytes(cdw[12:16]), byteorder="little", signed=False)

    nlb = (cdw12 & 0xFFFF) + 1
    sector = slba * 8
    nr_sector = nlb * 8
    bit8 = (cdw13 >> 8) & 1

    return (rel_ts, rw, sector, nr_sector, bit8), base_ts


def write_summary(path: Path, output_path: Path, counts: Counts) -> None:
    total = counts.rows
    with path.open("w", encoding="ascii") as out:
        out.write(f"output={output_path}\n")
        out.write(f"rows={counts.rows}\n")
        out.write(f"zero={counts.zero}\n")
        out.write(f"one={counts.one}\n")
        if total:
            out.write(f"zero_pct={counts.zero / total * 100:.4f}\n")
            out.write(f"one_pct={counts.one / total * 100:.4f}\n")
            if counts.one:
                out.write(f"ratio_zero_to_one={counts.zero / counts.one:.4f}\n")
                out.write(f"diff={counts.zero - counts.one}\n")


def main() -> int:
    args = parse_args()

    input_path = Path(args.input_dat)
    if not input_path.exists():
        print(f"Input file not found: {input_path}", file=sys.stderr)
        return 1

    output_path = Path(args.output) if args.output else input_path.with_suffix(".trace")
    summary_path = Path(args.summary_out) if args.summary_out else None

    cmd = ["sudo", "trace-cmd", "report", "-R", "-i", str(input_path)]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    base_ts: float | None = None
    counts = Counts()

    try:
        assert proc.stdout is not None
        with output_path.open("w", encoding="ascii") as out:
            for line in proc.stdout:
                decoded, base_ts = decode_line(line, args.disk, base_ts)
                if decoded is None:
                    continue

                rel_ts, rw, sector, nr_sector, bit8 = decoded

                if rel_ts < args.start_seconds:
                    continue
                if args.max_seconds is not None and rel_ts > args.max_seconds:
                    break

                out.write(f"{rel_ts:.9f},{rw},{sector},{nr_sector},{bit8}\n")
                counts.rows += 1
                if bit8:
                    counts.one += 1
                else:
                    counts.zero += 1
    finally:
        if proc.stdout is not None:
            proc.stdout.close()

    stderr = ""
    if proc.stderr is not None:
        stderr = proc.stderr.read()
        proc.stderr.close()

    ret = proc.wait()
    if ret != 0:
        print(stderr.strip() or f"trace-cmd failed with exit code {ret}", file=sys.stderr)
        return ret

    if summary_path is not None:
        write_summary(summary_path, output_path, counts)

    print(f"Wrote {counts.rows} rows to {output_path}")
    if summary_path is not None:
        print(f"Summary written to {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
