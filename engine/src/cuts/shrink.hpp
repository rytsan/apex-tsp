#pragma once

#include <cstdint>
#include <vector>

#include "cuts/support.hpp"

namespace apex {

// Support graph after safe shrinking. Nodes of `g` are supernodes; parallel edges are summed,
// self-loops dropped (lp_col = UINT32_MAX on shrunken edges).
struct ShrunkGraph {
    SupportGraph g;
    std::vector<std::vector<uint32_t>> members;  // supernode -> sorted original nodes
    std::vector<uint32_t> super_of;              // original node -> supernode

    // Original node set of a supernode set (sorted, unique).
    std::vector<uint32_t> expand(const std::vector<uint32_t>& supernodes) const;
};

// Iterative safe shrinking (MASTER §6): contract (super)edges (A, B) with x(A, B) >= 1 - tol
// whose endpoints BOTH have x(delta(.)) = 2. Safety: if S is violated with A in S, B not in S,
//   x(delta(S+B)) = x(delta(S)) + x(delta(B)) - 2 x(B, S) <= x(delta(S)) + 2 - 2 = x(delta(S)),
// so S + B is at least as violated, unless S + B = V; then S = V \ B has x(delta(S)) =
// x(delta(B)) = 2, i.e. S was not violated. Symmetric for B in S. Hence min_S x(delta(S)) is
// preserved. The degree condition matters: merging two supernodes joined by c > 1 yields a
// supernode of degree 4 - 2c < 2 (itself a violated SEC); it stays a singleton and is never
// merged again. Each round merges a matching of eligible pairs (disjoint merges commute).
// The file is named shrink.* (not support.*) to keep support.cpp a single responsibility;
// the API lives in the same `cuts/` layer as MASTER §3 places support/shrinking.
ShrunkGraph shrink_unit_edges(const SupportGraph& g, double tol = 1e-9);

}  // namespace apex
