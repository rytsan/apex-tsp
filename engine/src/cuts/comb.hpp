#pragma once

#include <cstdint>
#include <vector>

#include "cuts/blossom.hpp"
#include "cuts/support.hpp"

namespace apex {

// Comb inequality (Chvátal; Grötschel-Padberg): handle H, teeth T_1..T_k with k odd >= 3,
// teeth pairwise disjoint, every tooth meets both H and V \ H:
//   x(delta(H)) + sum_j x(delta(T_j)) >= 3k + 1.
// Valid for every tour for any such sets (no other condition), so a separated comb only needs
// these structural checks plus an exact violation check to be safe to add.
struct CombCut {
    std::vector<uint32_t> handle;              // sorted original nodes
    std::vector<std::vector<uint32_t>> teeth;  // each sorted; teeth sorted
    double violation;                          // (3k + 1) - lhs
};

bool comb_valid(uint32_t n, const std::vector<uint32_t>& handle, const std::vector<std::vector<uint32_t>>& teeth);
double comb_violation(const SupportGraph& g, const std::vector<uint32_t>& handle,
                      const std::vector<std::vector<uint32_t>>& teeth);

struct CombStats {
    uint32_t candidates = 0, invalid = 0, cuts = 0;
    uint32_t shrunk_nodes = 0;
};

// Combs from blossoms of the shrunk support graph: contract x_e = 1 chains (shrink_unit_edges),
// separate blossoms there (Padberg-Rao exact when the shrunk graph is small), expand each
// blossom: handle = union of its supernodes, tooth (A, B) = members(A) + members(B).
// Plain blossoms (every tooth two nodes, handle unchanged) are left to separate_blossom.
// Every cut is checked with comb_valid and re-verified exactly on `g`; sorted by violation.
std::vector<CombCut> separate_comb(const SupportGraph& g, const BlossomConfig& cfg = {}, CombStats* stats = nullptr);

}  // namespace apex
