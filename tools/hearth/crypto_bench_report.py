#!/usr/bin/env python3
"""The crypto bench's comparison matrix: the boards' serial output and the PC's JSON as Markdown.

    python tools/hearth/crypto_bench_report.py --board esp32s3=s3.log --board esp32c6=c6.log \
        --board esp32p4=p4.log --pc crypto_bench_pc.json

A board's log is what firmware/crypto-bench prints (INFO, CHECK, BENCH, FOOT and DONE lines, see
its main/crypto_bench.cpp); the last whole pass in it is the one read, so a capture that began in
the middle of one, or ended in the middle of another, still gives a result. The PC's file is
tests/performance/crypto_bench.cpp's --json-out.

"software" is iclforge::base::crypto's portable C++ and "hardware" is the PSA Crypto API, which
ESP-IDF routes to the chip's SHA peripheral; for the AEAD rows the two columns are ChaCha20-Poly1305
(software on every ESP part) and AES-256-GCM (the AES peripheral). Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# (title, software case, hardware case, message sizes) of each comparison.
MEMORIES = ["internal", "psram", "flash"]
TIMESTAMP = re.compile(r"^\[\s*\d+\.\d+\]\s*")

FAMILIES = [
    ("SHA-256", "sha256.base", "sha256.psa", [64, 256, 1024, 2304, 4096, 16384, 65536]),
    ("HMAC-SHA-256, 32-byte key", "hmac_sha256.base", "hmac_sha256.psa", [800, 2304, 4608]),
    (
        "Sendspin AEAD seal: ChaCha20-Poly1305 against AES-256-GCM",
        "aead.chacha20poly1305.psa",
        "aead.aes256gcm.psa",
        [256, 1024, 1500, 4096],
    ),
]


# What the encrypted channel carries to a sink while it plays a coded stream: one burst chunk per
# access unit, 1,536 samples at 48 kHz, so 31.25 a second. The sizes are the programme's recorded
# runs (the Music Assistant fork's lanes, 7130/7130 bursts at 768 kb/s = 21,903,360 B; 448 kb/s =
# 1,792 B a burst; the C6's stereo lane 128 kb/s = 512 B a burst), not a measurement made here.
BURSTS_PER_SECOND = 31.25
LANES = [("C6 stereo lane, 128 kb/s", 512), ("448 kb/s lane", 1792), ("768 kb/s lane", 3072)]


@dataclass
class Case:
    min_cycles: int
    mean_cycles: float
    calls: int
    mb_per_s: float


@dataclass
class Pass:
    # INFO,<chip>,<revision>,<idf>,<cpu_mhz>,<internal_free>,<psram_free>,<crypto_clock>,<flash_dma>
    # with the INFO itself already dropped.
    info: list[str] = field(default_factory=list)
    cases: dict[tuple[str, str, int], Case] = field(default_factory=dict)
    checks: list[tuple[str, int, bool]] = field(default_factory=list)
    foot: dict[str, tuple[int, int]] = field(default_factory=dict)
    left: dict[tuple[str, int], float] = field(default_factory=dict)

    @property
    def mhz(self) -> int:
        return int(self.info[3]) if len(self.info) > 3 else 0

    @property
    def crypto_clock(self) -> int:
        """The crypto clock selector's raw value, or -1 on a chip without one."""
        return int(self.info[6]) if len(self.info) > 6 else -1


def parse_pass(text: str) -> Pass | None:
    """The last pass in `text` that ends in DONE, or None."""
    current: Pass | None = None
    last: Pass | None = None
    for line in text.splitlines():
        # bench.py console puts the seconds since it opened the port in front of every line.
        fields = TIMESTAMP.sub("", line.strip()).split(",")
        kind = fields[0]
        if kind == "INFO":
            current = Pass(info=fields[1:])
        elif current is None:
            continue
        elif kind == "CHECK" and len(fields) == 4:
            current.checks.append((fields[1], int(fields[2]), fields[3] == "1"))
        elif kind == "BENCH" and len(fields) == 10:
            _, _, _, name, memory, size, low, mean, calls, rate = fields
            current.cases[(name, memory, int(size))] = Case(
                int(low), float(mean), int(calls), float(rate)
            )
        elif kind == "FOOT" and len(fields) == 5:
            current.foot[fields[2]] = (int(fields[3]), int(fields[4]))
        elif kind == "LEFT" and len(fields) == 5:
            current.left[(fields[2], int(fields[3]))] = float(fields[4])
        elif kind == "DONE":
            last = current
            current = None
    return last


def table(headers: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join(" --- " for _ in headers) + "|"]
    out += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(out)


def ratio(slow: float, fast: float) -> str:
    return f"{slow / fast:.1f}x" if fast > 0 else "-"


def family_tables(boards: dict[str, Pass]) -> list[str]:
    sections = []
    for title, software, hardware, sizes in FAMILIES:
        latency_headers = ["bytes"]
        rate_headers = ["bytes"]
        for chip in boards:
            latency_headers += [f"{chip} software", f"{chip} hardware", "speedup"]
            rate_headers += [f"{chip} software", f"{chip} hardware"]
        latency, rates = [], []
        for size in sizes:
            lrow, rrow = [str(size)], [str(size)]
            for run in boards.values():
                sw = run.cases.get((software, "internal", size))
                hw = run.cases.get((hardware, "internal", size))
                if sw is None or hw is None:
                    lrow += ["-", "-", "-"]
                    rrow += ["-", "-"]
                    continue
                lrow += [
                    f"{sw.min_cycles:,} cyc ({sw.min_cycles / run.mhz:.1f} us)",
                    f"{hw.min_cycles:,} cyc ({hw.min_cycles / run.mhz:.1f} us)",
                    ratio(sw.min_cycles, hw.min_cycles),
                ]
                rrow += [f"{sw.mb_per_s:.2f}", f"{hw.mb_per_s:.2f}"]
            latency.append(lrow)
            rates.append(rrow)
        sections.append(f"### {title}: latency, fastest call\n\n{table(latency_headers, latency)}")
        sections.append(f"### {title}: throughput, MB/s\n\n{table(rate_headers, rates)}")
    return sections


def memory_table(boards: dict[str, Pass]) -> str | None:
    """SHA-256 over PSA by where the message lives: internal RAM, PSRAM (DMA through the cache)
    or flash (no DMA: ESP-IDF writes it a block at a time through the registers)."""
    rows = []
    for chip, run in boards.items():
        for size in sorted({s for (name, _, s) in run.cases if name == "sha256.psa"}):
            rates = [run.cases.get(("sha256.psa", memory, size)) for memory in MEMORIES]
            if sum(case is not None for case in rates) > 1:
                rows.append([chip, str(size)] + [f"{c.mb_per_s:.2f}" if c else "-" for c in rates])
    if not rows:
        return None
    headers = ["chip", "bytes"] + [f"{memory} MB/s" for memory in MEMORIES]
    return "### SHA-256 over PSA by where the message lives\n\n" + table(headers, rows)


def clock_table(boards: dict[str, Pass]) -> str | None:
    """The same PSA hash with the crypto clock on the crystal ("@xtal") against what ESP-IDF
    selects at startup, on the chips where the clock can be chosen."""
    rows = []
    for chip, run in boards.items():
        if run.crypto_clock < 0:
            continue
        for size in sorted({s for (name, _, s) in run.cases if name == "sha256.psa@xtal"}):
            xtal = run.cases[("sha256.psa@xtal", "internal", size)]
            boot = run.cases.get(("sha256.psa", "internal", size))
            if boot is not None:
                rows.append(
                    [chip, str(run.crypto_clock), str(size), f"{xtal.min_cycles:,}",
                     f"{boot.min_cycles:,}", ratio(xtal.min_cycles, boot.min_cycles)]
                )
    if not rows:
        return None
    headers = [
        "chip",
        "selector at boot",
        "bytes",
        "crystal (cycles)",
        "at boot (cycles)",
        "speedup",
    ]
    return "### Crypto clock: crystal against the PLL ESP-IDF selects\n\n" + table(headers, rows)


def left_table(boards: dict[str, Pass]) -> str | None:
    """The CPU a call leaves to a lower-priority task on the same core, as a share of idle."""
    keys = sorted({key for run in boards.values() for key in run.left})
    if not keys:
        return None
    rows = []
    for name, size in keys:
        row = [name, str(size)]
        for run in boards.values():
            value = run.left.get((name, size))
            row.append(f"{value:.0f} %" if value is not None else "-")
        rows.append(row)
    headers = ["call", "bytes"] + [f"{chip} CPU left" for chip in boards]
    return "### CPU left for other tasks while the call repeats\n\n" + table(headers, rows)


def footprint_table(boards: dict[str, Pass]) -> str | None:
    names = sorted({name for run in boards.values() for name in run.foot if name != "empty"})
    if not names:
        return None
    headers = ["case"] + [f"{chip} stack / heap peak (B)" for chip in boards]
    rows = []
    for name in names:
        row = [name]
        for run in boards.values():
            stack, heap = run.foot.get(name, (0, 0))
            row.append(f"{stack:,} / {heap:,}" if name in run.foot else "-")
        rows.append(row)
    return "### Memory beyond an empty call, in a task of its own\n\n" + table(headers, rows)


def cycles_at(run: Pass, name: str, size: int) -> float | None:
    """The cycles of `name` for a message of `size` bytes: the measured sizes joined by straight
    lines, and the last two carried on past the ends."""
    points = sorted((s, c.min_cycles) for (n, memory, s), c in run.cases.items()
                    if n == name and memory == "internal")
    if len(points) < 2:
        return None
    upper = next((i for i, (s, _) in enumerate(points) if s >= size), len(points) - 1)
    (x0, y0), (x1, y1) = points[max(upper - 1, 0)], points[max(upper, 1)]
    return y0 + (y1 - y0) * (size - x0) / (x1 - x0)


def capacity_table(boards: dict[str, Pass]) -> str | None:
    """What sealing (or, the same work, opening) the sink's bursts costs as a share of the core
    that does it, per suite: the cycles ChaCha20-Poly1305 and AES-256-GCM take at the sizes the
    lanes carry. On a Hearth board that core is core 0 (the Sendspin host, with lwIP and the radio),
    not the decoder's core 1; the C6 has the one."""
    rows = []
    for label, size in LANES:
        for chip, run in boards.items():
            chacha = cycles_at(run, "aead.chacha20poly1305.psa", size)
            gcm = cycles_at(run, "aead.aes256gcm.psa", size)
            if chacha is None or gcm is None or run.mhz == 0:
                continue
            chacha_us, gcm_us = chacha / run.mhz, gcm / run.mhz
            share = BURSTS_PER_SECOND / 1e4  # microseconds a burst to percent of a core
            rows.append(
                [f"{label} ({size} B)", chip, f"{chacha_us:,.0f}", f"{gcm_us:,.0f}",
                 f"{chacha_us * share:.1f} %", f"{gcm_us * share:.1f} %",
                 f"{(chacha_us - gcm_us) * share:+.1f} %"]
            )
    if not rows:
        return None
    headers = ["lane (bytes a burst)", "chip", "ChaCha us", "AES-GCM us", "ChaCha, core share",
               "AES-GCM, core share", "freed by GCM"]
    return (
        f"### The Sendspin AEAD at the lanes' burst sizes "
        f"({BURSTS_PER_SECOND:g} bursts a second)\n\n"
        + table(headers, rows)
    )


def checks_section(boards: dict[str, Pass]) -> str:
    rows = []
    for chip, run in boards.items():
        failed = [f"{what} {size} B" for what, size, ok in run.checks if not ok]
        rows.append([chip, str(len(run.checks)), ", ".join(failed) if failed else "all equal"])
    return (
        "### Does the hardware path give the portable answer?\n\n"
        + table(["chip", "comparisons", "PSA against portable, bit for bit"], rows)
    )


def pc_section(path: Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    ns = {k["name"]: k["ns_per_call"] for k in data["kernels"]}
    rows = []
    for name, value in ns.items():
        match = re.fullmatch(r"sha256_compress_portable_(\d+)B", name)
        if match:
            size = int(match.group(1))
            hardware = ns.get(f"sha256_compress_hardware_{size}B")
            if hardware:
                rows.append(
                    [f"SHA-256 compression, {size} B", f"{value:,.0f} ns", f"{hardware:,.0f} ns",
                     f"{size / value * 1e3:,.0f} MB/s", f"{size / hardware * 1e3:,.0f} MB/s",
                     ratio(value, hardware)]
                )
    for name, value in ns.items():
        match = re.fullmatch(r"hmac_sha256_(\d+)B", name)
        if match:
            rows.append([f"HMAC-SHA-256 (dispatching), {match.group(1)} B", "-", f"{value:,.0f} ns",
                         "-", f"{int(match.group(1)) / value * 1e3:,.0f} MB/s", "-"])
    headers = ["case", "portable", "hardware", "portable MB/s", "hardware MB/s", "speedup"]
    return "### PC\n\n" + table(headers, rows)


def render(boards: dict[str, Pass], pc: Path | None) -> str:
    parts = ["## Boards (cycles from the CPU's cycle counter; software = portable C++)"]
    parts.append(
        "\n".join(
            f"- {chip}: chip revision {run.info[1]}, ESP-IDF {run.info[2]}, {run.mhz} MHz"
            for chip, run in boards.items()
        )
    )
    parts.append(checks_section(boards))
    parts += family_tables(boards)
    for extra in (capacity_table(boards), memory_table(boards), clock_table(boards),
                  left_table(boards), footprint_table(boards)):
        if extra:
            parts.append(extra)
    if pc is not None:
        parts.append(pc_section(pc))
    return "\n\n".join(parts) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--board", action="append", default=[], metavar="CHIP=LOG")
    parser.add_argument("--pc", type=Path, metavar="JSON")
    args = parser.parse_args(argv)

    boards: dict[str, Pass] = {}
    for spec in args.board:
        chip, _, log = spec.partition("=")
        run = parse_pass(Path(log).read_text(encoding="utf-8", errors="replace"))
        if run is None:
            print(f"{log}: no whole pass (INFO .. DONE) in it", file=sys.stderr)
            return 1
        boards[chip] = run
    sys.stdout.write(render(boards, args.pc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
