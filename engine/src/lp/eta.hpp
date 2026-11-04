#pragma once

#include <cstdint>
#include <vector>

#include "lp/lu.hpp"

namespace apex {

// Product-form-of-inverse updates. After a basis change at position r with
// aq = B^-1 a_q (position-indexed), B_new^-1 = E^-1 B^-1, E = I with column r := aq.
class EtaFile {
public:
    void reset();
    void push(uint32_t r, const std::vector<double>& aq);  // requires aq[r] != 0
    void apply_ftran(std::vector<double>& v) const;  // after LU ftran (position space)
    void apply_btran(std::vector<double>& v) const;  // before LU btran (position space)
    size_t size() const { return etas_.size(); }
    size_t nnz() const { return idx_.size() + etas_.size(); }

private:
    struct Eta {
        uint32_t r;
        double pivot;
        uint32_t begin, end;  // off-pivot entries (i, aq_i)
    };
    std::vector<Eta> etas_;
    std::vector<uint32_t> idx_;
    std::vector<double> val_;
};

// Refactorize when iterations_since_refact >= interval OR eta nnz >= factor * LU nnz
// at the last refactorization, whichever first. LU size uses nnz(L)+nnz(U) rather than
// pure fill-in: an all-slack basis has zero fill-in, which would fire on every update.
class RefactorScheduler {
public:
    explicit RefactorScheduler(uint32_t interval = 100, double factor = 2.0) : interval_(interval), factor_(factor) {}
    void on_refactor(size_t lu_nnz) { lu_nnz_ = lu_nnz; iters_ = 0; }
    void on_update() { ++iters_; }
    bool should_refactor(size_t eta_nnz) const {
        return iters_ >= interval_ || static_cast<double>(eta_nnz) >= factor_ * static_cast<double>(lu_nnz_);
    }
    uint32_t iterations_since_refactor() const { return iters_; }

private:
    uint32_t interval_;
    double factor_;
    size_t lu_nnz_ = 0;
    uint32_t iters_ = 0;
};

// LU + eta file + scheduler: the basis inverse used by the simplex.
class BasisFactor {
public:
    explicit BasisFactor(uint32_t refactor_interval = 100) : sched_(refactor_interval) {}

    bool factorize(uint32_t m, const std::vector<SparseCol>& cols, double threshold = 0.01) {
        eta_.reset();
        const bool ok = lu_.factorize(m, cols, threshold);
        sched_.on_refactor(lu_.nnz_factors());
        return ok;
    }
    void ftran(std::vector<double>& a) const { lu_.ftran(a); eta_.apply_ftran(a); }
    void btran(std::vector<double>& d) const { eta_.apply_btran(d); lu_.btran(d); }
    void update(uint32_t r, const std::vector<double>& aq) { eta_.push(r, aq); sched_.on_update(); }
    bool needs_refactor() const { return sched_.should_refactor(eta_.nnz()); }

    const LuFactorization& lu() const { return lu_; }
    const EtaFile& eta() const { return eta_; }

private:
    LuFactorization lu_;
    EtaFile eta_;
    RefactorScheduler sched_;
};

}  // namespace apex
