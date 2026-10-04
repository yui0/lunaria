#!/usr/bin/env python3
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Patch src/svc.h and src/svc.c generated regions from arm_exec.cpp."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARM_EXEC = ROOT / "src" / "arm_exec.cpp"
SVC_H = ROOT / "src" / "svc.h"
SVC_C = ROOT / "src" / "svc.c"
MARK_H_BEGIN = "/* @svc-gen-h@ */"
MARK_H_END = "/* @/svc-gen-h@ */"
MARK_C_BEGIN = "/* @svc-gen-c@ */"
MARK_C_END = "/* @/svc-gen-c@ */"


def patch_marked(path: Path, begin: str, end: str, body: str) -> None:
    text = path.read_text(encoding="utf-8")
    pat = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
    repl = begin + "\n" + body.rstrip() + "\n" + end
    if not pat.search(text):
        raise SystemExit(f"markers not found in {path}")
    path.write_text(pat.sub(repl, text, count=1), encoding="utf-8")


def parse_svc_constants(text: str) -> dict[str, str]:
    consts: dict[str, str] = {}
    for m in re.finditer(
        r"static\s+constexpr\s+uint32_t\s+(\w+)\s*=\s*([^;]+);", text
    ):
        consts[m.group(1)] = m.group(2).strip()
    return consts


def eval_const(name: str, consts: dict[str, str], cache: dict[str, int]) -> int:
    if name in cache:
        return cache[name]
    expr = consts[name]
    expr = re.sub(r"(\d+)u", r"\1", expr)

    def repl(tok: str) -> str:
        if tok.isdigit():
            return tok
        if tok in cache:
            return str(cache[tok])
        if tok in consts:
            return str(eval_const(tok, consts, cache))
        raise KeyError(tok)

    parts = re.split(r"(\W+)", expr)
    out = []
    for p in parts:
        if not p or p.isspace():
            continue
        if re.fullmatch(r"\d+", p):
            out.append(p)
        elif re.fullmatch(r"\w+", p):
            out.append(repl(p))
        else:
            out.append(p)
    val = eval("".join(out), {"__builtins__": {}})
    cache[name] = int(val)
    return cache[name]


def emit_gen_h(consts: dict[str, str]) -> str:
    cache: dict[str, int] = {}
    lines = ["/* Auto-generated from arm_exec.cpp — do not edit by hand. */"]
    order = [k for k in consts if k.startswith("SVC_") or k in ("NUM_DETOURS", "JNI_VTABLE_COUNT")]
    order += [k for k in ("JVM_SLOT_RESERVED0", "JVM_SLOT_RESERVED1", "JVM_SLOT_RESERVED2",
                           "JVM_SLOT_DESTROY", "JVM_SLOT_ATTACH", "JVM_SLOT_DETACH",
                           "JVM_SLOT_GETENV", "JVM_SLOT_ATTACH_DA", "JVM_SLOT_COUNT",
                           "TRAMP_STRIDE", "SVC_TRAMP_TOTAL") if k in consts]
    seen = set()
    for name in order:
        if name in seen or name not in consts:
            continue
        seen.add(name)
        val = eval_const(name, consts, cache)
        lines.append(f"#define {name} {val}u")
    return "\n".join(lines)


def extract_symbol_map(text: str) -> list[tuple[str, str]]:
    start = text.find("static const std::pair<const char *, uint32_t> kSymbolSvcMap[] = {")
    if start < 0:
        raise SystemExit("kSymbolSvcMap not found")
    end = text.find("};", start)
    block = text[start:end]
    entries = []
    for m in re.finditer(r'\{"([^"]+)",\s*(\w+)\}', block):
        entries.append((m.group(1), m.group(2)))
    return entries


def extract_math_table(text: str, var: str) -> list[tuple[str, str]]:
    pat = rf"static const std::pair<const char \*, math_\w+> {var}\[.*?\] = \{{(.*?)\}};"
    m = re.search(pat, text, re.S)
    if not m:
        return []
    out = []
    for e in re.finditer(r'\{"([^"]+)",\s*(\w+)\}', m.group(1)):
        out.append((e.group(1), e.group(2)))
    return out


def emit_gen_c(text: str, entries: list[tuple[str, str]]) -> str:
    lines = ["/* Auto-generated from arm_exec.cpp — do not edit by hand. */", ""]
    lines.append("const struct svc_symbol_entry svc_symbol_map[] = {")
    for sym, svc in entries:
        lines.append(f'    {{"{sym}", {svc}}},')
    lines.append("};")
    lines.append("")
    lines.append(f"const unsigned svc_symbol_map_count = {len(entries)}u;")
    lines.append("")

    tables = [
        ("svc_math_f1", "kMathF1", "SVC_MATH_F1_COUNT", "svc_math_f1_entry"),
        ("svc_math_f2", "kMathF2", "SVC_MATH_F2_COUNT", "svc_math_f2_entry"),
        ("svc_math_d1", "kMathD1", "SVC_MATH_D1_COUNT", "svc_math_d1_entry"),
        ("svc_math_d2", "kMathD2", "SVC_MATH_D2_COUNT", "svc_math_d2_entry"),
    ]
    for arr, src, cnt, typ in tables:
        math_entries = extract_math_table(text, src)
        lines.append(f"const struct {typ} {arr}[] = {{")
        for sym, fn in math_entries:
            lines.append(f'    {{"{sym}", {fn}}},')
        lines.append("};")
        lines.append(f"const unsigned {arr}_count = {cnt};")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ARM_EXEC
    text = src.read_text(encoding="utf-8")
    consts = parse_svc_constants(text)
    entries = extract_symbol_map(text)
    patch_marked(SVC_H, MARK_H_BEGIN, MARK_H_END, emit_gen_h(consts))
    patch_marked(SVC_C, MARK_C_BEGIN, MARK_C_END, emit_gen_c(text, entries))
    n_svc = len([k for k in consts if k.startswith("SVC_")])
    print(f"Patched {SVC_H} ({n_svc} constants)")
    print(f"Patched {SVC_C} ({len(entries)} symbols + math tables)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
