#include "decomp/patching.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <atomic>
#include <exception>
#include <numeric>
#include <queue>
#include <mutex>
#include <thread>
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
                                   uint8_t axis, double split, const PatchConfig& cfg, std::vector<uint32_t>& touched,
                                   std::vector<uint32_t>* focus) {
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
    const uint32_t ends[4] = {a[bk], a[(bk + 1) % a.size()], b[bl], b[(bl + 1) % b.size()]};
    touched.insert(touched.end(), ends, ends + 4);
    if (focus) {
        focus->insert(focus->end(), sa.begin(), sa.end());
        focus->insert(focus->end(), sb.begin(), sb.end());
        focus->insert(focus->end(), ends, ends + 4);
        std::sort(focus->begin(), focus->end());
        focus->erase(std::unique(focus->begin(), focus->end()), focus->end());
    }
    return out;
}

int64_t reevaluate_cycle(const Instance& inst, std::vector<uint32_t>& cycle, const std::vector<uint32_t>& focus,
                         uint64_t iterations, uint64_t seed) {
    const auto m = static_cast<uint32_t>(cycle.size());
    if (m < 8 || iterations == 0) return 0;
    const Instance sub = sub_instance(inst, cycle);  // local id k = cycle position k
    const auto g = build_candidate_graph(sub);
    Tour t;
    t.order.resize(m);
    std::iota(t.order.begin(), t.order.end(), 0u);
    t.cost = tour_cost(sub, t);
    const int64_t before = t.cost;
    std::vector<std::pair<uint32_t, uint32_t>> local(m);
    for (uint32_t k = 0; k < m; ++k) local[k] = {cycle[k], k};
    std::sort(local.begin(), local.end());
    IlsConfig ic;
    ic.seed = seed;
    ic.max_iterations = iterations;
    for (uint32_t v : focus) {
        const auto it = std::lower_bound(local.begin(), local.end(), std::make_pair(v, 0u));
        if (it != local.end() && it->first == v) ic.focus.push_back(it->second);
    }
    iterated_local_search(t, sub, g, ic);
    std::vector<uint32_t> out(m);
    for (uint32_t k = 0; k < m; ++k) out[k] = cycle[t.order[k]];
    cycle.swap(out);
    return before - t.cost;
}

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Runs work(i) for i in [0, count) on `threads` threads; first exception rethrown here.
template <class F>
void parallel_for(size_t count, uint32_t threads, F&& work) {
    std::atomic<size_t> next{0};
    std::vector<std::exception_ptr> err(std::max(1u, threads));
    auto run = [&](uint32_t tid) {
        try {
            for (size_t i; (i = next.fetch_add(1)) < count;) work(i);
        } catch (...) {
            err[tid] = std::current_exception();
            next.store(count);
        }
    };
    const uint32_t nt = std::max(1u, std::min<uint32_t>(threads, static_cast<uint32_t>(count)));
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < nt; ++t) pool.emplace_back(run, t);
    run(0);
    for (auto& th : pool) th.join();
    for (auto& e : err)
        if (e) std::rethrow_exception(e);
}

}  // namespace

Tour decompose_and_patch(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, DecompStats* stats) {
    using Clock = std::chrono::steady_clock;
    DecompStats st;
    const auto t0 = Clock::now();
    const auto tree = cfg.density ? build_density_tree(inst, cfg.max_leaf) : build_kdtree(inst, cfg.max_leaf);
    if (cfg.leaf.on_cluster_box) {  // children come after their parent: boxes bottom-up in reverse
        std::vector<std::array<double, 4>> box(tree.size(), {INFINITY, INFINITY, -INFINITY, -INFINITY});
        for (size_t k = tree.size(); k-- > 0;) {
            auto& b = box[k];
            for (uint32_t p : tree[k].points) {
                b[0] = std::min(b[0], inst.xs[p]); b[1] = std::min(b[1], inst.ys[p]);
                b[2] = std::max(b[2], inst.xs[p]); b[3] = std::max(b[3], inst.ys[p]);
            }
            for (int32_t c : {tree[k].left, tree[k].right}) {
                if (c < 0) continue;
                const auto& cb = box[static_cast<size_t>(c)];
                b[0] = std::min(b[0], cb[0]); b[1] = std::min(b[1], cb[1]);
                b[2] = std::max(b[2], cb[2]); b[3] = std::max(b[3], cb[3]);
            }
        }
        for (size_t k = 0; k < tree.size(); ++k)
            cfg.leaf.on_cluster_box(static_cast<uint32_t>(k), box[k][0], box[k][1], box[k][2], box[k][3]);
    }
    auto cycles = solve_leaves(inst, tree, cfg.leaf, &st.leaf);
    for (const auto& c : cycles) {
        if (c.size() < 2) continue;
        for (size_t k = 0; k < c.size(); ++k) st.leaf_sum += dist(inst, c[k], c[(k + 1) % c.size()]);
    }
    const auto t1 = Clock::now();
    st.leaf_s = std::chrono::duration<double>(t1 - t0).count();
    // Levels: children come after their parent, so depth is known in one forward pass.
    std::vector<uint32_t> depth(tree.size(), 0);
    uint32_t max_depth = 0;
    for (size_t k = 0; k < tree.size(); ++k)
        if (tree[k].left >= 0) {
            depth[static_cast<size_t>(tree[k].left)] = depth[static_cast<size_t>(tree[k].right)] = depth[k] + 1;
            max_depth = std::max(max_depth, depth[k] + 1);
        }
    std::vector<std::vector<uint32_t>> touched(tree.size());
    std::vector<int64_t> stitch(tree.size(), 0), gain(tree.size(), 0);
    auto cost_of = [&](const std::vector<uint32_t>& c) {
        int64_t x = 0;
        for (size_t k = 0; c.size() > 1 && k < c.size(); ++k) x += dist(inst, c[k], c[(k + 1) % c.size()]);
        return x;
    };
    for (uint32_t d = max_depth; d-- > 0;) {
        std::vector<size_t> level;
        for (size_t k = 0; k < tree.size(); ++k)
            if (depth[k] == d && tree[k].left >= 0) level.push_back(k);
        if (level.empty()) continue;
        ++st.levels;
        parallel_for(level.size(), cfg.leaf.threads, [&](size_t i) {
            const size_t k = level[i];
            const auto& nd = tree[k];
            if (cfg.leaf.on_cluster) cfg.leaf.on_cluster(static_cast<uint32_t>(k), 2);
            auto& a = cycles[static_cast<size_t>(nd.left)];
            auto& b = cycles[static_cast<size_t>(nd.right)];
            const int64_t parts = cost_of(a) + cost_of(b);
            std::vector<uint32_t> focus;
            auto merged = merge_cycles(inst, a, b, nd.axis, nd.split, cfg.patch, touched[k], &focus);
            stitch[k] = cost_of(merged) - parts;
            gain[k] = reevaluate_cycle(inst, merged, focus, uint64_t(cfg.merge_iterations_per_focus) * focus.size(),
                                       mix64(cfg.leaf.seed ^ mix64(k + 0x51ED)));
            a.clear();
            b.clear();
            cycles[k] = std::move(merged);
            if (cfg.leaf.on_cluster) cfg.leaf.on_cluster(static_cast<uint32_t>(k), 0);
        });
        st.merges += static_cast<uint32_t>(level.size());
    }
    for (size_t k = 0; k < tree.size(); ++k) {
        st.stitch_cost += stitch[k];
        st.reeval_gain += gain[k];
    }
    Tour t;
    t.order = std::move(cycles[0]);
    t.cost = tour_cost(inst, t);
    st.patched_cost = t.cost;
    std::vector<uint32_t> all;
    for (const auto& v : touched) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    st.polish_moves = lk_descent(t, inst, g, all);
    st.final_cost = t.cost;
    st.patch_s = std::chrono::duration<double>(Clock::now() - t1).count();
    if (stats) *stats = st;
    return t;
}

Tour cluster_merge(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, DecompStats* stats) {
    using Clock = std::chrono::steady_clock;
    DecompStats st;
    const auto t0 = Clock::now();
    const uint32_t n = inst.n;
    const auto tree = build_density_tree(inst, cfg.max_leaf);
    // Forest shown to the GUI: one slot per current cluster, filled as leaves / groups finish.
    std::mutex forest_mu;
    std::vector<std::vector<uint32_t>> forest;
    std::vector<int32_t> slot_of_leaf(tree.size(), -1);
    for (size_t k = 0; k < tree.size(); ++k)
        if (tree[k].left < 0) {
            slot_of_leaf[k] = static_cast<int32_t>(forest.size());
            forest.emplace_back();
        }
    auto show = [&](auto&& update) {
        if (!cfg.on_forest) return;
        std::lock_guard<std::mutex> lk(forest_mu);
        update();
        cfg.on_forest(forest);
    };
    LeafConfig lc = cfg.leaf;
    if (cfg.on_forest)
        lc.on_leaf = [&](uint32_t id, const std::vector<uint32_t>& c) { show([&] { forest[slot_of_leaf[id]] = c; }); };
    if (cfg.leaf.on_cluster_box) {  // leaf boxes only (no internal nodes: merges follow the MST)
        for (size_t k = 0; k < tree.size(); ++k) {
            if (tree[k].left >= 0) continue;
            double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (uint32_t v : tree[k].points) {
                x0 = std::min(x0, inst.xs[v]); y0 = std::min(y0, inst.ys[v]);
                x1 = std::max(x1, inst.xs[v]); y1 = std::max(y1, inst.ys[v]);
            }
            cfg.leaf.on_cluster_box(static_cast<uint32_t>(k), x0, y0, x1, y1);
        }
    }
    auto leaf_cycles = solve_leaves(inst, tree, lc, &st.leaf);
    std::vector<std::vector<uint32_t>> cyc;  // current clusters
    for (auto& c : leaf_cycles)
        if (!c.empty()) cyc.push_back(std::move(c));
    for (const auto& c : cyc)
        for (size_t k = 0; c.size() > 1 && k < c.size(); ++k) st.leaf_sum += dist(inst, c[k], c[(k + 1) % c.size()]);
    const auto t1 = Clock::now();
    st.leaf_s = std::chrono::duration<double>(t1 - t0).count();
    show([&] { forest = cyc; });

    std::vector<uint32_t> owner(n, 0), pos(n, 0), touched;
    std::mutex touched_mu;
    uint32_t round = 0;
    while (cyc.size() > 1) {
        ++round;
        const auto C = static_cast<uint32_t>(cyc.size());
        for (uint32_t c = 0; c < C; ++c)
            for (uint32_t v : cyc[c]) owner[v] = c;
        // Cluster graph: cheapest candidate edge between every pair of touching clusters.
        std::vector<std::vector<std::pair<uint32_t, int64_t>>> cadj(C);
        {
            std::vector<std::tuple<uint32_t, uint32_t, int64_t>> cross;
            for (const auto& e : g.edges)
                if (owner[e.u] != owner[e.v])
                    cross.push_back({std::min(owner[e.u], owner[e.v]), std::max(owner[e.u], owner[e.v]), dist(inst, e.u, e.v)});
            std::sort(cross.begin(), cross.end());
            for (size_t k = 0; k < cross.size(); ++k) {
                if (k > 0 && std::get<0>(cross[k]) == std::get<0>(cross[k - 1]) && std::get<1>(cross[k]) == std::get<1>(cross[k - 1])) continue;
                const auto [a, b, w] = cross[k];
                cadj[a].push_back({b, w});
                cadj[b].push_back({a, w});
            }
        }
        // Prim's MST over the clusters (the candidate graph is connected, so is this graph).
        std::vector<std::tuple<int64_t, uint32_t, uint32_t>> mst;  // (weight, a, b)
        {
            std::vector<uint8_t> in(C, 0);
            using QE = std::tuple<int64_t, uint32_t, uint32_t>;
            std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
            for (uint32_t s0 = 0; s0 < C; ++s0) {
                if (in[s0]) continue;
                in[s0] = 1;
                for (auto [b, w] : cadj[s0]) pq.push({w, s0, b});
                while (!pq.empty()) {
                    auto [w, a, b] = pq.top();
                    pq.pop();
                    if (in[b]) continue;
                    in[b] = 1;
                    mst.push_back({w, a, b});
                    for (auto [c2, w2] : cadj[b])
                        if (!in[c2]) pq.push({w2, b, c2});
                }
            }
        }
        // Group cap from density: points per bounding-box area vs the median cluster.
        std::vector<double> dens(C);
        for (uint32_t c = 0; c < C; ++c) {
            double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (uint32_t v : cyc[c]) {
                x0 = std::min(x0, inst.xs[v]); y0 = std::min(y0, inst.ys[v]);
                x1 = std::max(x1, inst.xs[v]); y1 = std::max(y1, inst.ys[v]);
            }
            dens[c] = cyc[c].size() / std::max(1e-9, (x1 - x0) * (y1 - y0));
        }
        auto sorted = dens;
        std::nth_element(sorted.begin(), sorted.begin() + C / 2, sorted.end());
        const double med = sorted[C / 2];
        const uint32_t maxg = std::max(2u, cfg.max_group);
        std::vector<uint32_t> cap(C);
        for (uint32_t c = 0; c < C; ++c)
            cap[c] = dens[c] >= med ? 2u : dens[c] >= med / 4 ? std::min(3u, maxg) : maxg;
        // Groups along MST edges, cheapest first (union-find; cap = smallest member cap).
        std::vector<uint32_t> root(C), gsize(C, 1), gcap(cap);
        std::iota(root.begin(), root.end(), 0u);
        auto find = [&](uint32_t x) {
            while (root[x] != x) x = root[x] = root[root[x]];
            return x;
        };
        std::sort(mst.begin(), mst.end());
        std::vector<std::vector<std::pair<uint32_t, uint32_t>>> gedges(C);  // MST edges inside a group
        for (const auto& [w, a, b] : mst) {
            uint32_t ra = find(a), rb = find(b);
            const uint32_t gc = std::min(gcap[ra], gcap[rb]);
            if (ra == rb || gsize[ra] + gsize[rb] > gc) continue;
            if (ra > rb) std::swap(ra, rb);
            root[rb] = ra;
            gsize[ra] += gsize[rb];
            gcap[ra] = gc;
            gedges[ra].insert(gedges[ra].end(), gedges[rb].begin(), gedges[rb].end());
            gedges[rb].clear();
            gedges[ra].push_back({a, b});
        }
        std::vector<uint32_t> groups;
        for (uint32_t c = 0; c < C; ++c)
            if (find(c) == c && gsize[c] > 1) groups.push_back(c);
        // Merge every group: clusters joined in the order of its MST edges.
        std::vector<std::vector<uint32_t>> merged(C);
        std::vector<int64_t> gstitch(C, 0), ggain(C, 0);
        parallel_for(groups.size(), cfg.leaf.threads, [&](size_t gi) {
            const uint32_t r = groups[gi];
            std::vector<uint32_t> members{r};
            std::vector<uint8_t> done_edge(gedges[r].size(), 0);
            std::vector<uint32_t> cur = cyc[r], focus;
            auto in_cur = [&](uint32_t c) { return std::find(members.begin(), members.end(), c) != members.end(); };
            for (size_t step = 0; step + 1 < gsize[r]; ++step) {
                uint32_t next = UINT32_MAX;  // a member joined to cur by an MST edge
                for (size_t k = 0; k < gedges[r].size() && next == UINT32_MAX; ++k) {
                    if (done_edge[k]) continue;
                    const auto [a, b] = gedges[r][k];
                    if (in_cur(a) != in_cur(b)) { next = in_cur(a) ? b : a; done_edge[k] = 1; }
                }
                const auto& other = cyc[next];
                const auto m1 = static_cast<uint32_t>(cur.size()), m2 = static_cast<uint32_t>(other.size());
                for (uint32_t k = 0; k < m1; ++k) pos[cur[k]] = k;
                for (uint32_t k = 0; k < m2; ++k) pos[other[k]] = k;
                // Cheapest exchange: drop (x, y = succ x) in cur and (p, q = succ p) in other, add
                // (x, p) + (q, y) [other reversed] or (x, q) + (p, y) [other forward].
                int64_t best = INT64_MAX;
                uint32_t bx = 0, bp = 0;
                bool brev = false;
                for (uint32_t v : other)
                    for (uint32_t k = g.adj_start[v]; k < g.adj_start[v + 1]; ++k) {
                        const uint32_t u = g.other(g.adj[k], v);
                        if (!in_cur(owner[u])) continue;
                        for (uint32_t x : {u, cur[(pos[u] + m1 - 1) % m1]})
                            for (uint32_t p : {v, other[(pos[v] + m2 - 1) % m2]}) {
                                const uint32_t y = cur[(pos[x] + 1) % m1], q = other[(pos[p] + 1) % m2];
                                const int64_t base = dist(inst, x, y) + dist(inst, p, q);
                                const int64_t d1 = dist(inst, x, p) + dist(inst, q, y) - base;
                                const int64_t d2 = dist(inst, x, q) + dist(inst, p, y) - base;
                                if (d1 < best) { best = d1; bx = x; bp = p; brev = true; }
                                if (d2 < best) { best = d2; bx = x; bp = p; brev = false; }
                            }
                    }
                const uint32_t y = cur[(pos[bx] + 1) % m1], q = other[(pos[bp] + 1) % m2];
                std::vector<uint32_t> out;
                out.reserve(m1 + m2);
                for (uint32_t k = 0; k < m1; ++k) out.push_back(cur[(pos[y] + k) % m1]);  // y .. x
                for (uint32_t k = 0; k < m2; ++k)
                    out.push_back(brev ? other[(pos[bp] + m2 - k) % m2] : other[(pos[q] + k) % m2]);  // p .. q or q .. p
                gstitch[r] += best;
                for (uint32_t v : {bx, y, bp, q}) {
                    focus.push_back(v);
                    for (uint32_t k = g.adj_start[v]; k < g.adj_start[v + 1]; ++k) focus.push_back(g.other(g.adj[k], v));
                }
                cur.swap(out);
                members.push_back(next);
            }
            std::sort(focus.begin(), focus.end());
            focus.erase(std::unique(focus.begin(), focus.end()), focus.end());
            ggain[r] = reevaluate_cycle(inst, cur, focus, uint64_t(cfg.merge_iterations_per_focus) * focus.size(),
                                        mix64(cfg.leaf.seed ^ mix64((uint64_t(round) << 32) | r)));
            {
                std::lock_guard<std::mutex> lk(touched_mu);
                touched.insert(touched.end(), focus.begin(), focus.end());
            }
            show([&] {
                for (uint32_t c : members)
                    if (c != r) forest[c].clear();
                forest[r] = cur;
            });
            merged[r] = std::move(cur);
        });
        // Next round: merged groups plus the clusters that stayed alone, in cluster order.
        std::vector<std::vector<uint32_t>> nxt;
        for (uint32_t c = 0; c < C; ++c) {
            if (find(c) != c) continue;
            nxt.push_back(gsize[c] > 1 ? std::move(merged[c]) : std::move(cyc[c]));
            st.stitch_cost += gstitch[c];
            st.reeval_gain += ggain[c];
            if (gsize[c] > 1) st.merges += gsize[c] - 1;
        }
        cyc.swap(nxt);
        ++st.levels;
        show([&] { forest = cyc; });
    }
    Tour t;
    t.order = cyc.empty() ? std::vector<uint32_t>{} : std::move(cyc[0]);
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
