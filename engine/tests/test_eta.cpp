#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "lp/eta.hpp"

using apex::BasisFactor;
using apex::RefactorScheduler;
using apex::SparseCol;

namespace {

SparseCol random_col(std::mt19937_64& rng, uint32_t m, uint32_t forced_row) {
    std::uniform_real_distribution<double> U(0.5, 2.0);
    const uint32_t kind = rng() % 10;
    if (kind < 4) return {{forced_row, (rng() & 1) ? 1.0 : -1.0}};
    if (kind < 9) return {{forced_row, 1.0}, {static_cast<uint32_t>(rng() % m), 1.0}};
    return {{forced_row, U(rng)}, {static_cast<uint32_t>(rng() % m), -U(rng)}, {static_cast<uint32_t>(rng() % m), U(rng)}};
}

std::vector<SparseCol> tsp_like_basis(std::mt19937_64& rng, uint32_t m) {
    std::vector<uint32_t> perm(m);
    for (uint32_t i = 0; i < m; ++i) perm[i] = i;
    for (uint32_t i = m - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
    std::vector<SparseCol> cols(m);
    for (uint32_t j = 0; j < m; ++j) cols[j] = random_col(rng, m, perm[j]);
    return cols;
}

std::vector<double> mul(const std::vector<SparseCol>& B, const std::vector<double>& x) {
    std::vector<double> y(B.size(), 0.0);
    for (size_t j = 0; j < B.size(); ++j)
        for (const auto& e : B[j]) y[e.index] += e.value * x[j];
    return y;
}

std::vector<double> mul_t(const std::vector<SparseCol>& B, const std::vector<double>& y) {
    std::vector<double> x(B.size(), 0.0);
    for (size_t j = 0; j < B.size(); ++j)
        for (const auto& e : B[j]) x[j] += e.value * y[e.index];
    return x;
}

double rel_residual(const std::vector<double>& lhs, const std::vector<double>& b) {
    double num = 0, den = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        num += (lhs[i] - b[i]) * (lhs[i] - b[i]);
        den += b[i] * b[i];
    }
    return std::sqrt(num / den);
}

}  // namespace

TEST_CASE("T_eta_accumulated_residual") {
    std::mt19937_64 rng(2025);
    std::normal_distribution<double> N(0, 1);
    double worst_f = 0, worst_b = 0;
    int bases = 0, tries = 0;
    while (bases < 10) {
        REQUIRE(++tries < 200);
        const uint32_t m = bases % 2 ? 500 : 2000;
        auto B = tsp_like_basis(rng, m);
        BasisFactor f(1000000, apex::BasisUpdateKind::Pfi);  // never refactor: measure pure eta accumulation
        if (!f.factorize(m, B)) continue;
        ++bases;
        for (int upd = 0; upd < 64; ++upd) {
            // New column; leave position = largest |aq_r| among 20 random positions (stable pivot).
            const auto a = random_col(rng, m, static_cast<uint32_t>(rng() % m));
            std::vector<double> aq(m, 0.0);
            for (const auto& e : a) aq[e.index] += e.value;
            f.ftran(aq);
            uint32_t r = 0;
            double best = 0;
            for (int s = 0; s < 20; ++s) {
                const uint32_t c = static_cast<uint32_t>(rng() % m);
                if (std::abs(aq[c]) > best) { best = std::abs(aq[c]); r = c; }
            }
            if (best < 1e-3) { --upd; continue; }
            f.update(r, aq);
            B[r] = a;
        }
        REQUIRE(f.eta().size() == 64);
        std::vector<double> xt(m), yt(m);
        for (auto& v : xt) v = N(rng);
        for (auto& v : yt) v = N(rng);
        const auto b = mul(B, xt);
        auto x = b;
        f.ftran(x);
        worst_f = std::max(worst_f, rel_residual(mul(B, x), b));
        const auto d = mul_t(B, yt);
        auto y = d;
        f.btran(y);
        worst_b = std::max(worst_b, rel_residual(mul_t(B, y), d));
    }
    MESSAGE("64 updates x 10 bases: worst_ftran=" << worst_f << " worst_btran=" << worst_b);
    CHECK(worst_f <= 1e-6);
    CHECK(worst_b <= 1e-6);
}

TEST_CASE("T_eta_refact_schedule") {
    SUBCASE("iteration trigger") {
        RefactorScheduler s(100, 2.0);
        s.on_refactor(1000000);
        for (int i = 0; i < 99; ++i) { s.on_update(); CHECK_FALSE(s.should_refactor(10)); }
        s.on_update();
        CHECK(s.should_refactor(10));  // 100th update
        s.on_refactor(1000000);
        CHECK_FALSE(s.should_refactor(10));
    }
    SUBCASE("fill trigger") {
        RefactorScheduler s(100, 2.0);
        s.on_refactor(500);
        s.on_update();
        CHECK_FALSE(s.should_refactor(999));
        CHECK(s.should_refactor(1000));  // eta nnz reached 2x LU nnz
    }
    SUBCASE("through BasisFactor") {
        std::mt19937_64 rng(3);
        const uint32_t m = 50;
        std::vector<SparseCol> I(m);
        for (uint32_t i = 0; i < m; ++i) I[i] = {{i, 1.0}};  // LU nnz = 50
        BasisFactor f(100, apex::BasisUpdateKind::Pfi);
        REQUIRE(f.factorize(m, I));
        int updates = 0;
        while (!f.needs_refactor()) {  // dense etas: ~m nnz each => fill trigger after ~2 updates
            std::vector<double> aq(m, 1.0);
            f.update(static_cast<uint32_t>(rng() % m), aq);
            ++updates;
        }
        CHECK(updates == 2);
        REQUIRE(f.factorize(m, I));
        CHECK_FALSE(f.needs_refactor());
    }
}

namespace {

// m x m TSP-like basis with `updates` stable column replacements applied to f (and B).
void churn(std::mt19937_64& rng, BasisFactor& f, std::vector<SparseCol>& B, uint32_t m, int updates) {
    for (int upd = 0; upd < updates; ++upd) {
        const auto a = random_col(rng, m, static_cast<uint32_t>(rng() % m));
        std::vector<double> aq(m, 0.0);
        for (const auto& e : a) aq[e.index] += e.value;
        f.ftran(aq);
        uint32_t r = 0;
        double best = 0;
        for (int s = 0; s < 20; ++s) {
            const uint32_t c = static_cast<uint32_t>(rng() % m);
            if (std::abs(aq[c]) > best) { best = std::abs(aq[c]); r = c; }
        }
        if (best < 1e-3) { --upd; continue; }
        f.update(r, aq);
        B[r] = a;
    }
}

}  // namespace

TEST_CASE("T_ft_residual") {
    std::normal_distribution<double> N(0, 1);
    std::mt19937_64 rng(77);
    double worst_f = 0, worst_b = 0;
    for (const uint32_t m : {500u, 2000u}) {
        std::vector<SparseCol> B;
        BasisFactor f(1000000);  // Forrest-Tomlin, never refactor
        do B = tsp_like_basis(rng, m); while (!f.factorize(m, B));
        churn(rng, f, B, m, 200);
        REQUIRE(f.updates() == 200);
        std::vector<double> xt(m), yt(m);
        for (auto& v : xt) v = N(rng);
        for (auto& v : yt) v = N(rng);
        const auto b = mul(B, xt);
        auto x = b;
        f.ftran(x);
        worst_f = std::max(worst_f, rel_residual(mul(B, x), b));
        const auto d = mul_t(B, yt);
        auto y = d;
        f.btran(y);
        worst_b = std::max(worst_b, rel_residual(mul_t(B, y), d));
    }
    MESSAGE("FT 200 updates: worst_ftran=" << worst_f << " worst_btran=" << worst_b);
    CHECK(worst_f <= 1e-8);
    CHECK(worst_b <= 1e-8);
}

TEST_CASE("T_hyper_sparse_ftran") {
    // Same FtFactor, dense vs hypersparse traversal: bitwise identical results on sparse
    // right-hand sides, far fewer steps + entries touched.
    std::mt19937_64 rng(5);
    const uint32_t m = 3000;
    std::vector<SparseCol> B;
    BasisFactor f(1000000);
    do B = tsp_like_basis(rng, m); while (!f.factorize(m, B));
    churn(rng, f, B, m, 50);
    apex::LuFactorization lu;
    REQUIRE(lu.factorize(m, B));
    apex::FtFactor ft;
    ft.init(lu);
    uint64_t dense_touch = 0, hyper_touch = 0;
    for (int t = 0; t < 200; ++t) {
        std::vector<double> rhs(m, 0.0);
        for (int k = 0; k < 3; ++k) rhs[rng() % m] = 1.0 + (rng() % 7);
        for (int dir = 0; dir < 2; ++dir) {
            auto a = rhs, b = rhs;
            const uint64_t t0 = ft.touched;
            dir ? ft.btran(a, false) : ft.ftran(a, false);
            const uint64_t t1 = ft.touched;
            dir ? ft.btran(b, true) : ft.ftran(b, true);
            dense_touch += t1 - t0;
            hyper_touch += ft.touched - t1;
            REQUIRE(a == b);
        }
    }
    MESSAGE("touched dense=" << dense_touch << " hyper=" << hyper_touch << " ratio=" << double(hyper_touch) / dense_touch);
    CHECK(hyper_touch * 2 < dense_touch);
}
