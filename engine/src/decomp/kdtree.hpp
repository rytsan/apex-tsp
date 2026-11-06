#pragma once

// heuristic: spatial decomposition for the scalable mode (UB / candidate statistics only).

#include <cstdint>
#include <vector>

#include "core/instance.hpp"

namespace apex {

struct KdNode {
    int32_t left = -1, right = -1;      // children (-1: leaf)
    std::vector<uint32_t> points;       // leaf: node ids (sorted); internal: empty
    uint8_t axis = 0;                   // split axis (0 = x, 1 = y) of an internal node
    double split = 0;                   // split coordinate
};

// Balanced KD-tree: a node with more than max_leaf points splits on the wider bounding-box axis
// so that its k = ceil(size / max_leaf) future leaves divide floor(k / 2) : ceil(k / 2). Leaves
// end up with size / k points each (> max_leaf * 2 / 3 once k >= 3), e.g. 500-800 for max_leaf
// 800. nodes[0] is the root; children come after their parent. Deterministic (ties by id).
std::vector<KdNode> build_kdtree(const Instance& inst, uint32_t max_leaf);

}  // namespace apex
