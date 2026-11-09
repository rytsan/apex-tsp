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

namespace {

// For a fixed handle H the comb violation is additive over teeth:
//   (3k + 1) - x(delta(H)) - sum_j x(delta(T_j)) = 1 - x(delta(H)) + sum_j (3 - x(delta(T_j))),
// so teething picks disjoint teeth by benefit 3 - x(delta(T)) (the set version of best_teeth:
// a two-node tooth {u, v} has benefit 2 x_uv - 1).
struct Teether {
    const SupportGraph& g;
    std::vector<double> deg, acc;
    std::vector<uint8_t> in;
    std::vector<std::vector<uint32_t>> grown;  // per support edge, lazily
    std::vector<double> grown_cut;             // x(delta(grown[e]))
    static constexpr uint32_t kMaxTooth = 64;  // ponytail: fixed growth cap; tune if big teeth matter

    explicit Teether(const SupportGraph& gr)
        : g(gr), deg(gr.n, 0.0), acc(gr.n, 0.0), in(gr.n, 0), grown(gr.edges.size()), grown_cut(gr.edges.size(), 0.0) {
        for (const auto& e : g.edges) { deg[e.u] += e.cap; deg[e.v] += e.cap; }
    }

    // Tooth grown from edge e = (u, v): add the neighbor w with the largest x(w, T) while
    // deg(w) - 2 x(w, T) <= 0, i.e. while x(delta(T)) does not increase; ties smallest w.
    void grow(uint32_t e) {
        if (!grown[e].empty()) return;
        auto& t = grown[e];
        std::vector<uint32_t> touched;
        auto add = [&](uint32_t v) {
            t.push_back(v);
            in[v] = 1;
            for (uint32_t k = g.adj_start[v]; k < g.adj_start[v + 1]; ++k) {
                const uint32_t ed = g.adj[k], w = g.other(ed, v);
                if (in[w]) continue;
                if (acc[w] == 0.0) touched.push_back(w);
                acc[w] += g.edges[ed].cap;
            }
        };
        const auto& ed = g.edges[e];
        double cut = deg[ed.u] + deg[ed.v] - 2 * ed.cap;
        add(ed.u);
        add(ed.v);
        while (t.size() < kMaxTooth) {
            int64_t best = -1;
            for (uint32_t w : touched)
                if (!in[w] && deg[w] - 2 * acc[w] <= 1e-9 &&
                    (best < 0 || acc[w] > acc[best] + 1e-12 || (std::abs(acc[w] - acc[best]) <= 1e-12 && w < best)))
                    best = w;
            if (best < 0) break;
            cut += deg[best] - 2 * acc[best];
            add(static_cast<uint32_t>(best));
        }
        for (uint32_t v : t) in[v] = 0;
        for (uint32_t w : touched) acc[w] = 0.0;
        std::sort(t.begin(), t.end());
        grown_cut[e] = cut;
    }

    // Best comb for handle H from grown and two-node teeth; false if none with >= 3 teeth.
    bool comb_for(const std::vector<uint32_t>& handle, std::vector<std::vector<uint32_t>>& teeth) {
        std::vector<uint8_t> in_h(g.n, 0);
        for (uint32_t v : handle) in_h[v] = 1;
        struct Cand { double benefit; std::vector<uint32_t> set; };
        std::vector<Cand> cand;
        double handle_cut = 0;
        for (uint32_t e = 0; e < g.edges.size(); ++e) {
            const auto& ed = g.edges[e];
            if (in_h[ed.u] == in_h[ed.v]) continue;
            handle_cut += ed.cap;
            cand.push_back({2 * ed.cap - 1, {std::min(ed.u, ed.v), std::max(ed.u, ed.v)}});
            grow(e);
            if (grown[e].size() > 2) cand.push_back({3 - grown_cut[e], grown[e]});
        }
        std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
            return a.benefit > b.benefit || (a.benefit == b.benefit && a.set.size() < b.set.size());
        });
        std::vector<uint8_t> used(g.n, 0);
        auto free_set = [&](const std::vector<uint32_t>& s) {
            return std::none_of(s.begin(), s.end(), [&](uint32_t v) { return used[v] != 0; });
        };
        std::vector<size_t> pick;
        for (size_t i = 0; i < cand.size() && cand[i].benefit > 1e-12; ++i)
            if (free_set(cand[i].set)) {
                pick.push_back(i);
                for (uint32_t v : cand[i].set) used[v] = 1;
            }
        if (pick.size() % 2 == 0 && !pick.empty()) {  // parity: drop the weakest or add the best free one
            int64_t add = -1;
            for (size_t i = 0; i < cand.size() && add < 0; ++i)
                if (cand[i].benefit <= 1e-12 && free_set(cand[i].set)) add = static_cast<int64_t>(i);
            if (add >= 0 && cand[add].benefit > -cand[pick.back()].benefit) pick.push_back(static_cast<size_t>(add));
            else pick.pop_back();
        }
        if (pick.size() < 3) return false;
        double viol = 1 - handle_cut;
        bool beyond_blossom = false;
        teeth.clear();
        for (size_t i : pick) {
            viol += cand[i].benefit;
            beyond_blossom |= cand[i].set.size() > 2;
            teeth.push_back(cand[i].set);
        }
        std::sort(teeth.begin(), teeth.end());
        return beyond_blossom && viol > 0;
    }
};

}  // namespace

std::vector<CombCut> separate_comb(const SupportGraph& g, const BlossomConfig& cfg, CombStats* stats) {
    CombStats st;
    std::vector<CombCut> out;
    const auto sh = shrink_unit_edges(g);
    st.shrunk_nodes = sh.g.n;
    std::set<std::pair<std::vector<uint32_t>, std::vector<std::vector<uint32_t>>>> seen;
    std::vector<std::vector<uint32_t>> handles = heuristic_handles(g);
    auto emit = [&](std::vector<uint32_t> handle, std::vector<std::vector<uint32_t>> teeth) {
        ++st.candidates;
        if (!comb_valid(g.n, handle, teeth)) { ++st.invalid; return; }
        const double v = comb_violation(g, handle, teeth);  // exact re-verification
        if (!(v > cfg.violation_tol)) return;
        if (!seen.insert({handle, teeth}).second) return;
        out.push_back({std::move(handle), std::move(teeth), v});
    };
    if (sh.g.n < g.n && sh.g.n >= 6) {
        BlossomConfig bc = cfg;
        // ponytail: Padberg-Rao (Gusfield, ~n + m max flows) only on small shrunk graphs; large ones
        // keep the component heuristic until a faster exact routine (e.g. Letchford-Reinelt-Theis) exists.
        if (sh.g.n <= 3000) bc.mode = BlossomMode::Full;
        for (const auto& b : separate_blossom(sh.g, bc)) {
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
            handles.push_back(handle);  // teething below may find better teeth for it
            emit(std::move(handle), std::move(teeth));
        }
    }
    // Teething: grown teeth on every candidate handle.
    Teether th(g);
    std::vector<std::vector<uint32_t>> teeth;
    for (const auto& h : handles) {
        if (h.size() >= g.n) continue;
        if (th.comb_for(h, teeth)) {
            ++st.teething;
            emit(h, teeth);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const CombCut& a, const CombCut& b) { return a.violation > b.violation; });
    st.cuts = static_cast<uint32_t>(out.size());
    if (stats) *stats = st;
    return out;
}

}  // namespace apex
