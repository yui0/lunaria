/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include <mcl/assert.hpp>
#include <mcl/bit_cast.hpp>
#include <mcl/scope_exit.hpp>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/common/x64_disassemble.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::A64 {

/* JIT compilation counters.  Process-wide on purpose: an engine pool has one
 * Jit per host thread and the question is what the process spends on
 * translation, not what one engine does.
 *
 * Count and wall time are always accumulated (cheap atomics) so the emulator
 * can put a progress card on screen while a cold start spends a minute in the
 * emitter.  The translate/opt/emit breakdown stays behind LUNARIA_JIT_STATS. */
static std::atomic<uint64_t> g_compiles{0};
static std::atomic<uint64_t> g_compile_ns{0};
static std::atomic<uint64_t> g_cache_evacuations{0};
static std::atomic<uint64_t> g_translate_ns{0};
static std::atomic<uint64_t> g_opt_ns{0};
static std::atomic<uint64_t> g_emit_ns{0};
static std::atomic<uint64_t> g_ir_insts{0};

using ProgressHook = void (*)(uint64_t compiles, uint64_t compile_ns);
static std::atomic<ProgressHook> g_progress_hook{nullptr};
static std::atomic<uint64_t> g_hook_last_ns{0};
static std::atomic<uint64_t> g_hook_last_compiles{0};


using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        std::make_unique<ArgCallback>(LookupBlock, reinterpret_cast<u64>(arg)),
        std::make_unique<ArgCallback>(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        std::make_unique<ArgCallback>(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf)
            : conf(conf)
            , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
            , emitter(block_of_code, conf, jit)
            , polyfill_options(GenPolyfillOptions(block_of_code)) {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    }

    ~Impl() = default;

    HaltReason Run() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));

        is_executing = true;
        SCOPE_EXIT {
            this->is_executing = false;
        };

        // TODO: Check code alignment

        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSBPtrMask;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return reinterpret_cast<CodePtr>(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }

            return GetCurrentBlock();
        }();

        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);

        PerformRequestedCacheInvalidation(hr);

        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));

        is_executing = true;
        SCOPE_EXIT {
            this->is_executing = false;
        };

        const HaltReason hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());

        PerformRequestedCacheInvalidation(hr);

        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    void DumpDisassembly() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        Common::DumpDisassembledX64(block_of_code.GetCodeBegin(), size);
    }

    std::vector<std::string> Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(block_of_code.GetCodeBegin(), size);
    }

private:
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    CodePtr GetBlock(IR::LocationDescriptor current_location) {
        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        /* Compiling is invisible from outside the JIT, and a run that spends
         * its first minute in the emitter looks exactly like a run whose guest
         * simply has a lot to do.  Always count compiles and wall time; the
         * phase breakdown and the 20k-line log stay behind LUNARIA_JIT_STATS.
         * A registered progress hook (throttled) lets the emulator paint a
         * status card while this thread is stuck here. */
        static const bool stats = [] {
            const char *e = std::getenv("LUNARIA_JIT_STATS");
            return e && *e && *e != '0';
        }();
        struct CompileTimer {
            bool detail;
            std::chrono::steady_clock::time_point t0;
            explicit CompileTimer(bool d)
                : detail(d), t0(std::chrono::steady_clock::now()) {}
            ~CompileTimer() {
                const auto dt = std::chrono::steady_clock::now() - t0;
                const uint64_t ns = (uint64_t)std::chrono::duration_cast<
                    std::chrono::nanoseconds>(dt).count();
                const uint64_t total_ns = g_compile_ns.fetch_add(ns) + ns;
                const uint64_t n = g_compiles.fetch_add(1) + 1;
                if (detail && n % 20000 == 0)
                    std::fprintf(stderr,
                                 "[jit] %llu compiles, %.1f s compiling "
                                 "(%.1f us each: translate %.1f, opt %.1f, "
                                 "emit %.1f), %.1f IR insts each, "
                                 "%llu cache evacuations\n",
                                 (unsigned long long)n,
                                 (double)total_ns / 1e9,
                                 (double)total_ns / 1e3 / (double)n,
                                 (double)g_translate_ns / 1e3 / (double)n,
                                 (double)g_opt_ns / 1e3 / (double)n,
                                 (double)g_emit_ns / 1e3 / (double)n,
                                 (double)g_ir_insts / (double)n,
                                 (unsigned long long)g_cache_evacuations);
                ProgressHook hook = g_progress_hook.load(std::memory_order_relaxed);
                if (!hook) return;
                /* ~60 Hz or every 64 blocks.  The hook is what draws the
                 * boot card while the guest is compiling and issuing no
                 * syscalls, so its rate *is* that card's frame rate: at 10 Hz
                 * the falling snow advanced in visible jumps.  Two clock
                 * reads and a compare per compiled block is nothing beside
                 * the emitter. */
                const uint64_t now_ns = (uint64_t)std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                uint64_t last_ns = g_hook_last_ns.load(std::memory_order_relaxed);
                uint64_t last_n = g_hook_last_compiles.load(std::memory_order_relaxed);
                if (n - last_n < 64 && now_ns - last_ns < 16'000'000ull)
                    return;
                if (!g_hook_last_ns.compare_exchange_strong(
                        last_ns, now_ns, std::memory_order_relaxed))
                    return;
                g_hook_last_compiles.store(n, std::memory_order_relaxed);
                hook(n, total_ns);
            }
        } compile_timer{stats};

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
            ++g_cache_evacuations;
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // JIT Compile
        const auto phase_now = [] { return std::chrono::steady_clock::now(); };
        const auto phase_t0 = phase_now();
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        IR::Block ir_block = A64::Translate(A64::LocationDescriptor{current_location}, get_code,
                                            {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
        const auto phase_t1 = phase_now();
        Optimization::PolyfillPass(ir_block, polyfill_options);
        Optimization::A64CallbackConfigPass(ir_block, conf);
        Optimization::NamingPass(ir_block);
        if (conf.HasOptimization(OptimizationFlag::GetSetElimination) && !conf.check_halt_on_memory_access) {
            Optimization::A64GetSetElimination(ir_block);
            Optimization::DeadCodeElimination(ir_block);
        }
        if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
            Optimization::ConstantPropagation(ir_block);
            Optimization::DeadCodeElimination(ir_block);
        }
        if (conf.HasOptimization(OptimizationFlag::MiscIROpt)) {
            Optimization::A64MergeInterpretBlocksPass(ir_block, conf.callbacks);
        }
        Optimization::VerificationPass(ir_block);
        const auto phase_t2 = phase_now();
        const auto entry = emitter.Emit(ir_block).entrypoint;
        if (stats) {
            const auto ns = [](auto a, auto b) {
                return (uint64_t)std::chrono::duration_cast<
                    std::chrono::nanoseconds>(b - a).count();
            };
            g_translate_ns += ns(phase_t0, phase_t1);
            g_opt_ns += ns(phase_t1, phase_t2);
            g_emit_ns += ns(phase_t2, phase_now());
            g_ir_insts += ir_block.size();
        }
        return entry;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
                block_of_code.ClearCache();
                emitter.ClearCache();
            } else {
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    bool is_executing = false;

    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;

    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

void Jit::DumpDisassembly() const {
    return impl->DumpDisassembly();
}

std::vector<std::string> Jit::Disassemble() const {
    return impl->Disassemble();
}

/* C ABI for the emulator's progress UI.  `extern "C"` inside the namespace
 * still exports the unmangled names; keeping the bodies here lets them read
 * the file-local atomics without widening their linkage. */
extern "C" {

uint64_t dynarmic_a64_compile_count(void) {
    return g_compiles.load(std::memory_order_relaxed);
}

uint64_t dynarmic_a64_compile_ns(void) {
    return g_compile_ns.load(std::memory_order_relaxed);
}

void dynarmic_a64_set_progress_hook(void (*fn)(uint64_t, uint64_t)) {
    g_progress_hook.store(fn, std::memory_order_relaxed);
}

}

}  // namespace Dynarmic::A64

