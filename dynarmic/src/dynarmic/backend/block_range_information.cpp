/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <new>
#include <boost/icl/interval_set.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) return;
    if (!dynarmic_block_range_add(&block_ranges, boost::icl::first(range),
                                 boost::icl::last(range), location.Value())) {
        throw std::bad_alloc{};
    }
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    dynarmic_block_range_clear(&block_ranges);
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (const auto& range : ranges) {
        dynarmic_block_range_query(block_ranges, boost::icl::first(range),
                                  boost::icl::last(range),
                                  [](uint64_t location, void* context) {
                                      auto& locations = *static_cast<tsl::robin_set<IR::LocationDescriptor>*>(context);
                                      locations.insert(IR::LocationDescriptor{location});
                                  }, &erase_locations);
    }
    // TODO: EFFICIENCY: Remove ranges that are to be erased.
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
