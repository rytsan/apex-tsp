#pragma once

#include <cstdint>
#include <vector>

#include "cuts/support.hpp"

namespace apex {

// Domino-parity inequality (Letchford 2000; Cook, Espinoza, Goycoolea 2007), in the form used here:
// dominoes {A_j, B_j}, j = 1..k, k odd (A_j, B_j disjoint and nonempty, T_j = A_j + B_j != V),
// any node set S, and F = delta(S) xor E(A_1:B_1) xor ... xor E(A_k:B_k) (mod-2 sum over K_n):
//   sum_j [x(delta(T_j)) + x(E(A_j:B_j))] + x(F) >= 3k + 1.
// Valid for every tour: each domino term equals (x(delta A) + x(delta B) + x(delta T)) / 2 >= 3;
// equality 3k would need x(F) = 0 and every x(E(A_j:B_j)) = 1, hence x(delta S) = x(F) +
// sum_j x(E_j) = k odd (mod 2), but a tour crosses every cut an even number of times. Nothing
// else is required of S or of the dominoes (they may overlap), so any separation procedure is
// safe as long as dp_valid holds; violations are always recomputed exactly.
// Combs are the case S = H, A_j = T_j & H, B_j = T_j \ H.
struct Domino {
    std::vector<uint32_t> a, b;  // sorted, disjoint, nonempty
    bool operator<(const Domino& o) const { return a < o.a || (a == o.a && b < o.b); }
    bool operator==(const Domino& o) const { return a == o.a && b == o.b; }
};

struct DpCut {
    std::vector<uint32_t> handle;   // S, sorted (may be empty)
    std::vector<Domino> dominoes;   // sorted
    double violation;               // (3k + 1) - lhs
};

bool dp_valid(uint32_t n, const std::vector<Domino>& dominoes);

// Coefficient of edge (u, v) in the row, from per-node memberships (see DpMembership).
struct DpMembership {
    std::vector<uint8_t> in_s;
    std::vector<std::vector<std::pair<uint32_t, uint8_t>>> dom;  // node -> (domino j, side 0 = A / 1 = B), sorted by j
    DpMembership(uint32_t n, const std::vector<uint32_t>& handle, const std::vector<Domino>& dominoes);
    int coef(uint32_t u, uint32_t v) const;
};

double dp_violation(const SupportGraph& g, const std::vector<uint32_t>& handle, const std::vector<Domino>& dominoes);

struct DpConfig {
    double violation_tol = 1e-5;
    uint32_t max_tooth = 5000;  // BFS cap on a domino's T (larger thetas are skipped)
};

struct DpStats {
    uint32_t planar_edges = 0, dropped_edges = 0, faces = 0, dominoes = 0, cycles = 0, coloring_failed = 0, cuts = 0;
};

// Planar separation heuristic (exact only on a planar support): planar subgraph of the support
// from the coordinates (the lighter edge of every crossing pair is dropped), faces and dual graph;
// dominoes from min-cost triples of edge-disjoint dual paths between the two faces of each edge
// (theta = delta(T) plus the chord E(A:B)); then minimum odd cycles of weight < 1 in the dual
// plus one "odd" edge per domino; S from a 2-colouring. Every cut is checked with dp_valid and
// its violation recomputed exactly on `g`; sorted by violation.
std::vector<DpCut> separate_dp(const SupportGraph& g, const std::vector<double>& xs, const std::vector<double>& ys,
                               const DpConfig& cfg = {}, DpStats* stats = nullptr);

}  // namespace apex
