#!/usr/bin/env python3
"""Compare a gba_emu CPU trace against a reference (mGBA) trace.

Usage:
    python3 compare_logs.py emu_trace.log mgba_trace.log [--max-report N]

Both logs are parsed line by line into normalized CPU states. A state is a
dict of PC, CPSR and R0-R14. The first line where any field differs is
reported with a field-by-field diff, plus a few lines of context.

Supported line formats (mixed freely; unparseable lines are skipped):
  gba_emu --trace:
    PC=08000000 CPSR=0000001F R0=00000000 R1=... R15=08000008
  mGBA debugger/trace styles, e.g.:
    086A0B4C:  E92D4010  stmfd sp!, ...
    r0: 00000000 r1: 00000000 ... cpsr: 6000001F
  or single-line combined dumps containing "rN: hex" / "rN=hex" pairs.

R15 is deliberately NOT compared: emulators disagree on whether to log the
pipelined value (instruction + 8/4) or the instruction address. PC is
compared instead, which both sides log as the executing instruction.
"""

import argparse
import re
import sys

# Matches "PC=08000000", "pc: 08000000", "R10=DEADBEEF", "r10: deadbeef",
# "cpsr: 6000001F", ...
FIELD_RE = re.compile(r"\b(pc|cpsr|r\d{1,2})\s*[:=]\s*(0x)?([0-9a-f]{1,8})\b",
                      re.IGNORECASE)
# Matches a leading bare address like "086A0B4C:" used by mGBA disassembly.
LEADING_ADDR_RE = re.compile(r"^\s*(0x)?([0-9A-Fa-f]{8})\s*:")

COMPARED_FIELDS = ["PC", "CPSR"] + [f"R{i}" for i in range(15)]


def parse_line(line):
    """Returns a {field: int} dict for one log line, or None if the line
    carries no register state."""
    state = {}
    match = LEADING_ADDR_RE.match(line)
    if match:
        state["PC"] = int(match.group(2), 16)
    for name, _, value in FIELD_RE.findall(line):
        state[name.upper()] = int(value, 16)
    return state if state else None


def load_states(path):
    """Parses a log into a list of per-instruction states.

    Reference logs sometimes split one instruction across several lines
    (disassembly line followed by register lines); successive fragments are
    merged until a field repeats, which starts the next instruction.
    """
    states = []
    current = {}
    try:
        f = open(path, "r", errors="replace")
    except OSError as e:
        sys.exit(f"error: cannot open {path}: {e}")
    with f:
        for line in f:
            fragment = parse_line(line)
            if fragment is None:
                continue
            if any(k in current for k in fragment):
                if current:
                    states.append(current)
                current = dict(fragment)
            else:
                current.update(fragment)
    if current:
        states.append(current)
    return states


def describe(state):
    parts = []
    for field in COMPARED_FIELDS:
        if field in state:
            parts.append(f"{field}={state[field]:08X}")
    return " ".join(parts) if parts else "<no parsed fields>"


def main():
    ap = argparse.ArgumentParser(
        description="Find where gba_emu execution diverges from mGBA.")
    ap.add_argument("ours", help="gba_emu trace (emu_trace.log)")
    ap.add_argument("reference", help="mGBA trace log")
    ap.add_argument("--max-report", type=int, default=3,
                    help="number of divergent instructions to report")
    ap.add_argument("--context", type=int, default=2,
                    help="matching instructions to print before a divergence")
    args = ap.parse_args()

    ours = load_states(args.ours)
    ref = load_states(args.reference)
    if not ours:
        sys.exit(f"error: no parseable CPU states in {args.ours}")
    if not ref:
        sys.exit(f"error: no parseable CPU states in {args.reference}")

    print(f"ours:      {len(ours)} instructions from {args.ours}")
    print(f"reference: {len(ref)} instructions from {args.reference}")

    reported = 0
    compared = min(len(ours), len(ref))
    for i in range(compared):
        diffs = []
        for field in COMPARED_FIELDS:
            if field in ours[i] and field in ref[i]:
                if ours[i][field] != ref[i][field]:
                    diffs.append(field)
        if not diffs:
            continue

        print(f"\n=== DIVERGENCE at instruction #{i} "
              f"(fields: {', '.join(diffs)}) ===")
        for j in range(max(0, i - args.context), i):
            print(f"  [{j}] both: {describe(ours[j])}")
        print(f"  [{i}] ours: {describe(ours[i])}")
        print(f"  [{i}] ref:  {describe(ref[i])}")
        for field in diffs:
            print(f"    {field}: ours={ours[i][field]:08X} "
                  f"ref={ref[i][field]:08X}")
        reported += 1
        if reported >= args.max_report:
            break

    if reported == 0:
        print(f"\nNo divergence in {compared} compared instructions.")
        if len(ours) != len(ref):
            print(f"note: lengths differ (ours={len(ours)}, "
                  f"ref={len(ref)}); the shorter log ended first.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
