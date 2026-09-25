/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/common/fp/op/FPMulAdd.h"

#include <cstring>
#include <initializer_list>
#include <type_traits>

#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include <mcl/stdint.hpp>

#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/fused.h"
#include "dynarmic/common/fp/info.h"
#include "dynarmic/common/fp/process_exception.h"
#include "dynarmic/common/fp/process_nan.h"
#include "dynarmic/common/fp/unpacked.h"

namespace Dynarmic::FP {

// A binary32 product is exact in binary64 (24 + 24 <= 53 bits).  The
// Boldo--Melquiond approach uses that exact product and a binary64 sum.
// Its only possible binary32 double-rounding ambiguity is at a binary32
// midpoint; those cases use the existing exact integer implementation.
static bool FPMulAdd32Fast(u32 addend, u32 op1, u32 op2, FPCR fpcr,
                           FPSR& fpsr, u32& result) {
#if defined(__SSE2__)
    // The guest rounding mode alone does not guarantee the host MXCSR mode.
    if (fpcr.RMode() != RoundingMode::ToNearest_TieEven || fpcr.FZ() ||
        (_mm_getcsr() & 0x6000) != 0) {
        return false;
    }
#else
    return false;
#endif
    constexpr u32 exp_mask = FPInfo<u32>::exponent_mask;
    for (const u32 bits : {addend, op1, op2}) {
        const u32 exponent = bits & exp_mask;
        if (exponent == 0 || exponent == exp_mask) {
            return false;
        }
    }

    float a, b, c;
    std::memcpy(&a, &addend, sizeof(a));
    std::memcpy(&b, &op1, sizeof(b));
    std::memcpy(&c, &op2, sizeof(c));
    const double product = static_cast<double>(b) * static_cast<double>(c);
    const double sum = product + static_cast<double>(a);
    const float rounded = static_cast<float>(sum);
    u32 rounded_bits;
    std::memcpy(&rounded_bits, &rounded, sizeof(rounded_bits));
    // Exclude cancellation, subnormal/zero results, and the boundary where
    // an exact subnormal can round up to the smallest normal (underflow flag).
    const u32 result_exponent = rounded_bits & exp_mask;
    if (result_exponent <= 0x00800000 || result_exponent == exp_mask) {
        return false;
    }

    const double rounded_double = static_cast<double>(rounded);
    if (sum != rounded_double) {
        const bool toward_positive = sum > rounded_double;
        const bool positive = (rounded_bits & FPInfo<u32>::sign_mask) == 0;
        const u32 neighbor_bits = rounded_bits + (toward_positive == positive ? 1u : -1u);
        float neighbor;
        std::memcpy(&neighbor, &neighbor_bits, sizeof(neighbor));
        const double midpoint = (rounded_double + static_cast<double>(neighbor)) * 0.5;
        if (sum == midpoint) {
            return false;
        }
    }

    // TwoSum's residual is exact: the binary64 product and addend are exact,
    // and their sum cannot overflow or underflow binary64 for binary32 inputs.
    const double a64 = static_cast<double>(a);
    const double z = sum - product;
    const double residual = (product - (sum - z)) + (a64 - z);
    if (sum != rounded_double || residual != 0.0) {
        FPProcessException(FPExc::Inexact, fpcr, fpsr);
    }
    result = rounded_bits;
    return true;
}

template<typename FPT>
FPT FPMulAdd(FPT addend, FPT op1, FPT op2, FPCR fpcr, FPSR& fpsr) {
    if constexpr (std::is_same_v<FPT, u32>) {
        u32 fast_result;
        if (FPMulAdd32Fast(addend, op1, op2, fpcr, fpsr, fast_result)) {
            return fast_result;
        }
    }
    const RoundingMode rounding = fpcr.RMode();

    const auto [typeA, signA, valueA] = FPUnpack(addend, fpcr, fpsr);
    const auto [type1, sign1, value1] = FPUnpack(op1, fpcr, fpsr);
    const auto [type2, sign2, value2] = FPUnpack(op2, fpcr, fpsr);

    const bool infA = typeA == FPType::Infinity;
    const bool inf1 = type1 == FPType::Infinity;
    const bool inf2 = type2 == FPType::Infinity;
    const bool zeroA = typeA == FPType::Zero;
    const bool zero1 = type1 == FPType::Zero;
    const bool zero2 = type2 == FPType::Zero;

    const auto maybe_nan = FPProcessNaNs3<FPT>(typeA, type1, type2, addend, op1, op2, fpcr, fpsr);

    if (typeA == FPType::QNaN && ((inf1 && zero2) || (zero1 && inf2))) {
        FPProcessException(FPExc::InvalidOp, fpcr, fpsr);
        return FPInfo<FPT>::DefaultNaN();
    }

    if (maybe_nan) {
        return *maybe_nan;
    }

    // Calculate properties of product (op1 * op2).
    const bool signP = sign1 != sign2;
    const bool infP = inf1 || inf2;
    const bool zeroP = zero1 || zero2;

    // Raise NaN on (inf * inf) of opposite signs or (inf * zero).
    if ((inf1 && zero2) || (zero1 && inf2) || (infA && infP && signA != signP)) {
        FPProcessException(FPExc::InvalidOp, fpcr, fpsr);
        return FPInfo<FPT>::DefaultNaN();
    }

    // Handle infinities
    if ((infA && !signA) || (infP && !signP)) {
        return FPInfo<FPT>::Infinity(false);
    }
    if ((infA && signA) || (infP && signP)) {
        return FPInfo<FPT>::Infinity(true);
    }

    // Result is exactly zero
    if (zeroA && zeroP && signA == signP) {
        return FPInfo<FPT>::Zero(signA);
    }

    const FPUnpacked result_value = FusedMulAdd(valueA, value1, value2);
    if (result_value.mantissa == 0) {
        return FPInfo<FPT>::Zero(rounding == RoundingMode::TowardsMinusInfinity);
    }
    return FPRound<FPT>(result_value, fpcr, fpsr);
}

template u16 FPMulAdd<u16>(u16 addend, u16 op1, u16 op2, FPCR fpcr, FPSR& fpsr);
template u32 FPMulAdd<u32>(u32 addend, u32 op1, u32 op2, FPCR fpcr, FPSR& fpsr);
template u64 FPMulAdd<u64>(u64 addend, u64 op1, u64 op2, FPCR fpcr, FPSR& fpsr);

template<typename FPT>
FPT FPMulSub(FPT minuend, FPT op1, FPT op2, FPCR fpcr, FPSR& fpsr) {
    return FPMulAdd<FPT>(minuend, (op1 ^ FPInfo<FPT>::sign_mask), op2, fpcr, fpsr);
}

template u16 FPMulSub<u16>(u16 minuend, u16 op1, u16 op2, FPCR fpcr, FPSR& fpsr);
template u32 FPMulSub<u32>(u32 minuend, u32 op1, u32 op2, FPCR fpcr, FPSR& fpsr);
template u64 FPMulSub<u64>(u64 minuend, u64 op1, u64 op2, FPCR fpcr, FPSR& fpsr);

}  // namespace Dynarmic::FP
