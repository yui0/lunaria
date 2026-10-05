/* SPDX-License-Identifier: 0BSD */
#ifndef DYNARMIC_BLOCK_RANGE_INDEX_H
#define DYNARMIC_BLOCK_RANGE_INDEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dynarmic_block_range_node;
typedef void (*dynarmic_block_range_visit)(uint64_t location, void *context);

/* Closed intervals, ordered by (first, last, location), with subtree maximum
 * endpoints. The caller owns the root and serializes access. */
int dynarmic_block_range_add(struct dynarmic_block_range_node **root,
                            uint64_t first, uint64_t last, uint64_t location);
void dynarmic_block_range_clear(struct dynarmic_block_range_node **root);
void dynarmic_block_range_query(const struct dynarmic_block_range_node *root,
                              uint64_t first, uint64_t last,
                              dynarmic_block_range_visit visit, void *context);

#ifdef __cplusplus
}
#endif
#endif
