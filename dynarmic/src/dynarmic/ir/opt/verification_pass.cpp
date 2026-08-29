/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstdio>
#include <map>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"
#include "dynarmic/ir/type.h"

namespace Dynarmic::Optimization {

/* The pass exists only to fire an assert.  A release build compiles those out
 * (NDEBUG), so every block still paid for a full walk of its microinstructions
 * and a std::map node per value — with no way left to report anything.  That
 * is not free at this rate: a UE title compiles 600k blocks while it starts. */
#if defined(NDEBUG) || defined(MCL_IGNORE_ASSERTS)
void VerificationPass(const IR::Block&) {}
#else
void VerificationPass(const IR::Block& block) {
    for (const auto& inst : block) {
        for (size_t i = 0; i < inst.NumArgs(); i++) {
            const IR::Type t1 = inst.GetArg(i).GetType();
            const IR::Type t2 = IR::GetArgTypeOf(inst.GetOpcode(), i);
            if (!IR::AreTypesCompatible(t1, t2)) {
                std::puts(IR::DumpBlock(block).c_str());
                ASSERT_FALSE("above block failed validation");
            }
        }
    }

    std::map<IR::Inst*, size_t> actual_uses;
    for (const auto& inst : block) {
        for (size_t i = 0; i < inst.NumArgs(); i++) {
            const auto arg = inst.GetArg(i);
            if (!arg.IsImmediate()) {
                actual_uses[arg.GetInst()]++;
            }
        }
    }

    for (const auto& pair : actual_uses) {
        ASSERT(pair.first->UseCount() == pair.second);
    }
}
#endif

}  // namespace Dynarmic::Optimization
