#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>
#include <vector>

#include "lp/lu.hpp"

using apex::LuFactorization;
using apex::SparseCol;

namespace {

// TSP-like basis: slack columns (one +-1), edge columns (+1 at two rows), and a few
// 3-nnz "cut" columns with random values.
std::vector<SparseCol> tsp_like_basis(std::mt19937_64& rng, uint32_t m) {
    std::vector<uint32_t> perm(m);
    for (uint32_t i = 0; i < m; ++i) perm[i] = i;
    for (uint32_t i = m - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
    std::vector<SparseCol> cols(m);
    std::uniform_real_distribution<double> U(0.5, 2.0);
    for (uint32_t j = 0; j < m; ++j) {
        const uint32_t kind = rng() % 10;
        const uint32_t a = perm[j], b = rng() % m, c = rng() % m;  // perm => structural transversal
        if (kind < 4) cols[j] = {{a, (rng() & 1) ? 1.0 : -1.0}};
        else if (kind < 9) cols[j] = {{a, 1.0}, {b, 1.0}};
        else cols[j] = {{a, U(rng)}, {b, -U(rng)}, {c, U(rng)}};
    }
    return cols;
}

std::vector<double> mul(const std::vector<SparseCol>& B, const std::vector<double>& x, uint32_t m) {
    std::vector<double> y(m, 0.0);
    for (uint32_t j = 0; j < m; ++j)
        for (const auto& e : B[j]) y[e.index] += e.value * x[j];
    return y;
}

std::vector<double> mul_t(const std::vector<SparseCol>& B, const std::vector<double>& y, uint32_t m) {
    std::vector<double> x(m, 0.0);
    for (uint32_t j = 0; j < m; ++j)
        for (const auto& e : B[j]) x[j] += e.value * y[e.index];
    return x;
}

double rel_residual(const std::vector<double>& lhs, const std::vector<double>& b) {
    double num = 0, den = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        num += (lhs[i] - b[i]) * (lhs[i] - b[i]);
        den += b[i] * b[i];
    }
    return std::sqrt(num) / std::sqrt(den);
}

}  // namespace

TEST_CASE("T_lu_residual") {
    std::mt19937_64 rng(4242);
    std::normal_distribution<double> N(0, 1);
    int bases = 0, singular_rejected = 0;
    double worst_f = 0, worst_b = 0, total_ms = 0;
    long long max_fill = 0;
    const uint32_t sizes[] = {10, 50, 200, 1000, 5000};
    int attempts = 0;
    while (bases < 50) {
        REQUIRE(++attempts < 500);
        const uint32_t m = sizes[bases % 5];
        const auto B = tsp_like_basis(rng, m);
        LuFactorization lu;
        const auto t0 = std::chrono::steady_clock::now();
        if (!lu.factorize(m, B)) { ++singular_rejected; continue; }  // random bases may be singular
        total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++bases;
        max_fill = std::max(max_fill, lu.fill_in());

        std::vector<double> x_true(m), y_true(m);
        for (auto& v : x_true) v = N(rng);
        for (auto& v : y_true) v = N(rng);
        auto b = mul(B, x_true, m);
        auto x = b;
        lu.ftran(x);
        worst_f = std::max(worst_f, rel_residual(mul(B, x, m), b));
        auto d = mul_t(B, y_true, m);
        auto y = d;
        lu.btran(y);
        worst_b = std::max(worst_b, rel_residual(mul_t(B, y, m), d));
    }
    MESSAGE("bases=50 singular_rejected=" << singular_rejected << " worst_ftran=" << worst_f
                                          << " worst_btran=" << worst_b << " max_fill=" << max_fill
                                          << " total_factor_ms=" << total_ms);
    CHECK(worst_f <= 1e-8);
    CHECK(worst_b <= 1e-8);
}

TEST_CASE("T_lu_dense_random") {
    std::mt19937_64 rng(9);
    std::normal_distribution<double> N(0, 1);
    std::uniform_real_distribution<double> U(0, 1);
    const uint32_t m = 150;
    std::vector<SparseCol> B(m);
    for (uint32_t j = 0; j < m; ++j)
        for (uint32_t i = 0; i < m; ++i)
            if (i == j || U(rng) < 0.08) B[j].push_back({i, N(rng)});
    LuFactorization lu;
    REQUIRE(lu.factorize(m, B));
    std::vector<double> xt(m);
    for (auto& v : xt) v = N(rng);
    const auto b = mul(B, xt, m);
    auto x = b;
    lu.ftran(x);
    CHECK(rel_residual(mul(B, x, m), b) <= 1e-8);
    CHECK(lu.fill_in() > 0);
    CHECK(lu.density() > 0);
    MESSAGE("dense150 fill_in=" << lu.fill_in() << " density=" << lu.density());
}

TEST_CASE("T_lu_markowitz_threshold") {
    std::mt19937_64 rng(77);
    std::normal_distribution<double> N(0, 1);
    for (double thr : {0.01, 0.1, 0.5, 1.0}) {
        CAPTURE(thr);
        int done = 0, tries = 0;
        while (done < 10) {
            REQUIRE(++tries < 500);
            const uint32_t m = 300;
            auto B = tsp_like_basis(rng, m);
            for (auto& c : B)
                for (auto& e : c) e.value *= std::exp(3 * N(rng));  // badly scaled values
            LuFactorization lu;
            if (!lu.factorize(m, B, thr)) continue;
            ++done;
            CHECK(lu.min_pivot_ratio() >= thr);
        }
    }
}

TEST_CASE("T_lu_singular_detection") {
    // Duplicate column -> rank deficiency 1.
    std::vector<SparseCol> B = {{{0, 1}, {1, 2}}, {{0, 2}, {1, 4}}, {{2, 1}}};
    LuFactorization lu;
    CHECK_FALSE(lu.factorize(3, B));
    CHECK(lu.singular_positions().size() == 1);
    CHECK(lu.unpivoted_rows().size() == 1);
    // Empty column.
    std::vector<SparseCol> E = {{{0, 1}}, {}, {{1, 1}}};
    CHECK_FALSE(lu.factorize(3, E));
    CHECK(lu.singular_positions() == std::vector<uint32_t>{1});
    CHECK(lu.unpivoted_rows() == std::vector<uint32_t>{2});
    // Identity is trivially fine with zero fill.
    std::vector<SparseCol> I = {{{0, 1}}, {{1, 1}}, {{2, 1}}};
    CHECK(lu.factorize(3, I));
    CHECK(lu.fill_in() == 0);
}
