#!/usr/bin/env python3
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Patch arm_exec.cpp to use svc/ C layer."""
from pathlib import Path

P = Path(__file__).resolve().parents[1] / "src" / "arm_exec.cpp"
text = P.read_text(encoding="utf-8")

# 1. Add svc includes after jni.h
needle = '#include "jvm/jni.h"\n}'
if needle not in text:
    raise SystemExit("include anchor not found")
text = text.replace(
    '#include "jvm/jni.h"\n}',
    '#include "jvm/jni.h"\n\n#include "svc/svc_nums.h"\n#include "svc/svc_symbol.h"\n#include "svc/svc_bridge.h"\n}',
    1,
)

# 2. Remove SVC constant block (JNI_VTABLE_COUNT .. SVC_TRAMP_TOTAL / JVM_SLOT_COUNT)
start = text.find("static constexpr uint32_t JNI_VTABLE_COUNT = 229u;")
end = text.find("static const std::pair<const char *, uint32_t> kSymbolSvcMap[] = {")
if start < 0 or end < 0:
    raise SystemExit("svc block markers not found")
text = text[:start] + text[end:]

# 3. Remove kSymbolSvcMap array
mstart = text.find("static const std::pair<const char *, uint32_t> kSymbolSvcMap[] = {")
mend = text.find("/* ---- Math passthrough tables", mstart)
if mstart < 0 or mend < 0:
    raise SystemExit("kSymbolSvcMap markers not found")
text = text[:mstart] + text[mend:]

# 4. Remove math tables through kMathD2 closing };
tstart = text.find("/* ---- Math passthrough tables")
tend = text.find("/* Guest-side ARM stub for pthread_once", tstart)
if tstart < 0 or tend < 0:
    raise SystemExit("math table markers not found")
text = text[:tstart] + text[tend:]

# 5. Replace lookup_symbol_svc
old_lookup = """static uint32_t lookup_symbol_svc(const char *name) {
    for (auto &[n, s] : kSymbolSvcMap)
        if (strcmp(n, name) == 0) return s;
    for (uint32_t i = 0; i < SVC_MATH_F1_COUNT; ++i)
        if (strcmp(kMathF1[i].first, name) == 0) return SVC_MATH_F1_BASE + i;
    for (uint32_t i = 0; i < SVC_MATH_F2_COUNT; ++i)
        if (strcmp(kMathF2[i].first, name) == 0) return SVC_MATH_F2_BASE + i;
    for (uint32_t i = 0; i < SVC_MATH_D1_COUNT; ++i)
        if (strcmp(kMathD1[i].first, name) == 0) return SVC_MATH_D1_BASE + i;
    for (uint32_t i = 0; i < SVC_MATH_D2_COUNT; ++i)
        if (strcmp(kMathD2[i].first, name) == 0) return SVC_MATH_D2_BASE + i;
    return UINT32_MAX;
}"""
new_lookup = """static uint32_t lookup_symbol_svc(const char *name) {
    return svc_symbol_lookup(name);
}"""
if old_lookup not in text:
    raise SystemExit("lookup_symbol_svc block not found")
text = text.replace(old_lookup, new_lookup, 1)

# 6. Add g_svc bridge after g_ctx
anchor = "static ArmExecCtx *g_ctx = nullptr;"
insert = """static ArmExecCtx *g_ctx = nullptr;
static svc_arm_bridge g_svc;

static uint32_t svc_mem_read32(void *ctx, uint64_t va) {
    return static_cast<ArmMemory *>(ctx)->read32((uint32_t)va);
}
static void svc_mem_write32(void *ctx, uint64_t va, uint32_t val) {
    static_cast<ArmMemory *>(ctx)->write32((uint32_t)va, val);
}
static uint8_t *svc_mem_ptr(void *ctx, uint64_t va) {
    return static_cast<ArmMemory *>(ctx)->ptr((uint32_t)va);
}
static const char *svc_mem_cstr(void *ctx, uint64_t va) {
    return static_cast<ArmMemory *>(ctx)->cstr((uint32_t)va);
}
static void svc_request_yield(void *) { g_yield_requested = true; }"""
if anchor not in text:
    raise SystemExit("g_ctx anchor not found")
text = text.replace(anchor, insert, 1)

# 7. Replace heap allocator functions with thin wrappers
heap_start = text.find("/* -------------------------------------------------------------------------\n * Guest heap")
heap_end = text.find("static ArmExecCtx *g_ctx = nullptr;")
if heap_start < 0 or heap_end < 0:
    raise SystemExit("heap block markers not found")
heap_repl = """/* Guest heap — implementation in svc/guest_heap.c */
static uint32_t arm_malloc(ArmExecCtx &ctx, uint32_t size) {
    (void)ctx;
    return svc_arm_heap_malloc(&g_svc, size);
}
static void arm_free(ArmExecCtx &ctx, uint32_t va) {
    (void)ctx;
    svc_arm_heap_free(&g_svc, va);
}
static uint32_t arm_realloc(ArmExecCtx &ctx, uint32_t va, uint32_t newsize) {
    (void)ctx;
    return svc_arm_heap_realloc(&g_svc, va, newsize);
}

"""
text = text[:heap_start] + heap_repl + text[heap_end:]

# 8. Init bridge in arm_exec_context_init
old_init = """    g_ctx->mem.map(BRK_BASE,   BRK_END - BRK_BASE);
    return 0;
}"""
new_init = """    g_ctx->mem.map(BRK_BASE,   BRK_END - BRK_BASE);
    svc_arm_bridge_init(&g_svc, svc_mem_read32, svc_mem_write32, svc_mem_ptr, svc_mem_cstr,
                        &g_ctx->mem, HEAP_BASE, svc_request_yield, nullptr);
    return 0;
}"""
if old_init not in text:
    raise SystemExit("context_init tail not found")
text = text.replace(old_init, new_init, 1)

# 9. dispatch_svc: try C layer early
dispatch_anchor = """    auto ret32 = [&](uint32_t v)  { regs[0] = v; };
    auto ret64 = [&](uint64_t v)  { regs[0]=(uint32_t)v; regs[1]=(uint32_t)(v>>32); };

    /* inline-detour logging"""
dispatch_insert = """    auto ret32 = [&](uint32_t v)  { regs[0] = v; };
    auto ret64 = [&](uint64_t v)  { regs[0]=(uint32_t)v; regs[1]=(uint32_t)(v>>32); };

    if ((svc_no >= SVC_LOG_WRITE && svc_no <= SVC_STRNCAT) ||
        (svc_no >= SVC_MATH_F1_BASE && svc_no < SVC_MATH_D2_BASE + SVC_MATH_D2_COUNT)) {
        int dr = svc_arm_dispatch(&g_svc, svc_no, regs.data());
        if (dr) {
            if (dr == 2) g_yield_requested = true;
            return;
        }
    }

    /* inline-detour logging"""
if dispatch_anchor not in text:
    raise SystemExit("dispatch_svc anchor not found")
text = text.replace(dispatch_anchor, dispatch_insert, 1)

# 10. Remove duplicate case blocks (LOG_WRITE through STRNCAT, ABORT) and math default
cases = [
    "    case SVC_LOG_WRITE:",
    "    case SVC_MALLOC:",
    "    case SVC_FREE:",
    "    case SVC_CALLOC:",
    "    case SVC_REALLOC:",
    "    case SVC_MEMCPY:",
    "    case SVC_MEMSET:",
    "    case SVC_STRLEN:",
    "    case SVC_STRCPY:",
    "    case SVC_STRNCPY:",
    "    case SVC_STRCMP:",
    "    case SVC_STRNCMP:",
    "    case SVC_STRDUP:",
    "    case SVC_STRNDUP:",
    "    case SVC_STRCAT:",
    "    case SVC_STRNCAT:",
    "    case SVC_ABORT:",
]
# Remove from first case SVC_LOG_WRITE to just before case SVC_PTHREAD_KEY
rm_s = text.find("    case SVC_LOG_WRITE: {")
rm_e = text.find("    case SVC_PTHREAD_KEY", rm_s)
if rm_s < 0 or rm_e < 0:
    raise SystemExit("case removal markers not found")
text = text[:rm_s] + text[rm_e:]

# Remove math block in default
math_s = text.find("        /* ---- math passthrough blocks (softfp ABI) ---- */")
math_e = text.find("        fprintf(stderr, \"[arm_exec] unhandled SVC #%u (pc=0x%08x)\\n\",", math_s)
if math_s > 0 and math_e > math_s:
    text = text[:math_s] + text[math_e:]

# 11. arm_exec_heap_used
text = text.replace(
    "uint32_t arm_exec_heap_used(void) {\n    return g_ctx ? g_ctx->heap_ptr - HEAP_BASE : 0;\n}",
    "uint32_t arm_exec_heap_used(void) {\n    return g_ctx ? svc_arm_heap_used(&g_svc) : 0;\n}",
    1,
)

P.write_text(text, encoding="utf-8")
print("Patched", P)
