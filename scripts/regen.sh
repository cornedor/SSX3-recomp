#!/usr/bin/env bash
# Regenerate everything derived from the XBE: analysis, the per-function asm
# split (decomp/) and the lifted C (src/recomp/gen/). Neither output is in git
# -- both are the game's code -- so run this after cloning and whenever an
# override is added to src/ssx3_overrides.c (it excludes those bodies).
#
#   scripts/regen.sh                 # all steps (~2 min)
#   scripts/regen.sh lift            # only the C lift (after adding an override)
#
# XBE defaults to ../SSX3/default.xbe, else disk/SSX3/default.xbe; set XBE=... otherwise. The analysis
# JSON must sit beside the XBE, so the parse step writes there.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TK="${TK:-$(dirname "$HERE")/xboxrecomp}"
PY="${PY:-$TK/.venv/bin/python}"
if [ -z "${XBE:-}" ]; then
    XBE="$(dirname "$HERE")/SSX3/default.xbe"
    [ -f "$XBE" ] || XBE="$HERE/disk/SSX3/default.xbe"
fi
XBE="$(realpath "$XBE")"
ANALYSIS="$(dirname "$XBE")/default_analysis.json"
# DOLBY is DSP56300 microcode for the audio chip, not x86: never disassemble it.
SECTIONS=(--text-only --extra-sections D3D,D3DX,XGRPH,DSOUND,XPP)
# Entry points only reached through function pointers, measured at runtime
# ([ICALL] Failed to resolve VA ...). Both disasm and the split need them.
SEEDS=(--seed-functions "$HERE/analysis/icall_seeds.json")

cd "$TK"
if [ "${1:-all}" = all ]; then
    "$PY" -m tools.xbe_parser "$XBE" --json "$ANALYSIS" > /dev/null
    "$PY" -m tools.disasm "$XBE" "${SECTIONS[@]}" "${SEEDS[@]}"
    "$PY" -m tools.func_id "$XBE"
    "$PY" -m tools.abi_analysis "$XBE"
    # 143 FidDb names from Ghidra 12.1.4 (analysis/ghidra_names.json), applied
    # without needing Ghidra. tools.disasm rewrites functions.json, so this
    # must follow it every time.
    "$PY" tools/ghidra_naming/merge_names.py --names-json "$HERE/analysis/ghidra_names.json" --apply
    # tools.split ignores names in functions.json; split_named.py does not.
    "$PY" "$HERE/tools/split_named.py" --xboxrecomp "$TK" "$XBE" --out "$HERE/decomp" "${SECTIONS[@]}" "${SEEDS[@]}"
fi
"$PY" -m tools.recomp "$XBE" --all --split 250 --game-name "SSX 3" \
    --gen-dir "$HERE/src/recomp/gen" \
    --exclude-manual "$HERE/src/ssx3_overrides.c"
# Stores through MEMn become explicit STn(addr, value): the wasm build checks
# device addresses on every access and needs MEMn to be an rvalue (see the
# script). Same meaning natively.
python3 "$HERE/tools/explicit_stores.py" "$HERE"/src/recomp/gen/*.c
