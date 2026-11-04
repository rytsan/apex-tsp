#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

#include "lp/csc.hpp"

using apex::CscMatrix;
using apex::Entry;
using apex::Row;
using apex::RowData;
using apex::Sense;
using apex::Triplet;

namespace {

CscMatrix random_matrix(std::mt19937_64& rng, uint32_t rows, uint32_t cols, double density) {
    std::uniform_real_distribution<double> U(0, 1), V(-5, 5);
    std::vector<Triplet> t;
    for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < cols; ++j)
            if (U(rng) < density) t.push_back({i, j, std::round(V(rng) * 4) / 4});  // includes some zeros
    return CscMatrix::from_triplets(rows, cols, t);
}

// Brute-force 2-variable LP: min c.x s.t. matrix rows, 0 <= x <= ub. Enumerates every
// vertex formed by two constraint/bound lines. Reads rows through the CSR view.
double brute_lp_2d(const CscMatrix& m, double c0, double c1, double ub) {
    struct Line { double a, b, r; };
    std::vector<Line> lines{{1, 0, 0}, {0, 1, 0}, {1, 0, ub}, {0, 1, ub}};
    for (uint32_t i = 0; i < m.num_rows(); ++i) {
        double a = 0, b = 0;
        const auto cols = m.row_cols(i);
        const auto vals = m.row_vals(i);
        for (size_t k = 0; k < cols.size(); ++k) (cols[k] == 0 ? a : b) = vals[k];
        lines.push_back({a, b, m.row_data()[i].rhs});
    }
    auto feasible = [&](double x, double y) {
        if (x < -1e-9 || y < -1e-9 || x > ub + 1e-9 || y > ub + 1e-9) return false;
        for (uint32_t i = 0; i < m.num_rows(); ++i) {
            const auto& L = lines[4 + i];
            const double lhs = L.a * x + L.b * y;
            const auto s = m.row_data()[i].sense;
            if ((s == Sense::Ge && lhs < L.r - 1e-9) || (s == Sense::Le && lhs > L.r + 1e-9) ||
                (s == Sense::Eq && std::abs(lhs - L.r) > 1e-9))
                return false;
        }
        return true;
    };
    double best = std::numeric_limits<double>::infinity();
    for (size_t p = 0; p < lines.size(); ++p)
        for (size_t q = p + 1; q < lines.size(); ++q) {
            const double det = lines[p].a * lines[q].b - lines[p].b * lines[q].a;
            if (std::abs(det) < 1e-12) continue;
            const double x = (lines[p].r * lines[q].b - lines[p].b * lines[q].r) / det;
            const double y = (lines[p].a * lines[q].r - lines[p].r * lines[q].a) / det;
            if (feasible(x, y)) best = std::min(best, c0 * x + c1 * y);
        }
    return best;
}

}  // namespace

TEST_CASE("T_csc_transpose_idempotent") {
    std::mt19937_64 rng(1);
    for (int trial = 0; trial < 50; ++trial) {
        const uint32_t r = 1 + rng() % 60, c = 1 + rng() % 60;
        const auto a = random_matrix(rng, r, c, 0.1);
        const auto t = a.transposed();
        CHECK(t.num_rows() == a.num_cols());
        CHECK(t.num_cols() == a.num_rows());
        CHECK(t.nnz() == a.nnz());
        CHECK(t.transposed() == a);
        for (uint32_t j = 0; j < a.num_cols(); ++j) {  // column j of A == row j of A^T
            const auto ar = a.col_rows(j), tr = t.row_cols(j);
            CHECK(std::vector<uint32_t>(ar.begin(), ar.end()) == std::vector<uint32_t>(tr.begin(), tr.end()));
        }
    }
}

TEST_CASE("T_csc_column_view") {
    // [ 1 0 2 ]
    // [ 0 3 0 ]   built with a duplicate (2 = 1.5 + 0.5) and an explicit zero.
    auto m = CscMatrix::from_triplets(2, 3, {{0, 0, 1}, {1, 1, 3}, {0, 2, 1.5}, {0, 2, 0.5}, {1, 0, 0.0}});
    CHECK(m.nnz() == 3);
    CHECK(m.col_rows(0).size() == 1);
    CHECK(m.col_vals(2)[0] == 2.0);
    m.add_column(3, {{1, 4.0}, {0, -1.0}, {1, 1.0}});  // unsorted + duplicate row 1 => 5
    REQUIRE(m.num_cols() == 4);
    CHECK(m.col_rows(3)[0] == 0);
    CHECK(m.col_vals(3)[1] == 5.0);
    CHECK(m.row_cols(1).size() == 2);  // CSR kept in sync
    CHECK_THROWS_AS(m.add_column(9, {}), std::invalid_argument);
    CHECK_THROWS_AS(m.add_column(4, {{7, 1.0}}), std::out_of_range);

    m.append_rows({Row{{{0, 1.0}, {3, 1.0}}, Sense::Ge, 2.0}});
    CHECK(m.pending_rows() == 1);
    CHECK_THROWS_AS(m.col_rows(0), std::logic_error);  // must merge before column access
    CHECK(m.row_cols(2).size() == 2);                   // row view works while buffered
    m.ensure_merged();
    CHECK(m.col_rows(0).size() == 2);
    CHECK(m.col_rows(0)[1] == 2);
    CHECK(m.col_rows(3).back() == 2);
}

TEST_CASE("T_csc_append_rows") {
    // Merged CSC equals a from-scratch build with all rows, for random matrices and
    // buffer sizes that force automatic merges mid-stream.
    std::mt19937_64 rng(2);
    std::uniform_real_distribution<double> V(-3, 3);
    for (uint32_t buffer : {1u, 3u, 4096u}) {
        const uint32_t cols = 40;
        CscMatrix m(0, cols, buffer);
        std::vector<Triplet> all;
        std::vector<RowData> rd;
        for (uint32_t i = 0; i < 200; ++i) {
            Row r;
            for (uint32_t j = 0; j < cols; ++j)
                if (rng() % 7 == 0) {
                    const double v = std::round(V(rng));
                    r.entries.push_back({j, v});
                    all.push_back({i, j, v});
                }
            r.sense = Sense::Ge;
            r.rhs = 2;
            rd.push_back({Sense::Ge, 2});
            m.append_rows({r});
            CHECK(m.pending_rows() <= buffer);
        }
        m.ensure_merged();
        CHECK(m == CscMatrix::from_triplets(200, cols, all, rd));
    }

    // Small LP: min -x - y s.t. x + 2y <= 4, 3x + y <= 6, 0 <= x,y <= 10. Optimum at (1.6, 1.2) = -2.8.
    auto lp = CscMatrix::from_triplets(2, 2, {{0, 0, 1}, {0, 1, 2}, {1, 0, 3}, {1, 1, 1}},
                                       {{Sense::Le, 4}, {Sense::Le, 6}});
    const double before = brute_lp_2d(lp, -1, -1, 10);
    CHECK(before == doctest::Approx(-2.8));
    lp.append_rows({Row{{{0, 1}, {1, 1}}, Sense::Le, 5}, Row{{{0, 1}}, Sense::Ge, 0.5}});  // non-binding
    CHECK(brute_lp_2d(lp, -1, -1, 10) == doctest::Approx(before));
    lp.append_rows({Row{{{0, 1}, {1, 1}}, Sense::Le, 2}});  // binding: optimum must move to -2
    CHECK(brute_lp_2d(lp, -1, -1, 10) == doctest::Approx(-2.0));
    lp.ensure_merged();
    CHECK(lp.num_rows() == 5);
    CHECK(lp.col_rows(1).size() == 4);  // y appears in rows 0, 1, 2, 4
}
