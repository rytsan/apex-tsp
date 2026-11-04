#include "geo/delaunay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace apex {
namespace {

// ---- Exact arithmetic helpers (Shewchuk-style expansions) -------------------

inline void two_sum(double a, double b, double& x, double& y) {
    x = a + b;
    const double bv = x - a, av = x - bv;
    y = (a - av) + (b - bv);
}

inline void two_diff(double a, double b, double& x, double& y) {
    x = a - b;
    const double bv = a - x, av = x + bv;
    y = (a - av) + (bv - b);
}

inline void two_product(double a, double b, double& x, double& y) {
    x = a * b;
    y = std::fma(a, b, -x);  // exact error term
}

// Adds b to the non-overlapping expansion e (increasing magnitude), in place.
void grow_expansion(std::vector<double>& e, double b) {
    double q = b;
    for (double& ei : e) {
        double h = 0;
        two_sum(q, ei, q, h);
        ei = h;
    }
    e.push_back(q);
}

int orient_exact(double px, double py, double qx, double qy, double rx, double ry) {
    double a1, a0, b1, b0, c1, c0, d1, d0;
    two_diff(qx, px, a1, a0);
    two_diff(ry, py, b1, b0);
    two_diff(qy, py, c1, c0);
    two_diff(rx, px, d1, d0);
    const double ab[2][2] = {{a1, a0}, {b1, b0}};
    const double cd[2][2] = {{c1, c0}, {d1, d0}};
    std::vector<double> e;
    e.reserve(17);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            double x, y;
            two_product(ab[0][i], ab[1][j], x, y);
            grow_expansion(e, x);
            grow_expansion(e, y);
            two_product(cd[0][i], cd[1][j], x, y);
            grow_expansion(e, -x);
            grow_expansion(e, -y);
        }
    for (auto it = e.rbegin(); it != e.rend(); ++it)
        if (*it != 0.0) return *it > 0 ? 1 : -1;
    return 0;
}

// Robust "strictly counter-clockwise" (y-up).
inline bool ccw(double px, double py, double qx, double qy, double rx, double ry) {
    return orient2d_sign(px, py, qx, qy, rx, ry) > 0;
}

// True only when p is certainly inside the circumcircle of clockwise triangle (a, b, c).
inline bool in_circle_certain(double ax, double ay, double bx, double by, double cx, double cy, double px,
                              double py) {
    const double dx = ax - px, dy = ay - py;
    const double ex = bx - px, ey = by - py;
    const double fx = cx - px, fy = cy - py;
    const double ap = dx * dx + dy * dy, bp = ex * ex + ey * ey, cp = fx * fx + fy * fy;
    const double det = dx * (ey * cp - bp * fy) - dy * (ex * cp - bp * fx) + ap * (ex * fy - ey * fx);
    const double perm = std::abs(dx) * (std::abs(ey * cp) + std::abs(bp * fy)) +
                        std::abs(dy) * (std::abs(ex * cp) + std::abs(bp * fx)) +
                        ap * (std::abs(ex * fy) + std::abs(ey * fx));
    return det < -1e-12 * perm;  // generous vs. the ~1.2e-15 forward error bound
}

inline double circumradius2(double ax, double ay, double bx, double by, double cx, double cy) {
    const double dx = bx - ax, dy = by - ay, ex = cx - ax, ey = cy - ay;
    const double bl = dx * dx + dy * dy, cl = ex * ex + ey * ey;
    const double d = 0.5 / (dx * ey - dy * ex);
    const double x = (ey * bl - dy * cl) * d, y = (dx * cl - ex * bl) * d;
    return x * x + y * y;
}

inline void circumcenter(double ax, double ay, double bx, double by, double cx, double cy, double& ox,
                         double& oy) {
    const double dx = bx - ax, dy = by - ay, ex = cx - ax, ey = cy - ay;
    const double bl = dx * dx + dy * dy, cl = ex * ex + ey * ey;
    const double d = 0.5 / (dx * ey - dy * ex);
    ox = ax + (ey * bl - dy * cl) * d;
    oy = ay + (dx * cl - ex * bl) * d;
}

inline double pseudo_angle(double dx, double dy) {
    const double p = dx / (std::abs(dx) + std::abs(dy));
    return (dy > 0 ? 3 - p : 1 + p) / 4;  // [0, 1)
}

constexpr int32_t kNone = -1;

class Sweep {
public:
    Sweep(const std::vector<double>& xs, const std::vector<double>& ys) : x_(xs), y_(ys) {}

    DelaunayResult run();

private:
    uint32_t hash_key(double x, double y) const {
        const double a = pseudo_angle(x - cx_, y - cy_) * hash_size_;
        return static_cast<uint32_t>(std::floor(a)) % hash_size_;
    }
    uint32_t add_triangle(uint32_t i0, uint32_t i1, uint32_t i2, int32_t a, int32_t b, int32_t c) {
        const auto t = static_cast<uint32_t>(tri_.size());
        tri_.push_back(i0);
        tri_.push_back(i1);
        tri_.push_back(i2);
        half_.resize(tri_.size(), kNone);
        link(static_cast<int32_t>(t), a);
        link(static_cast<int32_t>(t + 1), b);
        link(static_cast<int32_t>(t + 2), c);
        return t;
    }
    void link(int32_t a, int32_t b) {
        half_[a] = b;
        if (b != kNone) half_[b] = a;
    }
    uint32_t legalize(uint32_t a);
    DelaunayResult collinear(std::vector<uint32_t> ids) const;

    const std::vector<double>& x_;
    const std::vector<double>& y_;
    std::vector<uint32_t> tri_;
    std::vector<int32_t> half_;
    std::vector<uint32_t> hull_next_, hull_prev_, hull_tri_;
    std::vector<int32_t> hull_hash_;
    std::vector<uint32_t> stack_;
    uint32_t hull_start_ = 0, hash_size_ = 1;
    double cx_ = 0, cy_ = 0;
};

uint32_t Sweep::legalize(uint32_t a) {
    stack_.clear();
    uint32_t ar = 0;
    for (;;) {
        const int32_t b = half_[a];
        const uint32_t a0 = a - a % 3;
        ar = a0 + (a + 2) % 3;
        if (b == kNone) {
            if (stack_.empty()) break;
            a = stack_.back();
            stack_.pop_back();
            continue;
        }
        const uint32_t ub = static_cast<uint32_t>(b);
        const uint32_t b0 = ub - ub % 3;
        const uint32_t al = a0 + (a + 1) % 3;
        const uint32_t bl = b0 + (ub + 2) % 3;
        const uint32_t p0 = tri_[ar], pr = tri_[a], pl = tri_[al], p1 = tri_[bl];
        if (in_circle_certain(x_[p0], y_[p0], x_[pr], y_[pr], x_[pl], y_[pl], x_[p1], y_[p1])) {
            tri_[a] = p1;
            tri_[ub] = p0;
            const int32_t hbl = half_[bl];
            if (hbl == kNone) {  // edge swapped on the other side of the hull: fix reference
                uint32_t e = hull_start_;
                do {
                    if (hull_tri_[e] == bl) {
                        hull_tri_[e] = a;
                        break;
                    }
                    e = hull_prev_[e];
                } while (e != hull_start_);
            }
            link(static_cast<int32_t>(a), hbl);
            link(b, half_[ar]);
            link(static_cast<int32_t>(ar), static_cast<int32_t>(bl));
            stack_.push_back(b0 + (ub + 1) % 3);
        } else {
            if (stack_.empty()) break;
            a = stack_.back();
            stack_.pop_back();
        }
    }
    return ar;
}

DelaunayResult Sweep::collinear(std::vector<uint32_t> ids) const {
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
        return std::tie(x_[a], y_[a], a) < std::tie(x_[b], y_[b], b);
    });
    DelaunayResult r;
    for (size_t k = 1, prev = 0; k < ids.size(); ++k) {
        if (x_[ids[k]] == x_[ids[prev]] && y_[ids[k]] == y_[ids[prev]]) continue;  // duplicate
        r.edges.emplace_back(std::min(ids[prev], ids[k]), std::max(ids[prev], ids[k]));
        prev = k;
    }
    std::sort(r.edges.begin(), r.edges.end());
    return r;
}

DelaunayResult Sweep::run() {
    const auto n = static_cast<uint32_t>(x_.size());
    std::vector<uint32_t> ids(n);
    for (uint32_t i = 0; i < n; ++i) ids[i] = i;
    if (n < 3) return collinear(ids);

    const auto [lx, hx] = std::minmax_element(x_.begin(), x_.end());
    const auto [ly, hy] = std::minmax_element(y_.begin(), y_.end());
    const double mx = (*lx + *hx) / 2, my = (*ly + *hy) / 2;
    auto dist2 = [&](uint32_t i, double ox, double oy) {
        const double dx = x_[i] - ox, dy = y_[i] - oy;
        return dx * dx + dy * dy;
    };

    // Seed: point nearest the box centre, its nearest distinct neighbour, and the
    // third point giving the smallest circumcircle.
    uint32_t i0 = 0, i1 = n, i2 = n;
    double best = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < n; ++i)
        if (const double d = dist2(i, mx, my); d < best) { best = d; i0 = i; }
    best = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < n; ++i)
        if (const double d = dist2(i, x_[i0], y_[i0]); d > 0 && d < best) { best = d; i1 = i; }
    if (i1 == n) return collinear(ids);  // all points identical
    best = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < n; ++i) {
        if (i == i0 || i == i1) continue;
        if (orient2d_sign(x_[i0], y_[i0], x_[i1], y_[i1], x_[i], y_[i]) == 0) continue;
        const double r = circumradius2(x_[i0], y_[i0], x_[i1], y_[i1], x_[i], y_[i]);
        if (r < best) { best = r; i2 = i; }
    }
    if (i2 == n) return collinear(ids);
    if (ccw(x_[i0], y_[i0], x_[i1], y_[i1], x_[i2], y_[i2])) std::swap(i1, i2);  // make clockwise

    circumcenter(x_[i0], y_[i0], x_[i1], y_[i1], x_[i2], y_[i2], cx_, cy_);
    std::vector<double> dists(n);
    for (uint32_t i = 0; i < n; ++i) dists[i] = dist2(i, cx_, cy_);
    // Total order: identical points become adjacent, so duplicates are skipped reliably.
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
        return std::tie(dists[a], x_[a], y_[a], a) < std::tie(dists[b], x_[b], y_[b], b);
    });

    hash_size_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(n)))));
    hull_next_.assign(n, 0);
    hull_prev_.assign(n, 0);
    hull_tri_.assign(n, 0);
    hull_hash_.assign(hash_size_, kNone);
    const size_t max_tri = n < 3 ? 1 : 2 * static_cast<size_t>(n) - 5;
    tri_.reserve(max_tri * 3);
    half_.reserve(max_tri * 3);

    hull_start_ = i0;
    hull_next_[i0] = hull_prev_[i2] = i1;
    hull_next_[i1] = hull_prev_[i0] = i2;
    hull_next_[i2] = hull_prev_[i1] = i0;
    hull_tri_[i0] = 0;
    hull_tri_[i1] = 1;
    hull_tri_[i2] = 2;
    hull_hash_[hash_key(x_[i0], y_[i0])] = static_cast<int32_t>(i0);
    hull_hash_[hash_key(x_[i1], y_[i1])] = static_cast<int32_t>(i1);
    hull_hash_[hash_key(x_[i2], y_[i2])] = static_cast<int32_t>(i2);
    add_triangle(i0, i1, i2, kNone, kNone, kNone);

    double xp = 0, yp = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = ids[k];
        const double x = x_[i], y = y_[i];
        if (k > 0 && x == xp && y == yp) continue;  // exact duplicate
        xp = x;
        yp = y;
        if (i == i0 || i == i1 || i == i2) continue;

        // Find a visible hull edge via the angular hash.
        uint32_t start = 0;
        const uint32_t key = hash_key(x, y);
        for (uint32_t j = 0; j < hash_size_; ++j) {
            const int32_t s = hull_hash_[(key + j) % hash_size_];
            if (s != kNone && static_cast<uint32_t>(s) != hull_next_[s]) {
                start = static_cast<uint32_t>(s);
                break;
            }
        }
        start = hull_prev_[start];
        uint32_t e = start, q = 0;
        bool found = true;
        while (q = hull_next_[e], !ccw(x, y, x_[e], y_[e], x_[q], y_[q])) {
            e = q;
            if (e == start) { found = false; break; }
        }
        if (!found) continue;  // not strictly outside the hull (cannot happen for distinct points)

        uint32_t t = add_triangle(e, i, hull_next_[e], kNone, kNone, static_cast<int32_t>(hull_tri_[e]));
        hull_tri_[i] = legalize(t + 2);
        hull_tri_[e] = t;

        uint32_t nn = hull_next_[e];
        while (q = hull_next_[nn], ccw(x, y, x_[nn], y_[nn], x_[q], y_[q])) {
            t = add_triangle(nn, i, q, static_cast<int32_t>(hull_tri_[i]), kNone, static_cast<int32_t>(hull_tri_[nn]));
            hull_tri_[i] = legalize(t + 2);
            hull_next_[nn] = nn;  // removed
            nn = q;
        }
        if (e == start) {
            while (q = hull_prev_[e], ccw(x, y, x_[q], y_[q], x_[e], y_[e])) {
                t = add_triangle(q, i, e, kNone, static_cast<int32_t>(hull_tri_[e]), static_cast<int32_t>(hull_tri_[q]));
                legalize(t + 2);
                hull_tri_[q] = t;
                hull_next_[e] = e;  // removed
                e = q;
            }
        }
        hull_start_ = hull_prev_[i] = e;
        hull_next_[e] = hull_prev_[nn] = i;
        hull_next_[i] = nn;
        hull_hash_[hash_key(x, y)] = static_cast<int32_t>(i);
        hull_hash_[hash_key(x_[e], y_[e])] = static_cast<int32_t>(e);
    }

    DelaunayResult r;
    r.triangles = std::move(tri_);
    r.edges.reserve(r.triangles.size() / 2 + 3);
    for (size_t h = 0; h < r.triangles.size(); ++h) {
        const int32_t opp = half_[h];
        if (opp != kNone && static_cast<size_t>(opp) < h) continue;  // interior edge: emit once
        const uint32_t u = r.triangles[h], v = r.triangles[h % 3 == 2 ? h - 2 : h + 1];
        r.edges.emplace_back(std::min(u, v), std::max(u, v));
    }
    std::sort(r.edges.begin(), r.edges.end());
    return r;
}

}  // namespace

int orient2d_sign(double px, double py, double qx, double qy, double rx, double ry) {
    const double l = (qx - px) * (ry - py), r = (qy - py) * (rx - px);
    const double det = l - r;
    const double bound = 3.3306690738754716e-16 * (std::abs(l) + std::abs(r));  // (3 + 16eps) eps
    if (det > bound) return 1;
    if (-det > bound) return -1;
    return orient_exact(px, py, qx, qy, rx, ry);
}

DelaunayResult delaunay(const Instance& inst) {
    return Sweep(inst.xs, inst.ys).run();
}

}  // namespace apex
