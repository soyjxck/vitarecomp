#!/usr/bin/env python3
"""Trap census: what an emitted module still cannot translate, by encoding.

    tools/trap-census.py <recomp_funcs.c> [--objdump PATH] [--kinds a,b] [--top N]

Every untranslated instruction in the generated C is a call of the form
vita_trap_unimpl(<pc>, <raw>, "<kind>"). This tallies them by raw encoding,
most frequent first, and prints `count kind raw`. With --objdump pointing at
an ARM objdump (the vitasdk one: arm-vita-eabi-objdump), it also writes the
top encodings to a binary, disassembles them as Thumb and prints the
mnemonic beside each count, which is what turns a list of hex words into an
order of work. Only the standard library is used.

The method found milestone B's order: the most frequent trap of the retail
title was `vdup.32` by lane, not anything the plan had put first.
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

TRAP = re.compile(r'vita_trap_unimpl\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), "([^"]*)"\)')


def tally(path):
    counts = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            for m in TRAP.finditer(line):
                counts[(m.group(3), int(m.group(2), 16))] += 1
    return counts


def disassemble(rows, objdump):
    """rows: [(count, kind, raw)]. Returns {offset: mnemonic}."""
    fd, binpath = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        with open(binpath, "wb") as b:
            for _count, _kind, raw in rows:
                if raw > 0xFFFF:
                    b.write(struct.pack("<HH", raw >> 16, raw & 0xFFFF))
                else:
                    b.write(struct.pack("<HH", raw, 0xBF00))      # pad with a nop
        out = subprocess.run([objdump, "-D", "-b", "binary", "-marm", "-Mforce-thumb", binpath],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(binpath)
    names = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$", line)
        if m:
            names[int(m.group(1), 16)] = m.group(2).strip()
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", help="the emitted recomp_funcs.c")
    ap.add_argument("--objdump", help="an ARM objdump; the top encodings are disassembled with it")
    ap.add_argument("--kinds", help="only these trap kinds, comma-separated (e.g. simd,?)")
    ap.add_argument("--top", type=int, default=160, help="how many encodings to list (default 160)")
    args = ap.parse_args()

    counts = tally(args.source)
    kinds = set(args.kinds.split(",")) if args.kinds else None
    rows = sorted(((c, k, r) for (k, r), c in counts.items() if not kinds or k in kinds),
                  key=lambda t: -t[0])
    total = sum(c for c, _, _ in rows)
    rows = rows[: args.top]

    names = disassemble(rows, args.objdump) if args.objdump else {}
    for i, (count, kind, raw) in enumerate(rows):
        width = "%08X" if raw > 0xFFFF else "%04X"
        line = "%7d %-6s %s" % (count, kind, width % raw)
        if names:
            line += "  " + names.get(i * 4, "")
        print(line)
    print("%d traps in %d encodings%s" % (total, len(counts), " (listed %d)" % len(rows) if len(rows) < len(counts) else ""),
          file=sys.stderr)


if __name__ == "__main__":
    main()
