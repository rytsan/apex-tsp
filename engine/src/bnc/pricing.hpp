#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "core/instance.hpp"
#include "geo/grid.hpp"

namespace apex {

struct NegEdge {
    double rc;
    uint32_t u, v;
};

// Dual of one active cut row: node set (SEC S / blossom handle) and its dual value.
struct CutDual {
    const std::vector<uint32_t>* nodes;
    double y;
};

enum class PricingMode : uint8_t {
    Grid,        // spatial grid + vector filter (AVX2 when compiled in, else scalar)
    GridScalar,  // spatial grid + scalar filter (parity reference)
    Scan,        // all n(n-1)/2 pairs (reference; also the Farkas mode, which has no radius)
};

struct PricingStats {
    uint64_t calls = 0, pairs_filtered = 0, exact_checks = 0, rcf_skipped = 0;
};

// Exact pricing over the complete graph (MASTER §5.6). For every pair {u, v} not in `exclude`
// (key (u << 32) | v, u < v):
//   rc = c_uv - y_u - y_v - sum_{K : |K ∩ {u,v}| = 1} y_K
// (non-candidate edges are never blossom teeth, so their cut coefficient is +1 when crossing).
// zero_cost: Farkas mode, c_uv = 0. Returns the sum of rc over pairs with rc < -1e-9 and appends
// them to `out` (if given); `count` gets their number.
//
// Grid mode is exact, not a heuristic: with Y_v = y_v + sum_{K ∋ v} max(y_K, 0), every pair
// satisfies rc >= c_uv - Y_u - Y_v and c_uv >= d_uv - 0.5 (EUC_2D; CEIL_2D: c >= d), so a
// negative pair has d_uv < Y_u + Y_v + 0.5. Each u scans the grid disk of radius
// Y_u + max Y + 0.5, a vector filter keeps pairs with d^2 < (Y_u + Y_v + 0.5 + 1e-6)^2, and
// only those get the exact rc.
// Global RCF filter (sound rule with the root bound, MASTER §5.5): a pair is skipped when even a
// lower bound of its root reduced cost, rc >= c_uv - Yroot_u - Yroot_v, gives
// ceil(root_lb + rc - 1e-6) >= ub: every tour through it costs >= UB, so it is fixed to 0
// globally and never needs to enter an LP (also not through Farkas pricing).
struct PricingFilter {
    const std::vector<double>* yroot = nullptr;
    double root_lb = 0;
    int64_t ub = 0;
};

class EdgePricer {
public:
    explicit EdgePricer(const Instance& inst) : inst_(inst), grid_(inst) {}

    double price(const std::vector<double>& ynode, const std::vector<CutDual>& cuts,
                 const std::unordered_set<uint64_t>& exclude, bool zero_cost, PricingMode mode,
                 std::vector<NegEdge>* out, uint64_t& count, const PricingFilter* filter = nullptr);

    const PricingStats& stats() const { return stats_; }

private:
    const Instance& inst_;
    SpatialGrid grid_;
    PricingStats stats_;
};

// Filter kernel (exposed for the parity test): keep[k] = 1 iff R_k > 0 and
// (xu - xs[k])^2 + (yu - ys[k])^2 < R_k^2, with R_k = Yu + Y[k] + 0.5 + 1e-6.
void pricing_filter_scalar(double xu, double yu, double Yu, const double* xs, const double* ys, const double* Y,
                           uint32_t m, uint8_t* keep);
void pricing_filter_simd(double xu, double yu, double Yu, const double* xs, const double* ys, const double* Y,
                         uint32_t m, uint8_t* keep);
bool pricing_simd_available();

}  // namespace apex
