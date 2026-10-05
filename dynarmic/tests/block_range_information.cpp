/* SPDX-License-Identifier: 0BSD */
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>
#include "dynarmic/backend/block_range_information.h"

TEST_CASE("block invalidation finds every intersecting range", "[cache]") {
    using Interval = boost::icl::discrete_interval<u64>;
    Dynarmic::Backend::BlockRangeInformation<u64> blocks;
    constexpr u64 base = 0x700010010000;
    for (u64 i = 0; i < 8; ++i) {
        const auto start = base + i * 16;
        blocks.AddRange(Interval::closed(start, start + 7),
                        Dynarmic::IR::LocationDescriptor{start});
    }
    auto query = [&](u64 first, u64 last) {
        boost::icl::interval_set<u64> ranges;
        ranges.add(Interval::closed(first, last));
        return blocks.InvalidateRanges(ranges);
    };
    REQUIRE(query(base, base + 4095).size() == 8);
    REQUIRE(query(base + 7, base + 16).size() == 2);
    REQUIRE(query(base + 8, base + 15).empty());
    REQUIRE(query(base + 1, base + 6).size() == 1);
    REQUIRE(query(base - 16, base - 1).empty());
    REQUIRE(query(base + 120, base + 4095).empty());
    blocks.AddRange(Interval::closed(base + 4, base + 20),
                    Dynarmic::IR::LocationDescriptor{base + 4});
    REQUIRE(query(base, base + 4095).size() == 9);
    REQUIRE(query(base + 8, base + 15).size() == 1);
    blocks.ClearCache();
    REQUIRE(query(base, base + 4095).empty());
}

TEST_CASE("block invalidation handles address limits and duplicate descriptors", "[cache]") {
    using Interval = boost::icl::discrete_interval<u64>;
    Dynarmic::Backend::BlockRangeInformation<u64> blocks;
    constexpr u64 maximum = std::numeric_limits<u64>::max();
    blocks.AddRange(Interval::closed(0, 0), Dynarmic::IR::LocationDescriptor{1});
    blocks.AddRange(Interval::closed(maximum - 3, maximum), Dynarmic::IR::LocationDescriptor{2});
    blocks.AddRange(Interval::closed(4, 8), Dynarmic::IR::LocationDescriptor{3});
    blocks.AddRange(Interval::closed(4, 8), Dynarmic::IR::LocationDescriptor{3});
    blocks.AddRange(Interval::closed(5, 9), Dynarmic::IR::LocationDescriptor{3});
    boost::icl::interval_set<u64> ranges;
    ranges.add(Interval::closed(0, maximum));
    REQUIRE(blocks.InvalidateRanges(ranges).size() == 3);
    ranges.clear();
    ranges.add(Interval::closed(maximum, maximum));
    REQUIRE(blocks.InvalidateRanges(ranges).size() == 1);
    Dynarmic::Backend::BlockRangeInformation<u32> blocks32;
    blocks32.AddRange(boost::icl::discrete_interval<u32>::closed(0xfffffff0u, 0xffffffffu),
                      Dynarmic::IR::LocationDescriptor{4});
    boost::icl::interval_set<u32> ranges32;
    ranges32.add(boost::icl::discrete_interval<u32>::closed(0xffffffffu, 0xffffffffu));
    REQUIRE(blocks32.InvalidateRanges(ranges32).size() == 1);
}

TEST_CASE("overlapping block ranges match a linear reference", "[cache]") {
    using Interval = boost::icl::discrete_interval<u64>;
    struct Range { u64 first, last; };
    std::vector<Range> reference;
    Dynarmic::Backend::BlockRangeInformation<u64> blocks;
    u64 random = 0x123456789abcdef;
    auto next = [&] {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        return random;
    };
    // Sorted insertions stress balancing; the second half introduces nesting,
    // equal starts and ranges spanning many disjoint blocks.
    for (u64 i = 0; i < 4096; ++i) {
        const u64 first = i < 2048 ? i * 16 : next() % 32768;
        const u64 last = first + next() % 4096;
        reference.push_back({first, last});
        blocks.AddRange(Interval::closed(first, last), Dynarmic::IR::LocationDescriptor{i});
    }
    for (unsigned i = 0; i < 512; ++i) {
        const u64 first = next() % 40000, last = first + next() % 2048;
        boost::icl::interval_set<u64> ranges;
        ranges.add(Interval::closed(first, last));
        const auto matches = blocks.InvalidateRanges(ranges);
        size_t expected = 0;
        for (size_t j = 0; j < reference.size(); ++j) {
            if (reference[j].first <= last && reference[j].last >= first) {
                REQUIRE(matches.contains(Dynarmic::IR::LocationDescriptor{j}));
                ++expected;
            }
        }
        REQUIRE(matches.size() == expected);
    }
}
