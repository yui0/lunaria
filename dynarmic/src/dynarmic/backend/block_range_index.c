/* SPDX-License-Identifier: 0BSD */
#include "dynarmic/backend/block_range_index.h"

#include <stdlib.h>

struct dynarmic_block_range_node {
    struct dynarmic_block_range_node *left, *right;
    uint64_t first, last, location, max_last;
    unsigned height;
};

static unsigned height(const struct dynarmic_block_range_node *node) {
    return node ? node->height : 0;
}

static void update(struct dynarmic_block_range_node *node) {
    const unsigned left = height(node->left), right = height(node->right);
    node->height = 1 + (left > right ? left : right);
    node->max_last = node->last;
    if (node->left && node->left->max_last > node->max_last)
        node->max_last = node->left->max_last;
    if (node->right && node->right->max_last > node->max_last)
        node->max_last = node->right->max_last;
}

static struct dynarmic_block_range_node *rotate_left(struct dynarmic_block_range_node *node) {
    struct dynarmic_block_range_node *next = node->right;
    node->right = next->left;
    next->left = node;
    update(node);
    update(next);
    return next;
}

static struct dynarmic_block_range_node *rotate_right(struct dynarmic_block_range_node *node) {
    struct dynarmic_block_range_node *next = node->left;
    node->left = next->right;
    next->right = node;
    update(node);
    update(next);
    return next;
}

static int compare(uint64_t first, uint64_t last, uint64_t location,
                   const struct dynarmic_block_range_node *node) {
    if (first != node->first) return first < node->first ? -1 : 1;
    if (last != node->last) return last < node->last ? -1 : 1;
    if (location != node->location) return location < node->location ? -1 : 1;
    return 0;
}

static struct dynarmic_block_range_node *insert(struct dynarmic_block_range_node *node,
                                               uint64_t first, uint64_t last,
                                               uint64_t location, int *success) {
    if (!node) {
        node = malloc(sizeof *node);
        if (!node) {
            *success = 0;
            return NULL;
        }
        *node = (struct dynarmic_block_range_node){
            .first = first, .last = last, .location = location,
            .max_last = last, .height = 1,
        };
        return node;
    }
    const int order = compare(first, last, location, node);
    if (order < 0)
        node->left = insert(node->left, first, last, location, success);
    else if (order > 0)
        node->right = insert(node->right, first, last, location, success);
    else
        return node;
    update(node);
    if (height(node->left) > height(node->right) + 1) {
        if (height(node->left->right) > height(node->left->left))
            node->left = rotate_left(node->left);
        return rotate_right(node);
    }
    if (height(node->right) > height(node->left) + 1) {
        if (height(node->right->left) > height(node->right->right))
            node->right = rotate_right(node->right);
        return rotate_left(node);
    }
    return node;
}

int dynarmic_block_range_add(struct dynarmic_block_range_node **root,
                            uint64_t first, uint64_t last, uint64_t location) {
    int success = 1;
    if (first <= last)
        *root = insert(*root, first, last, location, &success);
    return success;
}

void dynarmic_block_range_clear(struct dynarmic_block_range_node **root) {
    struct dynarmic_block_range_node *node = *root;
    if (!node) return;
    dynarmic_block_range_clear(&node->left);
    dynarmic_block_range_clear(&node->right);
    free(node);
    *root = NULL;
}

void dynarmic_block_range_query(const struct dynarmic_block_range_node *node,
                               uint64_t first, uint64_t last,
                               dynarmic_block_range_visit visit, void *context) {
    if (!node || first > last || node->max_last < first) return;
    dynarmic_block_range_query(node->left, first, last, visit, context);
    if (node->first > last) return;
    if (node->last >= first) visit(node->location, context);
    dynarmic_block_range_query(node->right, first, last, visit, context);
}
