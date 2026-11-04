#include "lp/eta.hpp"

#include <cmath>
#include <stdexcept>

namespace apex {

void EtaFile::reset() {
    etas_.clear();
    idx_.clear();
    val_.clear();
}

void EtaFile::push(uint32_t r, const std::vector<double>& aq) {
    if (r >= aq.size() || aq[r] == 0.0) throw std::invalid_argument("EtaFile::push: zero pivot");
    Eta e{r, aq[r], static_cast<uint32_t>(idx_.size()), 0};
    for (uint32_t i = 0; i < aq.size(); ++i)
        if (i != r && aq[i] != 0.0) {
            idx_.push_back(i);
            val_.push_back(aq[i]);
        }
    e.end = static_cast<uint32_t>(idx_.size());
    etas_.push_back(e);
}

void EtaFile::apply_ftran(std::vector<double>& v) const {
    for (const auto& e : etas_) {  // v := E^-1 v, oldest first
        const double vr = v[e.r] / e.pivot;
        v[e.r] = vr;
        if (vr == 0.0) continue;
        for (uint32_t q = e.begin; q < e.end; ++q) v[idx_[q]] -= val_[q] * vr;
    }
}

void EtaFile::apply_btran(std::vector<double>& v) const {
    for (size_t k = etas_.size(); k-- > 0;) {  // v := E^-T v, newest first
        const auto& e = etas_[k];
        double acc = v[e.r];
        for (uint32_t q = e.begin; q < e.end; ++q) acc -= val_[q] * v[idx_[q]];
        v[e.r] = acc / e.pivot;
    }
}

}  // namespace apex
