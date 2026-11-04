#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"

namespace apex {

// Hilbert curve on a 2^16 x 2^16 grid. Grid coordinates are used for ORDERING
// only; distances never use scaled coordinates (MASTER §2, directive 1).
inline constexpr uint32_t kHilbertBits = 16;
inline constexpr uint32_t kHilbertSide = 1u << kHilbertBits;

uint32_t hilbert_xy2d(uint32_t u, uint32_t v);                  // u, v < 2^16
void hilbert_d2xy(uint32_t d, uint32_t& u, uint32_t& v);

// Maps (x, y) into the grid using a square bounding box of side
// max(max_x - min_x, max_y - min_y) anchored at (min_x, min_y), then returns the key.
uint32_t hilbert_key(double x, double y, double min_x, double min_y, double max_x, double max_y);

// Node indices (0..n-1) sorted by Hilbert key; ties broken by index (deterministic).
std::vector<uint32_t> hilbert_order(const Instance& inst);

}  // namespace apex
