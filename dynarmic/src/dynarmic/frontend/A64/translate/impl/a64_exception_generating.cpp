/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

bool TranslatorVisitor::BRK(Imm<16> /*imm16*/) {
    return RaiseException(Exception::Breakpoint);
}

/* The emulator stands in for this whole guest function.
 *
 * The hook is handed x0..x2 and its result becomes x0, and then this returns
 * the way the function it replaced would have: PC from LR, PopRSBHint, which
 * is the predicted return path a RET takes.  No state is flushed and no halt
 * is checked, so the cost is the call itself — which is the entire reason to
 * have this rather than an SVC. */
bool TranslatorVisitor::HOSTHOOK(Imm<16> id) {
    const IR::U64 result = ir.CallHostHook(id.ZeroExtend<u32>(),
                                           ir.GetX(Reg::R0), ir.GetX(Reg::R1),
                                           ir.GetX(Reg::R2));
    ir.SetX(Reg::R0, result);
    ir.SetPC(ir.GetX(Reg::R30));
    ir.SetTerm(IR::Term::PopRSBHint{});
    return false;
}

bool TranslatorVisitor::SVC(Imm<16> imm16) {
    ir.PushRSB(ir.current_location->AdvancePC(4));
    ir.SetPC(ir.Imm64(ir.current_location->PC() + 4));
    ir.CallSupervisor(imm16.ZeroExtend());
    ir.SetTerm(IR::Term::CheckHalt{IR::Term::PopRSBHint{}});
    return false;
}

}  // namespace Dynarmic::A64
