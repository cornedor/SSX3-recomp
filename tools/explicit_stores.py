#!/usr/bin/env python3
"""Rewrite stores through the MEM macros into explicit store macros.

    MEM32(edi + 4) = eax;     ->   ST32(edi + 4, eax);

The lifted code writes guest memory as `MEMn(addr) = value`, which needs MEMn
to be an lvalue. Natively that is fine: device registers are unmapped pages
and a store to one faults into the MMIO handler. WebAssembly has no faults, so
the wasm build checks the address of every device-capable access instead --
and a check can only run in an expression, not in an lvalue. After this pass
every store is an ST macro and MEMn is only ever read, so the wasm header can
make MEMn an rvalue: a store this pass missed is then a compile error, never a
silent unchecked write. Native builds define STn as the plain store.

Runs in place over src/recomp/gen/*.c (scripts/regen.sh calls it after the
lift); idempotent.
"""
import re
import sys
from pathlib import Path

MACRO = re.compile(r"\b(S?MEM(?:8|16|32|64))\(")
WIDTH = {"MEM8": "8", "SMEM8": "8", "MEM16": "16", "SMEM16": "16",
         "MEM32": "32", "SMEM32": "32", "SMEM64": "64"}


def close_paren(s, i):
    """Index of the ')' matching the '(' just before i."""
    depth = 1
    while i < len(s):
        c = s[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced")


def rhs_end(s, i):
    """End of an assignment's right-hand side starting at i: the ';' or ','
    at depth 0, or an unmatched ')'."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            if depth == 0:
                return i
            depth -= 1
        elif c in ";," and depth == 0:
            return i
        i += 1
    raise ValueError("no end of statement")


def rewrite_line(line):
    out = []
    pos = 0
    count = 0
    while True:
        m = MACRO.search(line, pos)
        if not m:
            out.append(line[pos:])
            break
        start, open_end = m.start(), m.end()
        close = close_paren(line, open_end)
        j = close + 1
        while j < len(line) and line[j] == " ":
            j += 1
        if j < len(line) and line[j] == "=" and line[j + 1:j + 2] != "=":
            end = rhs_end(line, j + 1)
            addr = line[open_end:close]
            rhs = line[j + 1:end].strip()
            rhs, n = rewrite_line(rhs)   # never nested in practice; be safe
            out.append(line[pos:start])
            out.append(f"ST{WIDTH[m.group(1)]}({addr}, {rhs})")
            count += 1 + n
            pos = end
        else:
            # a read: keep it, but look for stores inside its address
            out.append(line[pos:open_end])
            pos = open_end
    return "".join(out), count


def main(paths):
    total = 0
    for p in paths:
        p = Path(p)
        src = p.read_text()
        lines = src.split("\n")
        n_file = 0
        for k, line in enumerate(lines):
            if "MEM" not in line or "=" not in line:
                continue
            s = line.lstrip()
            if s.startswith(("#", "//", "/*", "*")):
                continue
            new, n = rewrite_line(line)
            if n:
                lines[k] = new
                n_file += n
        if n_file:
            p.write_text("\n".join(lines))
        total += n_file
    print(f"explicit_stores: {total} stores rewritten in {len(paths)} files")


if __name__ == "__main__":
    main(sys.argv[1:])
