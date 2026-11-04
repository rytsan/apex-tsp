#include "cuts/pool.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace apex {
namespace {

void fnv(uint64_t& h, uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= (v >> (8 * b)) & 0xFF;
        h *= 1099511628211ull;
    }
}

bool same(const PoolCut& a, const PoolCut& b) { return a.kind == b.kind && a.nodes == b.nodes && a.teeth == b.teeth; }

bool canonical_less(const PoolCut& a, const PoolCut& b) {
    return std::tie(a.kind, a.nodes, a.teeth) < std::tie(b.kind, b.nodes, b.teeth);
}

}  // namespace

uint64_t CutPool::hash_of(const PoolCut& c) {
    uint64_t h = 1469598103934665603ull;
    fnv(h, static_cast<uint64_t>(c.kind));
    fnv(h, c.nodes.size());
    for (uint32_t v : c.nodes) fnv(h, v);
    for (const auto& [u, v] : c.teeth) fnv(h, (uint64_t(u) << 32) | v);
    return h;
}

void CutPool::stage_sec(const SecCut& c) {
    PoolCut p{CutKind::Sec, c.nodes, {}, 0, 0, -1, 0};
    std::sort(p.nodes.begin(), p.nodes.end());
    p.hash = hash_of(p);
    std::lock_guard<std::mutex> lock(mu_);
    staged_.push_back(std::move(p));
}

void CutPool::stage_blossom(const BlossomCut& c) {
    PoolCut p{CutKind::Blossom, c.handle, {}, 0, 0, -1, 0};
    std::sort(p.nodes.begin(), p.nodes.end());
    for (const auto& t : c.teeth) p.teeth.push_back({std::min(t.u, t.v), std::max(t.u, t.v)});
    std::sort(p.teeth.begin(), p.teeth.end());
    p.hash = hash_of(p);
    std::lock_guard<std::mutex> lock(mu_);
    staged_.push_back(std::move(p));
}

bool CutPool::insert(PoolCut&& c) {
    auto& bucket = by_hash_[c.hash];
    for (size_t i : bucket)
        if (same(cuts_[i], c)) { ++stats_.duplicates; return false; }
    c.id = next_id_++;
    bucket.push_back(cuts_.size());
    cuts_.push_back(std::move(c));
    ++stats_.inserted;
    return true;
}

std::vector<size_t> CutPool::commit() {
    std::vector<PoolCut> batch;
    {
        std::lock_guard<std::mutex> lock(mu_);
        batch.swap(staged_);
    }
    std::sort(batch.begin(), batch.end(), canonical_less);  // deterministic merge order
    std::vector<size_t> added;
    for (auto& c : batch) {
        const size_t idx = cuts_.size();
        if (insert(std::move(c))) added.push_back(idx);
    }
    return added;
}

Row CutPool::make_row(const PoolCut& c, const std::vector<Edge>& lp_edges) const {
    uint32_t n = 0;
    for (const auto& e : lp_edges) n = std::max(n, std::max(e.u, e.v) + 1);
    for (uint32_t v : c.nodes) n = std::max(n, v + 1);
    std::vector<uint8_t> in(n, 0);
    for (uint32_t v : c.nodes) in[v] = 1;
    Row r;
    r.sense = Sense::Ge;
    r.rhs = c.kind == CutKind::Sec ? 2.0 : 1.0 - static_cast<double>(c.teeth.size());
    for (uint32_t j = 0; j < lp_edges.size(); ++j) {
        const auto& e = lp_edges[j];
        if (in[e.u] == in[e.v]) continue;
        double coef = 1.0;
        if (c.kind == CutKind::Blossom &&
            std::binary_search(c.teeth.begin(), c.teeth.end(), std::make_pair(std::min(e.u, e.v), std::max(e.u, e.v))))
            coef = -1.0;
        r.entries.push_back({j, coef});
    }
    return r;
}

double CutPool::violation(const PoolCut& c, const std::vector<Edge>& lp_edges, const std::vector<double>& x) const {
    const Row r = make_row(c, lp_edges);
    double lhs = 0;
    for (const auto& e : r.entries) lhs += e.value * x[e.index];
    return r.rhs - lhs;
}

std::vector<size_t> CutPool::age(const std::vector<double>& duals, double tol) {
    std::vector<size_t> out;
    for (size_t i = 0; i < cuts_.size(); ++i) {
        auto& c = cuts_[i];
        if (c.lp_row < 0) continue;
        if (std::abs(duals[c.lp_row]) <= tol) {
            if (++c.inactive_rounds >= age_limit_) out.push_back(i);
        } else {
            c.inactive_rounds = 0;
        }
    }
    stats_.aged_out += out.size();
    return out;
}

std::vector<size_t> CutPool::violated_outside(const std::vector<Edge>& lp_edges, const std::vector<double>& x,
                                              double tol) const {
    std::vector<std::pair<double, size_t>> v;
    for (size_t i = 0; i < cuts_.size(); ++i) {
        if (cuts_[i].lp_row >= 0) continue;
        const double viol = violation(cuts_[i], lp_edges, x);
        if (viol > tol) v.push_back({-viol, i});
    }
    std::sort(v.begin(), v.end());
    std::vector<size_t> out;
    for (const auto& p : v) out.push_back(p.second);
    return out;
}

PoolStats CutPool::stats() const {
    PoolStats s = stats_;
    s.size = cuts_.size();
    s.in_lp = static_cast<size_t>(std::count_if(cuts_.begin(), cuts_.end(), [](const PoolCut& c) { return c.lp_row >= 0; }));
    return s;
}

}  // namespace apex
