#include "bnc/pricing.hpp"

#include <algorithm>
#include <cmath>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace apex {

void pricing_filter_scalar(double xu, double yu, double Yu, const double* xs, const double* ys, const double* Y,
                           uint32_t m, uint8_t* keep) {
    for (uint32_t k = 0; k < m; ++k) {
        const double r = Yu + Y[k] + 0.5 + 1e-6;
        const double dx = xu - xs[k], dy = yu - ys[k];
        const double dx2 = dx * dx, dy2 = dy * dy;  // separate products: same rounding as the SIMD path
        keep[k] = r > 0 && dx2 + dy2 < r * r;
    }
}

bool pricing_simd_available() {
#if defined(__AVX2__)
    return true;
#else
    return false;
#endif
}

void pricing_filter_simd(double xu, double yu, double Yu, const double* xs, const double* ys, const double* Y,
                         uint32_t m, uint8_t* keep) {
#if defined(__AVX2__)
    const __m256d vxu = _mm256_set1_pd(xu), vyu = _mm256_set1_pd(yu);
    const __m256d base = _mm256_set1_pd(Yu);
    const __m256d half = _mm256_set1_pd(0.5), eps = _mm256_set1_pd(1e-6), zero = _mm256_setzero_pd();
    uint32_t k = 0;
    for (; k + 4 <= m; k += 4) {
        // r = ((Yu + Y) + 0.5) + 1e-6, the scalar evaluation order
        const __m256d r = _mm256_add_pd(_mm256_add_pd(_mm256_add_pd(base, _mm256_loadu_pd(Y + k)), half), eps);
        const __m256d dx = _mm256_sub_pd(vxu, _mm256_loadu_pd(xs + k));
        const __m256d dy = _mm256_sub_pd(vyu, _mm256_loadu_pd(ys + k));
        const __m256d d2 = _mm256_add_pd(_mm256_mul_pd(dx, dx), _mm256_mul_pd(dy, dy));
        const __m256d ok = _mm256_and_pd(_mm256_cmp_pd(r, zero, _CMP_GT_OQ), _mm256_cmp_pd(d2, _mm256_mul_pd(r, r), _CMP_LT_OQ));
        const int mask = _mm256_movemask_pd(ok);
        for (int b = 0; b < 4; ++b) keep[k + b] = static_cast<uint8_t>((mask >> b) & 1);
    }
    pricing_filter_scalar(xu, yu, Yu, xs + k, ys + k, Y + k, m - k, keep + k);
#else
    pricing_filter_scalar(xu, yu, Yu, xs, ys, Y, m, keep);
#endif
}

double EdgePricer::price(const std::vector<double>& ynode, const std::vector<CutDual>& cuts,
                         const std::unordered_set<uint64_t>& exclude, bool zero_cost, PricingMode mode,
                         std::vector<NegEdge>* out, uint64_t& count, const PricingFilter* filter) {
    const uint32_t n = inst_.n;
    ++stats_.calls;
    std::vector<double> sigma(n, 0.0), Yp(n);
    std::vector<std::vector<std::pair<uint32_t, double>>> member(n);  // (cut k, y_K), sorted by k
    for (uint32_t k = 0; k < cuts.size(); ++k) {
        if (cuts[k].y == 0.0) continue;
        for (uint32_t v : *cuts[k].nodes) {
            sigma[v] += cuts[k].y;
            member[v].push_back({k, cuts[k].y});
        }
    }
    for (uint32_t v = 0; v < n; ++v) {
        double pos = 0;
        for (const auto& [k, y] : member[v]) pos += std::max(0.0, y);
        Yp[v] = ynode[v] + pos;  // rc_uv >= c_uv - Yp_u - Yp_v
    }
    double sum = 0;
    count = 0;
    auto exact = [&](uint32_t u, uint32_t v) {  // u < v
        ++stats_.exact_checks;
        if (exclude.count((uint64_t(u) << 32) | v)) return;
        if (filter && std::ceil(filter->root_lb + dist(inst_, u, v) - (*filter->yroot)[u] - (*filter->yroot)[v] - 1e-6) >=
                          static_cast<double>(filter->ub)) {
            ++stats_.rcf_skipped;
            return;
        }
        double rc = (zero_cost ? 0.0 : dist(inst_, u, v)) - ynode[u] - ynode[v] - sigma[u] - sigma[v];
        const auto& a = member[u];
        const auto& b = member[v];
        for (size_t i = 0, j = 0; i < a.size() && j < b.size();) {
            if (a[i].first == b[j].first) { rc += 2 * a[i].second; ++i; ++j; }
            else if (a[i].first < b[j].first) ++i;
            else ++j;
        }
        if (rc >= -1e-9) return;
        sum += rc;
        ++count;
        if (out) out->push_back({rc, u, v});
    };

    if (zero_cost || mode == PricingMode::Scan) {
        for (uint32_t u = 0; u < n; ++u)
            for (uint32_t v = u + 1; v < n; ++v) {
                ++stats_.pairs_filtered;
                if (!zero_cost && dist(inst_, u, v) - Yp[u] - Yp[v] >= 0) continue;  // exact lower bound
                exact(u, v);
            }
        return sum;
    }

    const double ymax = *std::max_element(Yp.begin(), Yp.end());
    std::vector<uint32_t> idx;
    std::vector<double> bx, by, bY;
    std::vector<uint8_t> keep;
    const bool simd = mode == PricingMode::Grid;
    for (uint32_t u = 0; u < n; ++u) {
        const double radius = Yp[u] + ymax + 0.5 + 1e-6;
        if (radius <= 0) continue;
        idx.clear();
        grid_.for_each_in_radius(u, radius, [&](uint32_t v) {
            if (v > u) idx.push_back(v);
        });
        if (idx.empty()) continue;
        std::sort(idx.begin(), idx.end());  // deterministic output order
        const uint32_t m = static_cast<uint32_t>(idx.size());
        bx.resize(m);
        by.resize(m);
        bY.resize(m);
        keep.resize(m);
        for (uint32_t k = 0; k < m; ++k) {
            bx[k] = inst_.xs[idx[k]];
            by[k] = inst_.ys[idx[k]];
            bY[k] = Yp[idx[k]];
        }
        stats_.pairs_filtered += m;
        (simd ? pricing_filter_simd : pricing_filter_scalar)(inst_.xs[u], inst_.ys[u], Yp[u], bx.data(), by.data(), bY.data(), m,
                                                             keep.data());
        for (uint32_t k = 0; k < m; ++k)
            if (keep[k]) exact(u, idx[k]);
    }
    return sum;
}

}  // namespace apex
