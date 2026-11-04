#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "core/instance.hpp"

namespace apex {

struct DelaunayResult {
    std::vector<uint32_t> triangles;                    // 3 node ids per triangle (clockwise, y-up)
    std::vector<std::pair<uint32_t, uint32_t>> edges;   // undirected, u < v, deduplicated
};

// Own sweep-hull Delaunay triangulation (Delaunator-style), O(n log n) expected.
// - Orientation is exact (filtered double + expansion arithmetic fallback).
// - Edge flips happen only when the in-circle test is certain, so near-cocircular
//   input yields a valid triangulation that is Delaunay up to round-off.
// - Exact duplicate points are skipped (they get no Delaunay edge; k-NN covers them).
// - All-collinear input: no triangles; edges chain consecutive points along the line.
DelaunayResult delaunay(const Instance& inst);

// Exact sign of (q - p) x (r - p): > 0 counter-clockwise, < 0 clockwise, 0 collinear.
int orient2d_sign(double px, double py, double qx, double qy, double rx, double ry);

}  // namespace apex
