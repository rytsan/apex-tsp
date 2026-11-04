#pragma once

#include <cstdint>
#include <vector>

#include "cuts/support.hpp"

namespace apex {

// TSP blossom (2-matching) inequality, operator-approved correction of MASTER §6:
//   handle H, odd tooth set T ⊆ delta(H):
//   x(E(H)) + x(T) <= |H| + (|T|-1)/2   <=>(degree 2)   x(delta(H) \ T) - x(T) >= 1 - |T|.
// Valid for every tour: x(delta(H)) of an integral 2-matching is even, so the left side
// x(delta(H) \ T) + sum_T (1 - x_e) cannot be 0 when |T| is odd.
struct BlossomTooth {
    uint32_t u, v;     // endpoints (u < v), exactly one in H
    uint32_t lp_col;   // LP column of the edge
};

struct BlossomCut {
    std::vector<uint32_t> handle;       // sorted original nodes
    std::vector<BlossomTooth> teeth;    // |teeth| odd, sorted by (u, v)
    double violation;                   // (1 - |T|) - (x(delta(H) \ T) - x(T))
};

// Best odd tooth set for a fixed handle (exact for that handle): take every crossing support
// edge with x_e > 1/2, fix parity by the cheapest flip. Returns violation (may be <= 0).
double best_teeth(const SupportGraph& g, const std::vector<uint32_t>& handle, std::vector<BlossomTooth>& teeth);

// Exact re-verification of a cut against the support graph values.
double blossom_violation(const SupportGraph& g, const std::vector<uint32_t>& handle,
                         const std::vector<BlossomTooth>& teeth);

enum class BlossomMode { Heuristic, Full };

struct BlossomConfig {
    BlossomMode mode = BlossomMode::Heuristic;  // --sep-blossom=full enables Padberg-Rao
    double violation_tol = 1e-5;
};

struct BlossomStats {
    uint32_t heuristic_cuts = 0, exact_cuts = 0, candidates = 0;
    bool heuristic = true;  // telemetry marker: the default routine may miss violated cuts
};

// heuristic: handles = connected components of the fractional support for several thresholds
// (Padberg-Hong / Groetschel-Holland), teeth by best_teeth.
// full: additionally Padberg-Rao exact separation (minimum odd cut on the edge-split auxiliary
// graph via a Gusfield Gomory-Hu tree).
// Every emitted cut is re-verified exactly; duplicates removed; sorted by violation.
std::vector<BlossomCut> separate_blossom(const SupportGraph& g, const BlossomConfig& cfg = {},
                                         BlossomStats* stats = nullptr);

}  // namespace apex
