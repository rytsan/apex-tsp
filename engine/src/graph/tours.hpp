#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"

namespace apex {

struct Tour {
    std::vector<uint32_t> order;  // node indices 0..n-1, a Hamiltonian cycle
    int64_t cost = 0;
};

// Integer tour length with the instance metric (EUC_2D / CEIL_2D in double).
inline int64_t tour_cost(const Instance& inst, const Tour& t) {
    int64_t c = 0;
    const size_t n = t.order.size();
    for (size_t k = 0; k < n; ++k) c += dist(inst, t.order[k], t.order[k + 1 == n ? 0 : k + 1]);
    return c;
}

// True iff order is a permutation of 0..n-1.
inline bool is_valid_tour(const Tour& t, uint32_t n) {
    if (t.order.size() != n) return false;
    std::vector<uint8_t> seen(n, 0);
    for (uint32_t v : t.order) {
        if (v >= n || seen[v]) return false;
        seen[v] = 1;
    }
    return true;
}

}  // namespace apex
