#include "cuts/pool.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "core/serial.hpp"

namespace apex {
namespace {

void fnv(uint64_t& h, uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= (v >> (8 * b)) & 0xFF;
        h *= 1099511628211ull;
    }
}

bool same(const PoolCut& a, const PoolCut& b) { return a.kind == b.kind && a.nodes == b.nodes && a.teeth == b.teeth && a.sets == b.sets; }

bool canonical_less(const PoolCut& a, const PoolCut& b) {
    return std::tie(a.kind, a.nodes, a.teeth, a.sets) < std::tie(b.kind, b.nodes, b.teeth, b.sets);
}

}  // namespace

uint64_t CutPool::hash_of(const PoolCut& c) {
    uint64_t h = 1469598103934665603ull;
    fnv(h, static_cast<uint64_t>(c.kind));
    fnv(h, c.nodes.size());
    for (uint32_t v : c.nodes) fnv(h, v);
    for (const auto& [u, v] : c.teeth) fnv(h, (uint64_t(u) << 32) | v);
    for (const auto& s : c.sets) {  // empty for SEC / blossom: their hashes are unchanged
        fnv(h, s.size());
        for (uint32_t v : s) fnv(h, v);
    }
    return h;
}

void CutPool::stage_sec(const SecCut& c) {
    PoolCut p{CutKind::Sec, c.nodes, {}, 0, 0, -1, 0, {}};
    std::sort(p.nodes.begin(), p.nodes.end());
    p.hash = hash_of(p);
    std::lock_guard<std::mutex> lock(mu_);
    staged_.push_back(std::move(p));
}

void CutPool::stage_blossom(const BlossomCut& c) {
    PoolCut p{CutKind::Blossom, c.handle, {}, 0, 0, -1, 0, {}};
    std::sort(p.nodes.begin(), p.nodes.end());
    for (const auto& t : c.teeth) p.teeth.push_back({std::min(t.u, t.v), std::max(t.u, t.v)});
    std::sort(p.teeth.begin(), p.teeth.end());
    p.hash = hash_of(p);
    std::lock_guard<std::mutex> lock(mu_);
    staged_.push_back(std::move(p));
}

void CutPool::stage_comb(const CombCut& c) {
    PoolCut p{CutKind::Comb, c.handle, {}, 0, 0, -1, 0, c.teeth};
    std::sort(p.nodes.begin(), p.nodes.end());
    for (auto& s : p.sets) std::sort(s.begin(), s.end());
    std::sort(p.sets.begin(), p.sets.end());
    p.hash = hash_of(p);
    std::lock_guard<std::mutex> lock(mu_);
    staged_.push_back(std::move(p));
}

void CutPool::stage_dp(const DpCut& c) {
    PoolCut p{CutKind::Domino, c.handle, {}, 0, 0, -1, 0, {}};
    std::sort(p.nodes.begin(), p.nodes.end());
    auto dom = c.dominoes;
    std::sort(dom.begin(), dom.end());
    for (auto& d : dom) { p.sets.push_back(d.a); p.sets.push_back(d.b); }
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
    c.for_each_set([&](const std::vector<uint32_t>& s) { for (uint32_t v : s) n = std::max(n, v + 1); });
    std::vector<uint8_t> in(n, 0);
    for (uint32_t v : c.nodes) in[v] = 1;
    Row r;
    r.sense = Sense::Ge;
    if (c.kind == CutKind::Domino) {
        std::vector<Domino> dom;
        for (size_t k = 0; k + 1 < c.sets.size(); k += 2) dom.push_back({c.sets[k], c.sets[k + 1]});
        const DpMembership mem(n, c.nodes, dom);
        r.rhs = 3.0 * static_cast<double>(dom.size()) + 1.0;
        for (uint32_t j = 0; j < lp_edges.size(); ++j)
            if (const int coef = mem.coef(lp_edges[j].u, lp_edges[j].v)) r.entries.push_back({j, static_cast<double>(coef)});
        return r;
    }
    if (c.kind == CutKind::Comb) {
        std::vector<uint32_t> tooth(n, 0);  // 1 + tooth index; teeth are disjoint
        for (uint32_t k = 0; k < c.sets.size(); ++k)
            for (uint32_t v : c.sets[k]) tooth[v] = k + 1;
        r.rhs = 3.0 * static_cast<double>(c.sets.size()) + 1.0;
        for (uint32_t j = 0; j < lp_edges.size(); ++j) {
            const auto& e = lp_edges[j];
            int coef = in[e.u] != in[e.v];
            if (tooth[e.u] != tooth[e.v]) coef += (tooth[e.u] != 0) + (tooth[e.v] != 0);
            if (coef) r.entries.push_back({j, static_cast<double>(coef)});
        }
        return r;
    }
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

size_t CutPool::add_or_find(const PoolCut& c) {
    for (size_t i : by_hash_[c.hash])
        if (same(cuts_[i], c)) return i;
    PoolCut copy = c;
    copy.lp_row = -1;
    copy.inactive_rounds = 0;
    const size_t idx = cuts_.size();
    insert(std::move(copy));
    return idx;
}

void CutPool::save(std::ostream& o) const {
    using namespace serial;
    tag(o, "pool");
    put(o, age_limit_);
    put(o, next_id_);
    put(o, stats_.inserted);
    put(o, stats_.duplicates);
    put(o, stats_.aged_out);
    put(o, stats_.reactivated);
    put(o, static_cast<uint64_t>(cuts_.size()));
    o << '\n';
    for (const auto& c : cuts_) {
        put(o, static_cast<uint8_t>(c.kind));
        put(o, c.hash);
        put(o, c.id);
        put(o, c.lp_row);
        put(o, c.inactive_rounds);
        put_vec(o, c.nodes);
        put(o, static_cast<uint64_t>(c.teeth.size()));
        for (const auto& [u, v] : c.teeth) { put(o, u); put(o, v); }
        if (c.kind == CutKind::Comb || c.kind == CutKind::Domino) {  // only these carry sets: older checkpoints load unchanged
            put(o, static_cast<uint64_t>(c.sets.size()));
            for (const auto& s : c.sets) put_vec(o, s);
        }
        o << '\n';
    }
}

void CutPool::load(std::istream& in) {
    using namespace serial;
    expect(in, "pool");
    age_limit_ = get<uint32_t>(in);
    next_id_ = get<uint64_t>(in);
    stats_ = PoolStats{};
    stats_.inserted = get<uint64_t>(in);
    stats_.duplicates = get<uint64_t>(in);
    stats_.aged_out = get<uint64_t>(in);
    stats_.reactivated = get<uint64_t>(in);
    const auto n = get<uint64_t>(in);
    cuts_.clear();
    by_hash_.clear();
    for (uint64_t k = 0; k < n; ++k) {
        PoolCut c;
        c.kind = static_cast<CutKind>(get<uint8_t>(in));
        c.hash = get<uint64_t>(in);
        c.id = get<uint64_t>(in);
        c.lp_row = get<int32_t>(in);
        c.inactive_rounds = get<uint32_t>(in);
        c.nodes = get_vec<uint32_t>(in);
        const auto t = get<uint64_t>(in);
        for (uint64_t i = 0; i < t; ++i) {
            const auto u = get<uint32_t>(in);
            c.teeth.push_back({u, get<uint32_t>(in)});
        }
        if (c.kind == CutKind::Comb || c.kind == CutKind::Domino) {
            const auto k = get<uint64_t>(in);
            for (uint64_t i = 0; i < k; ++i) c.sets.push_back(get_vec<uint32_t>(in));
        }
        if (hash_of(c) != c.hash) throw std::runtime_error("checkpoint: pool cut hash mismatch");
        by_hash_[c.hash].push_back(cuts_.size());
        cuts_.push_back(std::move(c));
    }
}

PoolStats CutPool::stats() const {
    PoolStats s = stats_;
    s.size = cuts_.size();
    s.in_lp = static_cast<size_t>(std::count_if(cuts_.begin(), cuts_.end(), [](const PoolCut& c) { return c.lp_row >= 0; }));
    return s;
}

}  // namespace apex
