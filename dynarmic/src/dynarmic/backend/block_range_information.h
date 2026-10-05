/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <boost/icl/interval_set.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/backend/block_range_index.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    BlockRangeInformation() = default;
    ~BlockRangeInformation() { ClearCache(); }
    BlockRangeInformation(const BlockRangeInformation&) = delete;
    BlockRangeInformation& operator=(const BlockRangeInformation&) = delete;
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);

private:
    dynarmic_block_range_node* block_ranges = nullptr;
};

}  // namespace Dynarmic::Backend
