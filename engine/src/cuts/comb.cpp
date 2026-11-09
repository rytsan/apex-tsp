#include "cuts/comb.hpp"

#include <algorithm>
#include <set>

#include "cuts/shrink.hpp"

namespace apex {

bool comb_valid(uint32_t n, const std::vector<uint32_t>& handle, const std::vector<std::vector<uint32_t>>& teeth) {
    if (teeth.size() < 3 || teeth.size() % 2 == 0 || handle.empty() || handle.size() >= n) return false;
    std::vector<uint8_t> in_h(n, 0), used(n, 0);
    for (uint32_t v : handle) in_h[v] = 1;
    for (const auto& t : teeth) {
        bool inside = false, outside = false;
        for (uint32_t v : t) {
            if (v >= n || used[v]) return false;  // teeth pairwise disjoint
            used[v] = 1;
            (in_h[v] ? inside : outside) = true;
        }
        if (!inside || !outside) return false;
    }
    return true;
}

double comb_violation(const SupportGraph& g, const std::vector<uint32_t>& handle,
                      const std::vector<std::vector<uint32_t>>& teeth) {
    // tooth_of[v] = 1 + tooth index (teeth are disjoint), 0 = none.
    std::vector<uint8_t> in_h(g.n, 0);
    std::vector<uint32_t> tooth_of(g.n, 0);
    for (uint32_t v : handle) in_h[v] = 1;
    for (uint32_t j = 0; j < teeth.size(); ++j)
        for (uint32_t v : teeth[j]) tooth_of[v] = j + 1;
    double lhs = 0;
    for (const auto& e : g.edges) {
        int coef = in_h[e.u] != in_h[e.v];
        if (tooth_of[e.u] != tooth_of[e.v]) coef += (tooth_of[e.u] != 0) + (tooth_of[e.v] != 0);
        lhs += coef * e.cap;
    }
    return 3.0 * static_cast<double>(teeth.size()) + 1.0 - lhs;
}

std::vector<CombCut> separate_comb(const SupportGraph& g, const BlossomConfig& cfg, CombStats* stats) {
    CombStats st;
    std::vector<CombCut> out;
    const auto sh = shrink_unit_edges(g);
    st.shrunk_nodes = sh.g.n;
    if (sh.g.n == g.n || sh.g.n < 6) {  // nothing shrunk: separate_blossom already covers it
        if (stats) *stats = st;
        return out;
    }
    BlossomConfig bc = cfg;
    // ponytail: Padberg-Rao (Gusfield, ~n + m max flows) only on small shrunk graphs; large ones
    // keep the component heuristic until a faster exact routine (e.g. Letchford-Reinelt-Theis) exists.
    if (sh.g.n <= 3000) bc.mode = BlossomMode::Full;
    std::set<std::pair<std::vector<uint32_t>, std::vector<std::vector<uint32_t>>>> seen;
    for (const auto& b : separate_blossom(sh.g, bc)) {
        ++st.candidates;
        auto handle = sh.expand(b.handle);
        std::vector<std::vector<uint32_t>> teeth;
        bool grown = handle.size() != b.handle.size();
        for (const auto& t : b.teeth) {
            std::vector<uint32_t> s = sh.expand({t.u, t.v});
            grown |= s.size() > 2;
            teeth.push_back(std::move(s));
        }
        if (!grown) continue;  // a plain blossom of g
        std::sort(teeth.begin(), teeth.end());
        if (!comb_valid(g.n, handle, teeth)) { ++st.invalid; continue; }
        const double v = comb_violation(g, handle, teeth);
        if (!(v > cfg.violation_tol)) continue;
        if (!seen.insert({handle, teeth}).second) continue;
        out.push_back({std::move(handle), std::move(teeth), v});
    }
    std::stable_sort(out.begin(), out.end(), [](const CombCut& a, const CombCut& b) { return a.violation > b.violation; });
    st.cuts = static_cast<uint32_t>(out.size());
    if (stats) *stats = st;
    return out;
}

}  // namespace apex
