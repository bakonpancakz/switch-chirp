// The ratchet tree's shape (RFC 9420 appendix C): its nodes in one array,
// leaves at even indices, parents at odd ones; an n-leaf tree has 2n - 1
// nodes and n is always a power of two (MLS keeps the tree full).
#pragma once
#include <stdint.h>

namespace mlstree {

// How many 1 bits x ends in: 0 for a leaf
inline uint32_t level(uint32_t x) {
    uint32_t k = 0;
    while ((x >> k) & 1) k++;
    return k;
}

inline uint32_t nodeWidth(uint32_t leaves) { return leaves ? 2 * (leaves - 1) + 1 : 0; }

inline uint32_t root(uint32_t leaves) {
    uint32_t w = nodeWidth(leaves), k = 0;
    while ((1u << (k + 1)) <= w) k++;
    return (1u << k) - 1;
}

// (only for parents: level(x) > 0)
inline uint32_t left(uint32_t x) { return x ^ (1u << (level(x) - 1)); }
inline uint32_t right(uint32_t x) { return x ^ (3u << (level(x) - 1)); }

// (not for the root)
inline uint32_t parent(uint32_t x) {
    uint32_t k = level(x), b = (x >> (k + 1)) & 1;
    return (x | (1u << k)) ^ (b << (k + 1));
}

inline uint32_t sibling(uint32_t x) {
    uint32_t p = parent(x);
    return x < p ? right(p) : left(p);
}

inline uint32_t leafNode(uint32_t leaf) { return leaf * 2; }

}  // namespace mlstree
