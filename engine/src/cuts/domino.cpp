#include "cuts/domino.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>

#include "core/control.hpp"

namespace apex {
namespace {

using Mem = std::vector<std::pair<uint32_t, uint8_t>>;  // (domino j, side), sorted by j

// Row coefficient of edge (u, v): sum_j ([crosses T_j] + [in E(A_j:B_j)]) + parity term of F.
int dp_coef(const Mem& du, const Mem& dv, bool su, bool sv) {
    int tcross = 0, eab = 0;
    size_t i = 0, k = 0;
    while (i < du.size() || k < dv.size()) {
        if (k == dv.size() || (i < du.size() && du[i].first < dv[k].first)) { ++tcross; ++i; }
        else if (i == du.size() || dv[k].first < du[i].first) { ++tcross; ++k; }
        else { eab += du[i].second != dv[k].second; ++i; ++k; }
    }
    return tcross + eab + ((su != sv) + eab) % 2;
}

}  // namespace

bool dp_valid(uint32_t n, const std::vector<Domino>& dominoes) {
    if (dominoes.empty() || dominoes.size() % 2 == 0) return false;
    std::vector<uint8_t> mark(n, 0);
    for (const auto& d : dominoes) {
        if (d.a.empty() || d.b.empty() || d.a.size() + d.b.size() >= n) return false;
        bool ok = true;
        for (const auto* side : {&d.a, &d.b})
            for (uint32_t v : *side) {
                if (v >= n || mark[v]) { ok = false; break; }
                mark[v] = 1;
            }
        for (const auto* side : {&d.a, &d.b})
            for (uint32_t v : *side)
                if (v < n) mark[v] = 0;
        if (!ok) return false;
    }
    return true;
}

DpMembership::DpMembership(uint32_t n, const std::vector<uint32_t>& handle, const std::vector<Domino>& dominoes)
    : in_s(n, 0), dom(n) {
    for (uint32_t v : handle) in_s[v] = 1;
    for (uint32_t j = 0; j < dominoes.size(); ++j) {
        for (uint32_t v : dominoes[j].a) dom[v].push_back({j, 0});
        for (uint32_t v : dominoes[j].b) dom[v].push_back({j, 1});
    }
}

int DpMembership::coef(uint32_t u, uint32_t v) const { return dp_coef(dom[u], dom[v], in_s[u], in_s[v]); }

double dp_violation(const SupportGraph& g, const std::vector<uint32_t>& handle, const std::vector<Domino>& dominoes) {
    const DpMembership m(g.n, handle, dominoes);
    double lhs = 0;
    for (const auto& e : g.edges) lhs += m.coef(e.u, e.v) * e.cap;
    return 3.0 * static_cast<double>(dominoes.size()) + 1.0 - lhs;
}

namespace {

struct Pt { double x, y; };

double orient(Pt a, Pt b, Pt c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
bool within(Pt a, Pt b, Pt c) {  // c (collinear with ab) lies on the closed segment ab
    return std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= c.y && c.y <= std::max(a.y, b.y);
}

// Segments (p, q) and (r, s) conflict in a straight-line embedding: proper crossing, a vertex on
// the other segment, or (sharing an endpoint) collinear overlap.
bool conflict(uint32_t p, uint32_t q, uint32_t r, uint32_t s, const std::vector<double>& xs, const std::vector<double>& ys) {
    const Pt P{xs[p], ys[p]}, Q{xs[q], ys[q]}, R{xs[r], ys[r]}, S{xs[s], ys[s]};
    auto shared = [&](uint32_t a, uint32_t b, uint32_t c) {  // common endpoint a; others b, c
        const Pt A{xs[a], ys[a]}, B{xs[b], ys[b]}, C{xs[c], ys[c]};
        return orient(A, B, C) == 0 && (B.x - A.x) * (C.x - A.x) + (B.y - A.y) * (C.y - A.y) > 0;
    };
    if (p == r) return shared(p, q, s);
    if (p == s) return shared(p, q, r);
    if (q == r) return shared(q, p, s);
    if (q == s) return shared(q, p, r);
    const double o1 = orient(P, Q, R), o2 = orient(P, Q, S), o3 = orient(R, S, P), o4 = orient(R, S, Q);
    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) return true;
    return (o1 == 0 && within(P, Q, R)) || (o2 == 0 && within(P, Q, S)) || (o3 == 0 && within(R, S, P)) ||
           (o4 == 0 && within(R, S, Q));
}

struct DomCand {
    Domino d;
    uint32_t f, g;  // dual endpoints (f == g: loop)
    double w;       // (x(dA) + x(dB) + x(dT)) / 2 - 3
};

}  // namespace

std::vector<DpCut> separate_dp(const SupportGraph& g, const std::vector<double>& xs, const std::vector<double>& ys,
                               const DpConfig& cfg, DpStats* stats) {
    DpStats st;
    std::vector<DpCut> out;
    const uint32_t n = g.n;
    if (n < 6 || xs.size() < n || ys.size() < n) { if (stats) *stats = st; return out; }

    // ---- 1. planar subgraph: heaviest edges first, skip any edge conflicting with a kept one.
    double x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (uint32_t v = 0; v < n; ++v) { x0 = std::min(x0, xs[v]); x1 = std::max(x1, xs[v]); y0 = std::min(y0, ys[v]); y1 = std::max(y1, ys[v]); }
    const uint32_t m = static_cast<uint32_t>(g.edges.size());
    const double side = std::max({x1 - x0, y1 - y0, 1e-9});
    const uint32_t G = std::clamp<uint32_t>(static_cast<uint32_t>(std::sqrt(static_cast<double>(m))), 1, 2048);
    const double cell = side / G * (1 + 1e-9);
    auto cx = [&](double x) { return std::min<uint32_t>(G - 1, static_cast<uint32_t>((x - x0) / cell)); };
    auto cy = [&](double y) { return std::min<uint32_t>(G - 1, static_cast<uint32_t>((y - y0) / cell)); };
    std::vector<std::vector<uint32_t>> grid(static_cast<size_t>(G) * G);
    std::vector<uint32_t> order(m), stamp(m, 0);
    for (uint32_t e = 0; e < m; ++e) order[e] = e;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return g.edges[a].cap > g.edges[b].cap; });
    std::vector<uint32_t> kept;  // support edge ids
    uint32_t tick = 0;
    for (uint32_t e : order) {
        const auto& ed = g.edges[e];
        if (xs[ed.u] == xs[ed.v] && ys[ed.u] == ys[ed.v]) { ++st.dropped_edges; continue; }
        const uint32_t ax = cx(std::min(xs[ed.u], xs[ed.v])), bx = cx(std::max(xs[ed.u], xs[ed.v]));
        const uint32_t ay = cy(std::min(ys[ed.u], ys[ed.v])), by = cy(std::max(ys[ed.u], ys[ed.v]));
        ++tick;
        bool bad = false;
        for (uint32_t i = ax; i <= bx && !bad; ++i)
            for (uint32_t j = ay; j <= by && !bad; ++j)
                for (uint32_t o : grid[static_cast<size_t>(i) * G + j]) {
                    if (stamp[o] == tick) continue;
                    stamp[o] = tick;
                    if (conflict(ed.u, ed.v, g.edges[o].u, g.edges[o].v, xs, ys)) { bad = true; break; }
                }
        if (bad) { ++st.dropped_edges; continue; }
        for (uint32_t i = ax; i <= bx; ++i)
            for (uint32_t j = ay; j <= by; ++j) grid[static_cast<size_t>(i) * G + j].push_back(e);
        kept.push_back(e);
    }
    st.planar_edges = static_cast<uint32_t>(kept.size());
    const uint32_t K = static_cast<uint32_t>(kept.size());

    // ---- 2. faces: half-edge 2i (u -> v) / 2i + 1 (v -> u) of kept edge i, ccw rotation per node.
    std::vector<std::vector<uint32_t>> rot(n);
    auto tail = [&](uint32_t h) { const auto& e = g.edges[kept[h >> 1]]; return (h & 1) ? e.v : e.u; };
    auto head = [&](uint32_t h) { const auto& e = g.edges[kept[h >> 1]]; return (h & 1) ? e.u : e.v; };
    for (uint32_t h = 0; h < 2 * K; ++h) rot[tail(h)].push_back(h);
    std::vector<uint32_t> pos(2 * K);
    for (uint32_t v = 0; v < n; ++v) {
        auto& r = rot[v];
        std::sort(r.begin(), r.end(), [&](uint32_t a, uint32_t b) {
            const double ta = std::atan2(ys[head(a)] - ys[v], xs[head(a)] - xs[v]);
            const double tb = std::atan2(ys[head(b)] - ys[v], xs[head(b)] - xs[v]);
            return ta < tb || (ta == tb && a < b);
        });
        for (uint32_t k = 0; k < r.size(); ++k) pos[r[k]] = k;
    }
    auto next = [&](uint32_t h) {  // next half-edge of h's face: at head(h), clockwise from the twin
        const auto& r = rot[head(h)];
        return r[(pos[h ^ 1] + r.size() - 1) % r.size()];
    };
    constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> face(2 * K, kNone);
    uint32_t F = 0;
    for (uint32_t h = 0; h < 2 * K; ++h) {
        if (face[h] != kNone) continue;
        for (uint32_t c = h; face[c] == kNone; c = next(c)) face[c] = F;
        ++F;
    }
    st.faces = F;
    // Dual adjacency (bridges, i.e. self-loops, left out): (face, kept edge i).
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> dual(F);
    for (uint32_t i = 0; i < K; ++i) {
        const uint32_t a = face[2 * i], b = face[2 * i + 1];
        if (a == b) continue;
        dual[a].push_back({b, i});
        dual[b].push_back({a, i});
    }
    auto wcap = [&](uint32_t i) { return g.edges[kept[i]].cap; };
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> padj(n);  // primal kept adjacency (node, kept edge i)
    for (uint32_t i = 0; i < K; ++i) {
        const auto& e = g.edges[kept[i]];
        padj[e.u].push_back({e.v, i});
        padj[e.v].push_back({e.u, i});
    }

    std::vector<uint8_t> inset(n, 0);

    // ---- 3. dominoes: min-cost 3 edge-disjoint dual paths between the two faces of each edge.
    // Faces are independent: worker threads take them in any order with their own scratch and
    // write per-face buckets, merged below in face order, so the output never depends on threads.
    struct Scratch {
        std::vector<double> dist;
        std::vector<uint32_t> local, touched, bfs_mark, tm;
        std::vector<uint8_t> cyc, chord, inset;
        uint32_t bfs_tick = 0, tt = 0;
        std::set<Domino> seen_dom;  // per worker: skips the w computation of repeats
    };
    std::vector<std::vector<DomCand>> per_face(F);
    constexpr double kTheta = 4.0, kChord = 2.0;  // w = total - 3 < 1; chord x(E(A:B)) < 2
    auto gen = [&](uint32_t s, Scratch& sc) {
        pause_point();
        auto& dist = sc.dist;
        auto& local = sc.local;
        auto& touched = sc.touched;
        auto& cyc = sc.cyc;
        auto& chord = sc.chord;
        auto& bfs_mark = sc.bfs_mark;
        auto& tm = sc.tm;
        auto& bfs_tick = sc.bfs_tick;
        auto& tt = sc.tt;
        auto& inset = sc.inset;
        auto& seen_dom = sc.seen_dom;
        auto& doms = per_face[s];
        auto cutval = [&](const std::vector<uint32_t>& set) {  // exact cut value on the full support
            for (uint32_t v : set) inset[v] = 1;
            double c = 0;
            for (uint32_t v : set)
                for (uint32_t k = g.adj_start[v]; k < g.adj_start[v + 1]; ++k)
                    if (!inset[g.other(g.adj[k], v)]) c += g.edges[g.adj[k]].cap;
            for (uint32_t v : set) inset[v] = 0;
            return c;
        };


        // Region: faces within kTheta of s (bounded Dijkstra).
        std::vector<uint32_t> region;
        using QE = std::pair<double, uint32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
        dist[s] = 0;
        touched.push_back(s);
        pq.push({0, s});
        while (!pq.empty() && region.size() < 400) {  // ponytail: region cap; raise if thetas are missed
            auto [d, a] = pq.top();
            pq.pop();
            if (d > dist[a] || local[a] != kNone) continue;
            local[a] = static_cast<uint32_t>(region.size());
            region.push_back(a);
            for (auto [b, i] : dual[a]) {
                const double nd = d + wcap(i);
                if (nd < dist[b] && nd < kTheta) {
                    if (dist[b] == std::numeric_limits<double>::infinity()) touched.push_back(b);
                    dist[b] = nd;
                    pq.push({nd, b});
                }
            }
        }

        // Min-cost flow (successive shortest paths, Dijkstra with potentials on the small region).
        struct Arc { uint32_t to; int cap; double cost; uint32_t edge; };
        std::vector<Arc> arcs;
        std::vector<std::vector<uint32_t>> out_arcs(region.size());

            for (uint32_t a : region)
                for (auto [b, i] : dual[a]) {
                    if (local[b] == kNone || a > b) continue;  // each dual edge once
                    const uint32_t la = local[a], lb = local[b];
                    for (auto [x, y] : {std::pair{la, lb}, std::pair{lb, la}}) {
                        out_arcs[x].push_back(static_cast<uint32_t>(arcs.size()));
                        arcs.push_back({y, 1, wcap(i), i});
                        out_arcs[y].push_back(static_cast<uint32_t>(arcs.size()));
                        arcs.push_back({x, 0, -wcap(i), i});
                    }
                }
        for (uint32_t t : region) {  // targets: faces closer than kChord (x(E(A:B)) < 2 when w < 1)
            if (t <= s || dist[t] >= kChord) continue;
            for (size_t ai = 0; ai < arcs.size(); ++ai) arcs[ai].cap = ai % 2 == 0 ? 1 : 0;
            double total = 0;
            int sent = 0;
            const uint32_t R = static_cast<uint32_t>(region.size());
            std::vector<double> pot(R, 0.0);  // Johnson potentials: reduced costs stay >= 0
            while (sent < 3) {
                constexpr double inf = std::numeric_limits<double>::infinity();
                std::vector<double> d(R, inf);
                std::vector<uint32_t> via(R, kNone);
                d[local[s]] = 0;
                std::priority_queue<QE, std::vector<QE>, std::greater<>> q;
                q.push({0, local[s]});
                while (!q.empty()) {
                    auto [dx, x] = q.top();
                    q.pop();
                    if (dx > d[x]) continue;
                    for (uint32_t ai : out_arcs[x]) {
                        const auto& ar = arcs[ai];
                        if (ar.cap <= 0) continue;
                        const double nd = dx + std::max(0.0, ar.cost + pot[x] - pot[ar.to]);
                        if (nd < d[ar.to] - 1e-12) { d[ar.to] = nd; via[ar.to] = ai; q.push({nd, ar.to}); }
                    }
                }
                if (d[local[t]] == inf) break;
                for (uint32_t x = 0; x < R; ++x)
                    if (d[x] < inf) pot[x] += d[x];
                double path_cost = 0;
                for (uint32_t x = local[t]; x != local[s]; x = arcs[via[x] ^ 1].to) path_cost += arcs[via[x]].cost;
                total += path_cost;
                for (uint32_t x = local[t]; x != local[s];) {
                    const uint32_t ai = via[x];
                    arcs[ai].cap -= 1;
                    arcs[ai ^ 1].cap += 1;
                    x = arcs[ai ^ 1].to;
                }
                ++sent;
            }
            if (sent == 3 && total < kTheta - cfg.violation_tol) {
                // Net flow per dual edge -> 3 edge-disjoint s-t walks (lists of kept edges).
                std::vector<std::vector<std::pair<uint32_t, uint32_t>>> fl(region.size());  // (to, edge)
                std::unordered_map<uint32_t, int> net;  // edge -> +1 (low->high local id) / -1
                for (uint32_t ai = 0; ai < arcs.size(); ai += 2) {
                    const int used = arcs[ai ^ 1].cap;  // flow on the forward arc
                    if (!used) continue;
                    const uint32_t to = arcs[ai].to, from = arcs[ai ^ 1].to;
                    net[arcs[ai].edge] += from < to ? 1 : -1;
                }
                for (uint32_t ai = 0; ai < arcs.size(); ai += 2) {
                    if (!arcs[ai ^ 1].cap) continue;
                    const uint32_t to = arcs[ai].to, from = arcs[ai ^ 1].to;
                    const int dir = from < to ? 1 : -1;
                    auto it = net.find(arcs[ai].edge);
                    if (it == net.end() || it->second != dir) continue;
                    it->second = 0;  // keep one arc per edge with net flow
                    fl[from].push_back({to, arcs[ai].edge});
                }
                std::vector<std::vector<uint32_t>> paths;
                for (int p = 0; p < 3; ++p) {
                    std::vector<uint32_t> path;
                    uint32_t x = local[s];
                    for (uint32_t guard = 0; x != local[t] && guard < 4 * arcs.size() + 4; ++guard) {
                        if (fl[x].empty()) { path.clear(); break; }
                        auto [y, e] = fl[x].back();
                        fl[x].pop_back();
                        path.push_back(e);
                        x = y;
                    }
                    if (x != local[t] || path.empty()) break;
                    paths.push_back(std::move(path));
                }
                for (size_t c = 0; paths.size() == 3 && c < 3; ++c) {
                    for (size_t p = 0; p < 3; ++p)
                        for (uint32_t e : paths[p]) (p == c ? chord : cyc)[e] ^= 1;
                    // T: primal BFS from a chord endpoint without crossing the cycle edges.
                    const uint32_t u0 = g.edges[kept[paths[c][0]]].u;
                    auto bfs = [&](uint32_t start, const std::vector<uint8_t>& block, const std::vector<uint32_t>* within_mark,
                                   uint32_t within_tick, uint32_t cap, std::vector<uint32_t>& got) {
                        got.clear();
                        const uint32_t tk = ++bfs_tick;
                        bfs_mark[start] = tk;
                        got.push_back(start);
                        for (size_t q = 0; q < got.size(); ++q) {
                            if (got.size() > cap) return false;
                            for (auto [w, i] : padj[got[q]]) {
                                if (block[i] || bfs_mark[w] == tk) continue;
                                if (within_mark && (*within_mark)[w] != within_tick) continue;
                                bfs_mark[w] = tk;
                                got.push_back(w);
                            }
                        }
                        return true;
                    };
                    std::vector<uint32_t> T, A;
                    bool ok = bfs(u0, cyc, nullptr, 0, cfg.max_tooth, T) && T.size() < n;
                    uint32_t ttick = bfs_tick;
                    if (ok)
                        for (uint32_t e : paths[c]) {
                            const auto& ed = g.edges[kept[e]];
                            if (bfs_mark[ed.u] != ttick || bfs_mark[ed.v] != ttick) { ok = false; break; }
                        }
                    if (ok) {
                        ++tt;  // T's own marks: the next BFS reuses bfs_mark
                        for (uint32_t v : T) tm[v] = tt;
                        ok = bfs(u0, chord, &tm, tt, cfg.max_tooth, A) && A.size() < T.size();
                        if (ok) {
                            std::vector<uint32_t> B;
                            const uint32_t atick = bfs_tick;
                            for (uint32_t v : T)
                                if (bfs_mark[v] != atick) B.push_back(v);
                            std::sort(A.begin(), A.end());
                            std::sort(B.begin(), B.end());
                            Domino d{A, B};
                            if (d.b < d.a) std::swap(d.a, d.b);
                            if (!seen_dom.count(d)) {
                                seen_dom.insert(d);
                                std::vector<uint32_t> Ts = T;
                                std::sort(Ts.begin(), Ts.end());
                                const double w = std::max(0.0, (cutval(d.a) + cutval(d.b) + cutval(Ts)) / 2 - 3);
                                if (w < 1 - cfg.violation_tol) {
                                    // Dual endpoints: faces of odd degree in E(A:B) over kept edges.
                                    for (uint32_t v : d.a) inset[v] = 1;
                                    for (uint32_t v : d.b) inset[v] = 2;
                                    std::unordered_map<uint32_t, int> deg;
                                    for (uint32_t v : d.a)
                                        for (auto [wv, i] : padj[v])
                                            if (inset[wv] == 2) { deg[face[2 * i]] ^= 1; deg[face[2 * i + 1]] ^= 1; }
                                    for (uint32_t v : d.a) inset[v] = 0;
                                    for (uint32_t v : d.b) inset[v] = 0;
                                    std::vector<uint32_t> odd;
                                    for (auto [fc, p] : deg)
                                        if (p) odd.push_back(fc);
                                    std::sort(odd.begin(), odd.end());
                                    if (odd.size() == 2) doms.push_back({d, odd[0], odd[1], w});
                                    else if (odd.empty()) doms.push_back({d, s, s, w});
                                }
                            }
                        }
                    }
                    for (size_t p = 0; p < 3; ++p)
                        for (uint32_t e : paths[p]) (p == c ? chord : cyc)[e] = 0;
                }
            }
        }
        for (uint32_t a : touched) { dist[a] = std::numeric_limits<double>::infinity(); local[a] = kNone; }
        touched.clear();
    };
    const uint32_t W = std::max<uint32_t>(1, std::min<uint32_t>(cfg.threads, F));
    std::atomic<uint32_t> next_face{0};
    auto worker = [&] {
        Scratch sc{std::vector<double>(F, std::numeric_limits<double>::infinity()), std::vector<uint32_t>(F, kNone), {},
                   std::vector<uint32_t>(n, 0), std::vector<uint32_t>(n, 0), std::vector<uint8_t>(K, 0),
                   std::vector<uint8_t>(K, 0), std::vector<uint8_t>(n, 0), 0, 0, {}};
        for (uint32_t s; (s = next_face.fetch_add(1, std::memory_order_relaxed)) < F;) gen(s, sc);
    };
    if (W == 1) worker();
    else {
        std::vector<std::thread> pool;
        for (uint32_t w = 0; w < W; ++w) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    std::vector<DomCand> doms;
    std::set<Domino> seen_dom;
    for (auto& bucket : per_face)
        for (auto& c : bucket)
            if (seen_dom.insert(c.d).second) doms.push_back(std::move(c));
    per_face = {};
    st.dominoes = static_cast<uint32_t>(doms.size());

    // ---- 4. minimum odd closed walks (< 1) in dual + domino edges, from every domino endpoint.
    struct AEdge { uint32_t to; double w; uint8_t odd; uint32_t id; };  // id: kept edge, or domino
    std::vector<std::vector<AEdge>> aug(F);
    for (uint32_t a = 0; a < F; ++a)
        for (auto [b, i] : dual[a]) aug[a].push_back({b, wcap(i), 0, i});
    std::vector<uint8_t> is_start(F, 0);
    for (uint32_t j = 0; j < doms.size(); ++j) {
        aug[doms[j].f].push_back({doms[j].g, doms[j].w, 1, j});
        if (doms[j].f != doms[j].g) aug[doms[j].g].push_back({doms[j].f, doms[j].w, 1, j});
        is_start[doms[j].f] = is_start[doms[j].g] = 1;
    }
    std::set<std::pair<std::vector<uint32_t>, std::vector<Domino>>> seen_cut;
    std::vector<double> d2(2 * F, std::numeric_limits<double>::infinity());
    std::vector<std::pair<uint32_t, uint32_t>> pred(2 * F);  // (prev state, index into aug[prev node])
    std::vector<uint32_t> t2;
    std::vector<uint32_t> comp(n, kNone);
    for (uint32_t s = 0; s < F; ++s) {
        pause_point();
        if (!is_start[s]) continue;
        using QE = std::pair<double, uint32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
        d2[2 * s] = 0;
        t2.push_back(2 * s);
        pq.push({0, 2 * s});
        const double limit = 1 - cfg.violation_tol;
        while (!pq.empty()) {
            auto [d, x] = pq.top();
            pq.pop();
            if (d > d2[x]) continue;
            if (x == 2 * s + 1) break;
            const uint32_t a = x >> 1, par = x & 1;
            for (uint32_t k = 0; k < aug[a].size(); ++k) {
                const auto& ae = aug[a][k];
                const uint32_t y = 2 * ae.to + (par ^ ae.odd);
                const double nd = d + ae.w;
                if (nd < limit && nd < d2[y]) {
                    if (d2[y] == std::numeric_limits<double>::infinity()) t2.push_back(y);
                    d2[y] = nd;
                    pred[y] = {x, k};
                    pq.push({nd, y});
                }
            }
        }
        if (d2[2 * s + 1] < limit) {
            ++st.cycles;
            // Odd-multiplicity edges and dominoes of the walk.
            std::unordered_map<uint32_t, int> fe, fd;
            for (uint32_t y = 2 * s + 1; y != 2 * s;) {
                const auto [x, k] = pred[y];
                const auto& ae = aug[x >> 1][k];
                (ae.odd ? fd : fe)[ae.id] ^= 1;
                y = x;
            }
            std::vector<uint32_t> J;
            for (auto [j, p] : fd)
                if (p) J.push_back(j);
            std::sort(J.begin(), J.end());
            if (J.size() % 2 == 1) {
                // Labels on kept edges: [in F] xor (number of chosen dominoes with the edge in E(A:B)).
                std::unordered_map<uint32_t, uint8_t> label;
                for (auto [i, p] : fe)
                    if (p) label[i] ^= 1;
                for (uint32_t j : J) {
                    for (uint32_t v : doms[j].d.a) inset[v] = 1;
                    for (uint32_t v : doms[j].d.b) inset[v] = 2;
                    for (uint32_t v : doms[j].d.a)
                        for (auto [wv, i] : padj[v])
                            if (inset[wv] == 2) label[i] ^= 1;
                    for (uint32_t v : doms[j].d.a) inset[v] = 0;
                    for (uint32_t v : doms[j].d.b) inset[v] = 0;
                }
                // S: components of the kept graph without labelled edges; labelled edges join
                // differently coloured components. Components past the cap are "big" (one id).
                std::vector<uint8_t> blocked(K, 0);
                std::vector<uint32_t> ends;
                for (auto [i, p] : label)
                    if (p) { blocked[i] = 1; ends.push_back(g.edges[kept[i]].u); ends.push_back(g.edges[kept[i]].v); }
                const uint32_t kBig = kNone - 1, cap = std::max<uint32_t>(cfg.max_tooth, 64);
                std::vector<uint32_t> compsz, visited;
                std::vector<std::vector<uint32_t>> members;
                for (uint32_t v : ends) {
                    if (comp[v] != kNone) continue;
                    std::vector<uint32_t> got{v};
                    comp[v] = static_cast<uint32_t>(members.size());
                    bool big = false;
                    for (size_t q = 0; q < got.size() && !big; ++q)
                        for (auto [w, i] : padj[got[q]]) {
                            if (blocked[i] || comp[w] != kNone) continue;
                            comp[w] = comp[v];
                            got.push_back(w);
                            if (got.size() > cap) { big = true; break; }
                        }
                    visited.insert(visited.end(), got.begin(), got.end());
                    if (big) for (uint32_t w : got) comp[w] = kBig;
                    members.push_back(big ? std::vector<uint32_t>{} : std::move(got));
                }
                // 2-colour the component graph (big = colour 0).
                const uint32_t C = static_cast<uint32_t>(members.size());
                std::vector<int> colour(C + 1, -1);  // index C = big
                auto cid = [&](uint32_t v) { return comp[v] == kBig ? C : comp[v]; };
                std::vector<std::vector<uint32_t>> cadj(C + 1);
                for (auto [i, p] : label) {
                    if (!p) continue;
                    const uint32_t a = cid(g.edges[kept[i]].u), b = cid(g.edges[kept[i]].v);
                    cadj[a].push_back(b);
                    cadj[b].push_back(a);
                }
                bool ok = true;
                colour[C] = 0;
                for (uint32_t r = 0; r <= C && ok; ++r) {
                    if (colour[r] < 0) colour[r] = 0;
                    std::vector<uint32_t> stack{r};
                    while (!stack.empty() && ok) {
                        const uint32_t a = stack.back();
                        stack.pop_back();
                        for (uint32_t b : cadj[a]) {
                            if (colour[b] < 0) { colour[b] = 1 - colour[a]; stack.push_back(b); }
                            else if (colour[b] == colour[a]) { ok = false; break; }
                        }
                    }
                }
                std::vector<uint32_t> S;
                if (ok)
                    for (uint32_t c = 0; c < C; ++c)
                        if (colour[c] == 1) S.insert(S.end(), members[c].begin(), members[c].end());
                for (uint32_t v : visited) comp[v] = kNone;
                if (!ok) ++st.coloring_failed;
                else {
                    std::sort(S.begin(), S.end());
                    std::vector<Domino> D;
                    for (uint32_t j : J) D.push_back(doms[j].d);
                    std::sort(D.begin(), D.end());
                    if (dp_valid(n, D) && seen_cut.insert({S, D}).second) {
                        const double v = dp_violation(g, S, D);  // exact on the full support
                        if (v > cfg.violation_tol) out.push_back({std::move(S), std::move(D), v});
                    }
                }
            }
        }
        for (uint32_t x : t2) d2[x] = std::numeric_limits<double>::infinity();
        t2.clear();
    }
    std::stable_sort(out.begin(), out.end(), [](const DpCut& a, const DpCut& b) { return a.violation > b.violation; });
    st.cuts = static_cast<uint32_t>(out.size());
    if (stats) *stats = st;
    return out;
}

}  // namespace apex
