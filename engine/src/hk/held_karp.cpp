#include "hk/held_karp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace apex {

namespace {

struct Dsu {
    std::vector<uint32_t> p;
    explicit Dsu(uint32_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t v) {
        while (p[v] != v) v = p[v] = p[p[v]];
        return v;
    }
};

// Candidate-graph 1-tree: value with multipliers and degrees. Returns false if the graph minus
// node 0 is disconnected or node 0 has fewer than two candidate edges.
bool one_tree_candidate(const CandidateGraph& g, const std::vector<double>& pi, std::vector<uint32_t>& order,
                        std::vector<double>& w, std::vector<int32_t>& deg, double& value) {
    const uint32_t n = g.n;
    const size_t m = g.edges.size();
    w.resize(m);
    for (size_t e = 0; e < m; ++e) w[e] = g.edges[e].cost + pi[g.edges[e].u] + pi[g.edges[e].v];
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return w[a] < w[b] || (w[a] == w[b] && a < b); });
    std::fill(deg.begin(), deg.end(), 0);
    Dsu dsu(n);
    double total = 0;
    uint32_t joined = 0, at0 = 0;
    double best0[2] = {0, 0};
    for (uint32_t e : order) {
        const uint32_t u = g.edges[e].u, v = g.edges[e].v;
        if (u == 0 || v == 0) {
            if (at0 < 2) {
                best0[at0++] = w[e];
                ++deg[u];
                ++deg[v];
            }
            continue;
        }
        if (joined + 2 == n) continue;
        const uint32_t a = dsu.find(u), b = dsu.find(v);
        if (a == b) continue;
        dsu.p[a] = b;
        total += w[e];
        ++deg[u];
        ++deg[v];
        ++joined;
    }
    if (joined + 2 != n || at0 < 2) return false;
    double sum_pi = 0;
    for (double p : pi) sum_pi += p;
    value = total + best0[0] + best0[1] - 2 * sum_pi;
    return true;
}

}  // namespace

double one_tree_bound_complete(const Instance& inst, const std::vector<double>& pi) {
    const uint32_t n = inst.n;
    if (n < 3) return 0;
    const bool ceil_metric = inst.metric == Metric::Ceil2D;
    auto metric = [&](double dx, double dy) {
        const double r = std::sqrt(dx * dx + dy * dy);
        return ceil_metric ? std::ceil(r) : std::floor(r + 0.5);
    };
    // Remaining nodes 2..n-1 in dense arrays (swap-remove): key = min over tree u of d(u,v) + pi_u.
    const uint32_t r0 = n - 2;
    std::vector<double> x(r0), y(r0), p(r0), key(r0, std::numeric_limits<double>::infinity());
    for (uint32_t k = 0; k < r0; ++k) { x[k] = inst.xs[k + 2]; y[k] = inst.ys[k + 2]; p[k] = pi[k + 2]; }
    long double total = 0;
    double ux = inst.xs[1], uy = inst.ys[1], upi = pi[1];
    for (uint32_t r = r0; r > 0; --r) {
        uint32_t arg = 0;
        double best = std::numeric_limits<double>::infinity();
        for (uint32_t k = 0; k < r; ++k) {
            const double c = metric(ux - x[k], uy - y[k]) + upi;
            key[k] = std::min(key[k], c);
            const double kv = key[k] + p[k];
            if (kv < best) { best = kv; arg = k; }
        }
        total += best;
        ux = x[arg]; uy = y[arg]; upi = p[arg];
        x[arg] = x[r - 1]; y[arg] = y[r - 1]; p[arg] = p[r - 1]; key[arg] = key[r - 1];
    }
    double a = std::numeric_limits<double>::infinity(), b = a;  // two cheapest edges at node 0
    for (uint32_t v = 1; v < n; ++v) {
        const double c = metric(inst.xs[0] - inst.xs[v], inst.ys[0] - inst.ys[v]) + pi[0] + pi[v];
        if (c < a) { b = a; a = c; } else if (c < b) b = c;
    }
    double sum_pi = 0;
    for (double q : pi) sum_pi += q;
    return static_cast<double>(total + a + b - 2 * static_cast<long double>(sum_pi));
}

HkResult held_karp(const Instance& inst, const CandidateGraph& g, int64_t ub, const HkConfig& cfg) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const uint32_t n = inst.n;
    HkResult res;
    res.pi.assign(n, 0.0);
    res.lb_candidate = -std::numeric_limits<double>::infinity();
    std::vector<double> pi(n, 0.0), w;
    std::vector<uint32_t> order(g.edges.size());
    std::iota(order.begin(), order.end(), 0u);
    std::vector<int32_t> deg(n);
    double lambda = cfg.lambda0;
    uint32_t stall = 0;
    for (; res.iterations < cfg.max_iterations; ++res.iterations) {
        if (cfg.time_limit_s > 0 && std::chrono::duration<double>(Clock::now() - t0).count() > cfg.time_limit_s) break;
        double L = 0;
        if (!one_tree_candidate(g, pi, order, w, deg, L)) break;
        double norm = 0;
        for (uint32_t v = 0; v < n; ++v) norm += double(deg[v] - 2) * double(deg[v] - 2);
        if (L > res.lb_candidate + 1e-9) {
            res.lb_candidate = L;
            res.pi = pi;
            res.one_tree_is_tour = norm == 0;
            stall = 0;
        } else if (++stall >= cfg.patience) {
            lambda *= 0.5;
            stall = 0;
        }
        if (norm == 0 || lambda < cfg.lambda_min) break;
        const double t = lambda * std::max(1.0, static_cast<double>(ub) - L) / norm;
        for (uint32_t v = 0; v < n; ++v) pi[v] += t * (deg[v] - 2);
    }
    res.ascent_s = std::chrono::duration<double>(Clock::now() - t0).count();
    if (cfg.exact) {
        const auto t1 = Clock::now();
        res.lb = one_tree_bound_complete(inst, res.pi);
        res.lb_int = static_cast<int64_t>(std::ceil(res.lb - 1e-6));
        res.valid = true;
        res.exact_s = std::chrono::duration<double>(Clock::now() - t1).count();
    }
    return res;
}

}  // namespace apex
