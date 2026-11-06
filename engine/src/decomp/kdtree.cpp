#include "decomp/kdtree.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

namespace apex {

namespace {

void build(const Instance& inst, std::vector<uint32_t> pts, uint32_t max_leaf, std::vector<KdNode>& out, size_t at,
           bool density) {
    if (pts.size() <= max_leaf) {
        std::sort(pts.begin(), pts.end());
        out[at].points = std::move(pts);
        return;
    }
    double lo[2] = {inst.xs[pts[0]], inst.ys[pts[0]]}, hi[2] = {lo[0], lo[1]};
    for (uint32_t v : pts) {
        lo[0] = std::min(lo[0], inst.xs[v]); hi[0] = std::max(hi[0], inst.xs[v]);
        lo[1] = std::min(lo[1], inst.ys[v]); hi[1] = std::max(hi[1], inst.ys[v]);
    }
    const uint8_t axis = hi[1] - lo[1] > hi[0] - lo[0] ? 1 : 0;
    const auto& c = axis == 0 ? inst.xs : inst.ys;
    auto less = [&](uint32_t a, uint32_t b) { return c[a] < c[b] || (c[a] == c[b] && a < b); };
    size_t cut;
    if (density) {
        std::sort(pts.begin(), pts.end(), less);
        const size_t n = pts.size(), lo = std::max<size_t>(1, n * 35 / 100), hi = std::min(n - 1, n * 65 / 100);
        cut = n / 2;
        double best = -1;
        for (size_t i = lo; i <= hi; ++i) {
            const double gap = c[pts[i]] - c[pts[i - 1]];
            const auto off = [&](size_t x) { return x > n / 2 ? x - n / 2 : n / 2 - x; };
            if (gap > best || (gap == best && off(i) < off(cut))) { best = gap; cut = i; }
        }
    } else {
        const size_t k = (pts.size() + max_leaf - 1) / max_leaf;
        cut = pts.size() * (k / 2) / k;
        std::nth_element(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(cut), pts.end(), less);
    }
    std::vector<uint32_t> left(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(cut)), right(pts.begin() + static_cast<std::ptrdiff_t>(cut), pts.end());
    out[at].axis = axis;
    out[at].split = density ? 0.5 * (c[pts[cut - 1]] + c[pts[cut]]) : c[pts[cut]];
    const size_t l = out.size();
    out.emplace_back();
    out.emplace_back();
    out[at].left = static_cast<int32_t>(l);
    out[at].right = static_cast<int32_t>(l + 1);
    build(inst, std::move(left), max_leaf, out, l, density);
    build(inst, std::move(right), max_leaf, out, l + 1, density);
}

}  // namespace

std::vector<KdNode> build_kdtree(const Instance& inst, uint32_t max_leaf) {
    std::vector<uint32_t> all(inst.n);
    std::iota(all.begin(), all.end(), 0u);
    std::vector<KdNode> out(1);
    build(inst, std::move(all), std::max(1u, max_leaf), out, 0, false);
    return out;
}

std::vector<KdNode> build_density_tree(const Instance& inst, uint32_t max_leaf) {
    std::vector<uint32_t> all(inst.n);
    std::iota(all.begin(), all.end(), 0u);
    std::vector<KdNode> out(1);
    build(inst, std::move(all), std::max(3u, max_leaf), out, 0, true);
    return out;
}

}  // namespace apex
