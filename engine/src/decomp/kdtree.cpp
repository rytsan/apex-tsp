#include "decomp/kdtree.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

namespace apex {

namespace {

void build(const Instance& inst, std::vector<uint32_t> pts, uint32_t max_leaf, std::vector<KdNode>& out, size_t at) {
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
    const size_t k = (pts.size() + max_leaf - 1) / max_leaf;
    const size_t cut = pts.size() * (k / 2) / k;
    auto less = [&](uint32_t a, uint32_t b) { return c[a] < c[b] || (c[a] == c[b] && a < b); };
    std::nth_element(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(cut), pts.end(), less);
    std::vector<uint32_t> left(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(cut)), right(pts.begin() + static_cast<std::ptrdiff_t>(cut), pts.end());
    out[at].axis = axis;
    out[at].split = c[pts[cut]];
    const size_t l = out.size();
    out.emplace_back();
    out.emplace_back();
    out[at].left = static_cast<int32_t>(l);
    out[at].right = static_cast<int32_t>(l + 1);
    build(inst, std::move(left), max_leaf, out, l);
    build(inst, std::move(right), max_leaf, out, l + 1);
}

}  // namespace

std::vector<KdNode> build_kdtree(const Instance& inst, uint32_t max_leaf) {
    std::vector<uint32_t> all(inst.n);
    std::iota(all.begin(), all.end(), 0u);
    std::vector<KdNode> out(1);
    build(inst, std::move(all), std::max(1u, max_leaf), out, 0);
    return out;
}

}  // namespace apex
