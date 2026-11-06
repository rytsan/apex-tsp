#include "decomp/patching.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

#include "primal/lk_lite.hpp"

namespace apex {

namespace {

// The m points of `c` nearest the split plane (ties by id).
std::vector<uint32_t> near_plane(const Instance& inst, const std::vector<uint32_t>& c, uint8_t axis, double split, uint32_t m) {
    const auto& co = axis == 0 ? inst.xs : inst.ys;
    std::vector<uint32_t> v(c);
    auto key = [&](uint32_t p) { return std::make_pair(std::abs(co[p] - split), p); };
    const size_t k = std::min<size_t>(m, v.size());
    std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end(),
                      [&](uint32_t x, uint32_t y) { return key(x) < key(y); });
    v.resize(k);
    return v;
}

}  // namespace

std::vector<uint32_t> merge_cycles(const Instance& inst, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                                   uint8_t axis, double split, const PatchConfig& cfg, std::vector<uint32_t>& touched) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    // Portals: closest cross pairs among the boundary samples.
    const auto sa = near_plane(inst, a, axis, split, cfg.boundary_sample);
    const auto sb = near_plane(inst, b, axis, split, cfg.boundary_sample);
    std::vector<std::tuple<int32_t, uint32_t, uint32_t>> pairs;
    for (uint32_t u : sa)
        for (uint32_t v : sb) pairs.emplace_back(dist(inst, u, v), u, v);
    const size_t np = std::min<size_t>(std::max(1u, cfg.portals), pairs.size());
    std::partial_sort(pairs.begin(), pairs.begin() + static_cast<std::ptrdiff_t>(np), pairs.end());
    pairs.resize(np);

    // Cycle positions (local maps: a / b are small next to n only at the top merge).
    auto index = [](const std::vector<uint32_t>& c) {
        std::vector<std::pair<uint32_t, uint32_t>> m;
        m.reserve(c.size());
        for (uint32_t k = 0; k < c.size(); ++k) m.emplace_back(c[k], k);
        std::sort(m.begin(), m.end());
        return m;
    };
    const auto ia = index(a), ib = index(b);
    auto pos = [](const std::vector<std::pair<uint32_t, uint32_t>>& m, uint32_t v) {
        return std::lower_bound(m.begin(), m.end(), std::make_pair(v, 0u))->second;
    };
    // Candidate tour edges (start position k: edge c[k] -> c[k+1]) at portal endpoints.
    std::vector<uint32_t> ea, eb;
    for (const auto& [d, u, v] : pairs) {
        const uint32_t pu = pos(ia, u), pv = pos(ib, v);
        ea.push_back(pu);
        ea.push_back(static_cast<uint32_t>((pu + a.size() - 1) % a.size()));
        eb.push_back(pv);
        eb.push_back(static_cast<uint32_t>((pv + b.size() - 1) % b.size()));
    }
    std::sort(ea.begin(), ea.end());
    ea.erase(std::unique(ea.begin(), ea.end()), ea.end());
    std::sort(eb.begin(), eb.end());
    eb.erase(std::unique(eb.begin(), eb.end()), eb.end());

    int64_t best = INT64_MAX;
    uint32_t bk = 0, bl = 0;
    bool bflip = false;
    for (uint32_t k : ea) {
        const uint32_t a1 = a[k], a2 = a[(k + 1) % a.size()];
        const int64_t da = a.size() > 1 ? dist(inst, a1, a2) : 0;
        for (uint32_t l : eb) {
            const uint32_t b1 = b[l], b2 = b[(l + 1) % b.size()];
            const int64_t db = b.size() > 1 ? dist(inst, b1, b2) : 0;
            const int64_t d1 = dist(inst, a1, b1) + dist(inst, a2, b2) - da - db;  // a2..a1 b1..(backward)..b2
            const int64_t d2 = dist(inst, a1, b2) + dist(inst, a2, b1) - da - db;  // a2..a1 b2..(forward)..b1
            if (d1 < best) { best = d1; bk = k; bl = l; bflip = false; }
            if (d2 < best) { best = d2; bk = k; bl = l; bflip = true; }
        }
    }
    std::vector<uint32_t> out;
    out.reserve(a.size() + b.size());
    for (size_t s = 1; s <= a.size(); ++s) out.push_back(a[(bk + s) % a.size()]);  // a2 .. a1
    if (!bflip) {
        for (size_t s = 0; s < b.size(); ++s) out.push_back(b[(bl + b.size() - s) % b.size()]);  // b1 backward .. b2
    } else {
        for (size_t s = 1; s <= b.size(); ++s) out.push_back(b[(bl + s) % b.size()]);  // b2 forward .. b1
    }
    touched.push_back(a[bk]);
    touched.push_back(a[(bk + 1) % a.size()]);
    touched.push_back(b[bl]);
    touched.push_back(b[(bl + 1) % b.size()]);
    return out;
}

Tour decompose_and_patch(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, DecompStats* stats) {
    using Clock = std::chrono::steady_clock;
    DecompStats st;
    const auto t0 = Clock::now();
    const auto tree = build_kdtree(inst, cfg.max_leaf);
    auto cycles = solve_leaves(inst, tree, cfg.leaf, &st.leaf);
    for (const auto& c : cycles) {
        if (c.size() < 2) continue;
        for (size_t k = 0; k < c.size(); ++k) st.leaf_sum += dist(inst, c[k], c[(k + 1) % c.size()]);
    }
    const auto t1 = Clock::now();
    st.leaf_s = std::chrono::duration<double>(t1 - t0).count();
    // Children follow their parent in `tree`, so a reverse sweep merges bottom-up.
    std::vector<uint32_t> touched;
    for (size_t k = tree.size(); k-- > 0;) {
        const auto& nd = tree[k];
        if (nd.left < 0) continue;
        cycles[k] = merge_cycles(inst, cycles[static_cast<size_t>(nd.left)], cycles[static_cast<size_t>(nd.right)], nd.axis,
                                 nd.split, cfg.patch, touched);
        cycles[static_cast<size_t>(nd.left)].clear();
        cycles[static_cast<size_t>(nd.right)].clear();
        ++st.merges;
    }
    Tour t;
    t.order = std::move(cycles[0]);
    t.cost = tour_cost(inst, t);
    st.patched_cost = t.cost;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    st.polish_moves = lk_descent(t, inst, g, touched);
    st.final_cost = t.cost;
    st.patch_s = std::chrono::duration<double>(Clock::now() - t1).count();
    if (stats) *stats = st;
    return t;
}

}  // namespace apex
