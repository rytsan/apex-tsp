#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "core/instance.hpp"
#include "geo/delaunay.hpp"

using apex::orient2d_sign;

namespace {

apex::Instance make(const std::vector<double>& xs, const std::vector<double>& ys) {
    apex::Instance inst;
    inst.name = "synthetic";
    inst.n = static_cast<uint32_t>(xs.size());
    inst.xs = xs;
    inst.ys = ys;
    for (uint32_t i = 0; i < inst.n; ++i) inst.original_ids.push_back(i + 1);
    return inst;
}

// Representative of each distinct coordinate (lowest index).
std::vector<uint32_t> distinct_points(const apex::Instance& in) {
    std::set<std::pair<double, double>> seen;
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < in.n; ++i)
        if (seen.emplace(in.xs[i], in.ys[i]).second) out.push_back(i);
    return out;
}

// Number of distinct points on the convex hull boundary (including collinear ones).
size_t hull_boundary_count(const apex::Instance& in, const std::vector<uint32_t>& pts) {
    std::vector<uint32_t> p = pts;
    std::sort(p.begin(), p.end(), [&](uint32_t a, uint32_t b) {
        return std::make_pair(in.xs[a], in.ys[a]) < std::make_pair(in.xs[b], in.ys[b]);
    });
    auto o = [&](uint32_t a, uint32_t b, uint32_t c) {
        return orient2d_sign(in.xs[a], in.ys[a], in.xs[b], in.ys[b], in.xs[c], in.ys[c]);
    };
    std::vector<uint32_t> h;  // strict hull (monotone chain)
    for (int pass = 0; pass < 2; ++pass) {
        const size_t base = h.size();
        for (uint32_t q : p) {
            while (h.size() >= base + 2 && o(h[h.size() - 2], h.back(), q) <= 0) h.pop_back();
            h.push_back(q);
        }
        h.pop_back();
        std::reverse(p.begin(), p.end());
    }
    size_t count = 0;
    for (uint32_t q : pts) {
        for (size_t k = 0; k < h.size(); ++k) {
            const uint32_t a = h[k], b = h[(k + 1) % h.size()];
            if (o(a, b, q) == 0 && in.xs[q] >= std::min(in.xs[a], in.xs[b]) &&
                in.xs[q] <= std::max(in.xs[a], in.xs[b]) && in.ys[q] >= std::min(in.ys[a], in.ys[b]) &&
                in.ys[q] <= std::max(in.ys[a], in.ys[b])) {
                ++count;
                break;
            }
        }
    }
    return count;
}

// Structural checks valid for any non-collinear input. Returns the edge count.
void check_structure(const apex::Instance& in, const apex::DelaunayResult& r) {
    const auto pts = distinct_points(in);
    const size_t n = pts.size(), h = hull_boundary_count(in, pts);
    const size_t m = r.edges.size(), t = r.triangles.size() / 3;
    CAPTURE(n);
    CAPTURE(h);
    CHECK(m == 3 * n - 3 - h);         // exact for a triangulation of n points with h on the boundary
    CHECK(t == 2 * n - 2 - h);
    CHECK(m + 5 + h >= 2 * n);         // sprint-file range [2n-5-h, 3n-3-h]
    CHECK(std::adjacent_find(r.edges.begin(), r.edges.end()) == r.edges.end());  // dedup
    std::vector<uint32_t> deg(in.n, 0);
    for (auto [u, v] : r.edges) {
        CHECK(u < v);
        ++deg[u];
        ++deg[v];
    }
    size_t uncovered = 0;
    for (uint32_t p : pts) uncovered += deg[p] == 0;
    CHECK(uncovered == 0);
    size_t bad_orient = 0;
    for (size_t k = 0; k < r.triangles.size(); k += 3) {
        const auto a = r.triangles[k], b = r.triangles[k + 1], c = r.triangles[k + 2];
        bad_orient += orient2d_sign(in.xs[a], in.ys[a], in.xs[b], in.ys[b], in.xs[c], in.ys[c]) >= 0;
    }
    CHECK(bad_orient == 0);  // all triangles strictly clockwise
}

}  // namespace

TEST_CASE("T_orient2d_exact") {
    // Near-degenerate: classic failure case for naive double orientation.
    CHECK(orient2d_sign(0.5, 0.5, 12, 12, 24, 24) == 0);
    CHECK(orient2d_sign(0, 0, 1, 1, 2, 2.0000000000000004) == 1);
    CHECK(orient2d_sign(0, 0, 1, 1, 2, 1.9999999999999998) == -1);
    CHECK(orient2d_sign(0.1, 0.1, 0.2, 0.2, 0.30000000000000004, 0.30000000000000004) == 0);
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<int> L(-1000, 1000);
    int mismatches = 0;
    for (int it = 0; it < 100000; ++it) {  // integer inputs: compare with exact int64 arithmetic
        const int64_t a = L(rng), b = L(rng), c = L(rng), d = L(rng), e = L(rng), f = L(rng);
        const int64_t det = (c - a) * (f - b) - (d - b) * (e - a);
        const int want = det > 0 ? 1 : det < 0 ? -1 : 0;
        mismatches += orient2d_sign(double(a), double(b), double(c), double(d), double(e), double(f)) != want;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("T_delaunay_empty_circle") {
    std::mt19937_64 rng(31337);
    size_t violations = 0, checked = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const uint32_t n = 3 + static_cast<uint32_t>(rng() % 198);  // 3..200
        std::uniform_real_distribution<double> U(0, 1000);
        std::vector<double> xs(n), ys(n);
        for (uint32_t i = 0; i < n; ++i) { xs[i] = U(rng); ys[i] = U(rng); }
        const auto in = make(xs, ys);
        const auto r = apex::delaunay(in);
        check_structure(in, r);
        for (size_t k = 0; k < r.triangles.size(); k += 3) {  // every triangle, brute force
            const auto a = r.triangles[k], b = r.triangles[k + 1], c = r.triangles[k + 2];
            for (uint32_t p = 0; p < n; ++p) {
                if (p == a || p == b || p == c) continue;
                const double dx = xs[a] - xs[p], dy = ys[a] - ys[p];
                const double ex = xs[b] - xs[p], ey = ys[b] - ys[p];
                const double fx = xs[c] - xs[p], fy = ys[c] - ys[p];
                const double ap = dx * dx + dy * dy, bp = ex * ex + ey * ey, cp = fx * fx + fy * fy;
                const double det = dx * (ey * cp - bp * fy) - dy * (ex * cp - bp * fx) + ap * (ex * fy - ey * fx);
                const double scale = (ap + bp + cp) * (ap + bp + cp);
                violations += det < -1e-9 * scale;  // p strictly inside (clockwise triangle)
                ++checked;
            }
        }
    }
    MESSAGE("empty-circle point/triangle checks=" << checked);
    CHECK(violations == 0);
}

TEST_CASE("T_delaunay_edge_count") {
    std::mt19937_64 rng(77);
    for (uint32_t n : {3u, 4u, 10u, 100u, 2000u}) {
        std::uniform_real_distribution<double> U(-1e5, 1e5);
        std::vector<double> xs(n), ys(n);
        for (uint32_t i = 0; i < n; ++i) { xs[i] = U(rng); ys[i] = U(rng); }
        const auto in = make(xs, ys);
        check_structure(in, apex::delaunay(in));
    }
}

TEST_CASE("T_delaunay_degenerate_inputs") {
    SUBCASE("integer lattice with duplicates (cocircular + collinear)") {
        std::mt19937_64 rng(3);
        std::uniform_int_distribution<int> L(0, 30);
        std::vector<double> xs, ys;
        for (int i = 0; i < 1500; ++i) { xs.push_back(L(rng)); ys.push_back(L(rng)); }
        const auto in = make(xs, ys);
        check_structure(in, apex::delaunay(in));
    }
    SUBCASE("full square lattice") {
        std::vector<double> xs, ys;
        for (int i = 0; i < 40; ++i)
            for (int j = 0; j < 40; ++j) { xs.push_back(i * 10.0); ys.push_back(j * 10.0); }
        const auto in = make(xs, ys);
        check_structure(in, apex::delaunay(in));
    }
    SUBCASE("collinear") {
        std::vector<double> xs, ys;
        for (int i = 0; i < 50; ++i) { xs.push_back(49 - i); ys.push_back(2.0 * (49 - i)); }
        xs.push_back(10);  // duplicate
        ys.push_back(20);
        const auto r = apex::delaunay(make(xs, ys));
        CHECK(r.triangles.empty());
        CHECK(r.edges.size() == 49);
    }
    SUBCASE("tiny / identical") {
        CHECK(apex::delaunay(make({1, 2}, {1, 2})).edges.size() == 1);
        CHECK(apex::delaunay(make({1, 1, 1}, {1, 1, 1})).edges.empty());
        const auto r = apex::delaunay(make({0, 1, 0}, {0, 0, 1}));
        CHECK(r.edges.size() == 3);
        CHECK(r.triangles.size() == 3);
    }
}

TEST_CASE("T_delaunay_reference_instances") {
    for (const char* name : {"eil51", "berlin52", "a280", "pr1002", "fl3795", "usa13509", "d15112", "d18512",
                             "pla85900", "mona-lisa100K"}) {
        CAPTURE(name);
        const auto in = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = apex::delaunay(in);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const auto pts = distinct_points(in);
        MESSAGE(std::string(name) << " n=" << in.n << " distinct=" << pts.size() << " edges=" << r.edges.size()
                     << " ms=" << ms);
        if (in.n <= 20000) {
            check_structure(in, r);  // O(n*h) hull check: keep to the smaller instances
        } else {
            CHECK(r.edges.size() + r.triangles.size() / 3 + 1 == pts.size() + r.triangles.size() / 3 * 2);  // Euler
        }
    }
}
