#include "lp/dual_simplex.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <stdexcept>

#include "lp/eta.hpp"

namespace apex {

const char* status_name(SimplexResult::Status s) {
    switch (s) {
        case SimplexResult::Status::Optimal: return "optimal";
        case SimplexResult::Status::IterationLimit: return "iteration_limit";
        case SimplexResult::Status::TimeLimit: return "time_limit";
        case SimplexResult::Status::Unbounded: return "unbounded";
        case SimplexResult::Status::Infeasible: return "infeasible";
        case SimplexResult::Status::StallRecovered: return "stall_recovered";
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
using Status = SimplexResult::Status;

constexpr double kArtificialCap = 1e12;   // artificial boxes grow x100 up to this
constexpr double kSmallDualViol = 1e-5;   // numerical dual infeasibility => cost shift

class Simplex {
public:
    Simplex(const Lp& lp, const SimplexOptions& opt)
        : A_(lp.A), opt_(opt), n_(A_.num_cols()), m_(A_.num_rows()), N_(n_ + m_),
          fac_(opt.refactor_interval), rng_(opt.seed), t0_(Clock::now()) {
        if (lp.c.size() != n_ || lp.lower.size() != n_ || lp.upper.size() != n_)
            throw std::invalid_argument("solve: c/lower/upper size must equal number of columns");
        c0_.assign(N_, 0.0);
        lb_.assign(N_, 0.0);
        ub_.assign(N_, 0.0);
        for (uint32_t j = 0; j < n_; ++j) {
            c0_[j] = lp.c[j];
            lb_[j] = lp.lower[j];
            ub_[j] = lp.upper[j];
            if (lb_[j] > ub_[j]) infeasible_bounds_ = true;
        }
        for (uint32_t i = 0; i < m_; ++i) {  // logical s_i = a_i x, column -e_i
            const auto& rd = A_.row_data()[i];
            lb_[n_ + i] = rd.sense == Sense::Le ? -kInf : rd.rhs;
            ub_[n_ + i] = rd.sense == Sense::Ge ? kInf : rd.rhs;
        }
        c_ = c0_;
        art_lo_.assign(N_, 0);
        art_up_.assign(N_, 0);
        x_.assign(N_, 0.0);
        d_.assign(N_, 0.0);
        y_.assign(m_, 0.0);
        st_.assign(N_, VarState::AtLower);
        pos_.assign(N_, -1);
        head_.resize(m_);
        w_.assign(m_, 1.0);
        alpha_.assign(N_, 0.0);
    }

    bool bounds_infeasible() const { return infeasible_bounds_; }

    void init(const Basis* warm) {
        bool ok = warm && warm->col.size() == n_ && warm->row.size() == m_;
        if (ok) {
            uint32_t basic = 0;
            for (auto s : warm->col) basic += s == VarState::Basic;
            for (auto s : warm->row) basic += s == VarState::Basic;
            ok = basic == m_;
        }
        uint32_t p = 0;
        for (uint32_t j = 0; j < N_; ++j) {
            const bool basic = ok ? (j < n_ ? warm->col[j] : warm->row[j - n_]) == VarState::Basic : j >= n_;
            if (basic) {
                st_[j] = VarState::Basic;
                pos_[j] = static_cast<int32_t>(p);
                head_[p++] = j;
            } else {
                const VarState want = ok ? (j < n_ ? warm->col[j] : warm->row[j - n_]) : VarState::AtLower;
                place_nonbasic(j, want);
            }
        }
    }

    Status run_dual() {
        if (infeasible_bounds_) return Status::Infeasible;
        recompute(true);
        double last_obj = -kInf;
        uint64_t last_progress = it_;
        for (;;) {
            if (it_ >= opt_.iteration_limit) return Status::IterationLimit;
            if (elapsed() > opt_.time_limit_s) return Status::TimeLimit;
            if (force_refactor_ || fac_.needs_refactor()) recompute(false);

            // Pricing: leaving row with largest infeasibility^2 / weight.
            int32_t r = -1;
            double best = 0;
            for (uint32_t p = 0; p < m_; ++p) {
                const uint32_t j = head_[p];
                const double inf = primal_infeasibility(j);
                if (inf <= 0) continue;
                const double score = inf * inf / w_[p];
                if (score > best) { best = score; r = static_cast<int32_t>(p); }
            }
            if (r < 0) {
                const int a = enlarge_binding_artificials();
                if (a == 1) { compute_primal(); continue; }
                if (a == 2) return Status::Unbounded;
                return Status::Optimal;
            }

            const uint32_t leaving = head_[r];
            const bool to_upper = x_[leaving] > ub_[leaving];
            const double target = to_upper ? ub_[leaving] : lb_[leaving];
            const double s = to_upper ? 1.0 : -1.0;

            // Pivot row: rho = B^-T e_r, alpha_j = rho^T a_j (row-wise through CSR).
            std::vector<double> rho(m_, 0.0);
            rho[r] = 1.0;
            fac_.btran(rho);
            compute_alpha_row(rho);

            // Ratio test: Harris two-pass + bound flipping.
            std::vector<uint32_t> flips;
            const int64_t q = ratio_test(s, std::abs(x_[leaving] - target), flips);
            if (q < 0) {
                clear_alpha();
                if (enlarge_all_artificials()) { compute_primal(); continue; }
                farkas_.assign(m_, 0.0);  // dual ray: no column j with s * rho^T a_j > 0 can repair row r
                for (uint32_t i = 0; i < m_; ++i) farkas_[i] = s * rho[i];
                return Status::Infeasible;  // dual unbounded
            }

            if (!flips.empty()) {
                std::vector<double> af(m_, 0.0);
                for (uint32_t j : flips) {
                    const bool at_lower = st_[j] == VarState::AtLower;
                    const double nv = at_lower ? ub_[j] : lb_[j];
                    const double dx = nv - x_[j];
                    x_[j] = nv;
                    st_[j] = at_lower ? VarState::AtUpper : VarState::AtLower;
                    for_col(j, [&](uint32_t i, double v) { af[i] += v * dx; });
                }
                fac_.ftran(af);
                for (uint32_t p = 0; p < m_; ++p) x_[head_[p]] -= af[p];
                bound_flips_ += flips.size();
            }

            const uint32_t qj = static_cast<uint32_t>(q);
            std::vector<double> aq(m_, 0.0);
            for_col(qj, [&](uint32_t i, double v) { aq[i] += v; });
            fac_.ftran(aq);
            const double alpha_rq = aq[r];
            if (std::abs(alpha_rq - alpha_[qj]) > 1e-6 * std::max(1.0, std::abs(alpha_rq)) && fac_.eta().size() > 0) {
                clear_alpha();
                force_refactor_ = true;  // row/column disagree: refactorize and retry
                continue;
            }
            if (std::abs(alpha_rq) < opt_.zero_tol) {
                clear_alpha();
                force_refactor_ = true;
                continue;
            }

            std::vector<double> tau;
            if (opt_.pricing == SimplexOptions::Pricing::DualSteepestEdge) {
                tau = rho;
                fac_.ftran(tau);
            }

            // Primal step.
            const double theta_p = (x_[leaving] - target) / alpha_rq;
            for (uint32_t p = 0; p < m_; ++p) x_[head_[p]] -= theta_p * aq[p];
            x_[qj] += theta_p;
            x_[leaving] = target;

            // Dual step.
            const double theta_d = d_[qj] / alpha_[qj];
            for (uint32_t j : alpha_idx_)
                if (st_[j] != VarState::Basic) d_[j] -= theta_d * alpha_[j];
            d_[qj] = 0.0;
            d_[leaving] = -theta_d;

            update_weights(static_cast<uint32_t>(r), aq, alpha_rq, rho, tau);
            clear_alpha();

            // Basis change.
            head_[r] = qj;
            pos_[qj] = r;
            pos_[leaving] = -1;
            st_[qj] = VarState::Basic;
            st_[leaving] = lb_[leaving] == ub_[leaving] ? VarState::Fixed
                                                         : (to_upper ? VarState::AtUpper : VarState::AtLower);
            fac_.update(static_cast<uint32_t>(r), aq);
            ++it_;

            // Stall detection on the dual objective.
            const double obj = objective(c_);
            if (obj > last_obj + 1e-9 * (1.0 + std::abs(obj))) {
                last_obj = obj;
                last_progress = it_;
            } else if (it_ - last_progress >= opt_.stall_window && !perturbed_) {
                perturb();
                last_progress = it_;
            }
        }
    }

    // Removes perturbations/shifts; if that breaks dual feasibility, cleans up with primal.
    Status finish_dual(Status st) {
        if (st != Status::Optimal) return st;
        if (perturbed_ || shifted_) {
            c_ = c0_;
            shifted_ = false;
            recompute_factors();
            compute_primal();
            compute_dual();
            if (max_dual_infeasibility() > opt_.feasibility_tol) {
                st = run_primal();
                if (st != Status::Optimal) return st;
            }
        }
        return perturbed_ ? Status::StallRecovered : Status::Optimal;
    }

    Status run_primal() {
        if (infeasible_bounds_) return Status::Infeasible;
        recompute_factors();
        compute_primal();
        for (;;) {
            if (it_ >= opt_.iteration_limit) return Status::IterationLimit;
            if (elapsed() > opt_.time_limit_s) return Status::TimeLimit;
            if (force_refactor_ || fac_.needs_refactor()) { recompute_factors(); compute_primal(); }

            std::vector<double> cb(m_, 0.0);
            bool phase1 = false;
            for (uint32_t p = 0; p < m_; ++p) {
                const uint32_t j = head_[p];
                if (x_[j] < lb_[j] - opt_.feasibility_tol) { cb[p] = -1; phase1 = true; }
                else if (x_[j] > ub_[j] + opt_.feasibility_tol) { cb[p] = 1; phase1 = true; }
            }
            if (!phase1)
                for (uint32_t p = 0; p < m_; ++p) cb[p] = c_[head_[p]];
            fac_.btran(cb);
            y_ = cb;

            int64_t q = -1;
            double best = 0, dq = 0;
            for (uint32_t j = 0; j < N_; ++j) {
                if (st_[j] == VarState::Basic || st_[j] == VarState::Fixed) continue;
                double dj = phase1 ? 0.0 : c_[j];
                for_col(j, [&](uint32_t i, double v) { dj -= v * y_[i]; });
                d_[j] = dj;
                const bool improving = (st_[j] == VarState::AtLower && dj < -opt_.feasibility_tol) ||
                                       (st_[j] == VarState::AtUpper && dj > opt_.feasibility_tol) ||
                                       (st_[j] == VarState::Free && std::abs(dj) > opt_.feasibility_tol);
                if (improving && std::abs(dj) > best) { best = std::abs(dj); q = j; dq = dj; }
            }
            if (q < 0) {
                if (phase1) return Status::Infeasible;
                compute_dual();
                return Status::Optimal;
            }
            const uint32_t qj = static_cast<uint32_t>(q);
            const double dir = dq < 0 ? 1.0 : -1.0;
            std::vector<double> aq(m_, 0.0);
            for_col(qj, [&](uint32_t i, double v) { aq[i] += v; });
            fac_.ftran(aq);

            double theta = ub_[qj] - lb_[qj];  // bound flip distance (inf if unbounded / free)
            if (st_[qj] == VarState::Free) theta = kInf;
            int32_t r = -1;
            double r_bound = 0, r_piv = 0;
            for (uint32_t p = 0; p < m_; ++p) {
                const double a = aq[p];
                if (std::abs(a) <= opt_.pivot_tol) continue;
                const uint32_t j = head_[p];
                const double rate = -dir * a;
                double lim, bound;
                if (rate < 0) {
                    if (x_[j] > ub_[j] + opt_.feasibility_tol) { lim = (x_[j] - ub_[j]) / -rate; bound = ub_[j]; }
                    else if (x_[j] >= lb_[j] - opt_.feasibility_tol && lb_[j] > -kInf) {
                        lim = std::max(0.0, x_[j] - lb_[j]) / -rate; bound = lb_[j];
                    } else continue;
                } else {
                    if (x_[j] < lb_[j] - opt_.feasibility_tol) { lim = (lb_[j] - x_[j]) / rate; bound = lb_[j]; }
                    else if (x_[j] <= ub_[j] + opt_.feasibility_tol && ub_[j] < kInf) {
                        lim = std::max(0.0, ub_[j] - x_[j]) / rate; bound = ub_[j];
                    } else continue;
                }
                if (lim < theta - 1e-12 || (lim <= theta + 1e-12 && r >= 0 && std::abs(a) > std::abs(r_piv))) {
                    theta = lim; r = static_cast<int32_t>(p); r_bound = bound; r_piv = a;
                }
            }
            if (theta == kInf) return Status::Unbounded;
            for (uint32_t p = 0; p < m_; ++p) x_[head_[p]] -= dir * theta * aq[p];
            x_[qj] += dir * theta;
            ++it_;
            ++primal_it_;
            if (r < 0) {  // entering variable just moves to its other bound
                st_[qj] = st_[qj] == VarState::AtLower ? VarState::AtUpper : VarState::AtLower;
                x_[qj] = st_[qj] == VarState::AtLower ? lb_[qj] : ub_[qj];
                ++bound_flips_;
                continue;
            }
            const uint32_t leaving = head_[r];
            x_[leaving] = r_bound;
            head_[r] = qj;
            pos_[qj] = r;
            pos_[leaving] = -1;
            st_[qj] = VarState::Basic;
            st_[leaving] = lb_[leaving] == ub_[leaving] ? VarState::Fixed
                                                         : (r_bound == ub_[leaving] ? VarState::AtUpper : VarState::AtLower);
            fac_.update(static_cast<uint32_t>(r), aq);
        }
    }

    SimplexResult result(Status st) {
        compute_dual();  // y is only refreshed at refactorization during iterations
        SimplexResult res;
        res.status = st;
        res.x.assign(x_.begin(), x_.begin() + n_);
        res.objective = 0;
        for (uint32_t j = 0; j < n_; ++j) res.objective += c0_[j] * x_[j];
        res.duals = y_;
        res.reduced_costs.assign(d_.begin(), d_.begin() + n_);
        res.var_state.resize(n_);
        res.basis.col.assign(st_.begin(), st_.begin() + n_);
        res.basis.row.assign(st_.begin() + n_, st_.end());
        for (uint32_t j = 0; j < n_; ++j) res.var_state[j] = static_cast<uint8_t>(st_[j]);
        res.iterations = it_;
        res.primal_iterations = primal_it_;
        res.refactorizations = refacts_;
        res.bound_flips = bound_flips_;
        res.solve_time_s = elapsed();
        res.used_artificial_bounds = used_art_;
        res.perturbed = perturbed_;
        res.farkas = farkas_;
        res.initial_dual_flips = initial_flips_;
        return res;
    }

private:
    template <class F>
    void for_col(uint32_t j, F&& f) const {
        if (j < n_) {
            const auto r = A_.col_rows(j);
            const auto v = A_.col_vals(j);
            for (size_t k = 0; k < r.size(); ++k) f(r[k], v[k]);
        } else {
            f(j - n_, -1.0);
        }
    }

    double elapsed() const { return std::chrono::duration<double>(Clock::now() - t0_).count(); }

    double primal_infeasibility(uint32_t j) const {
        if (x_[j] < lb_[j] - opt_.feasibility_tol) return lb_[j] - x_[j];
        if (x_[j] > ub_[j] + opt_.feasibility_tol) return x_[j] - ub_[j];
        return 0;
    }

    double objective(const std::vector<double>& c) const {
        double o = 0;
        for (uint32_t j = 0; j < n_; ++j) o += c[j] * x_[j];
        return o;
    }

    // Puts nonbasic j at a finite bound (preferring `want`), Fixed if l == u, else Free at 0.
    void place_nonbasic(uint32_t j, VarState want) {
        pos_[j] = -1;
        if (lb_[j] == ub_[j]) { st_[j] = VarState::Fixed; x_[j] = lb_[j]; return; }
        const bool lo = lb_[j] > -kInf, up = ub_[j] < kInf;
        if (want == VarState::AtUpper && up) { st_[j] = VarState::AtUpper; x_[j] = ub_[j]; }
        else if (lo) { st_[j] = VarState::AtLower; x_[j] = lb_[j]; }
        else if (up) { st_[j] = VarState::AtUpper; x_[j] = ub_[j]; }
        else { st_[j] = VarState::Free; x_[j] = 0; }
    }

    void recompute_factors() {
        force_refactor_ = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::vector<SparseCol> cols(m_);
            for (uint32_t p = 0; p < m_; ++p) for_col(head_[p], [&](uint32_t i, double v) { cols[p].push_back({i, v}); });
            ++refacts_;
            if (fac_.factorize(m_, cols)) return;
            // Singular basis: replace unpivoted positions by logicals of unpivoted rows.
            const auto sp = fac_.lu().singular_positions();
            const auto ur = fac_.lu().unpivoted_rows();
            for (size_t k = 0; k < std::min(sp.size(), ur.size()); ++k) {
                const uint32_t p = sp[k], old = head_[p], slack = n_ + ur[k];
                if (pos_[slack] >= 0) continue;
                place_nonbasic(old, x_[old] - lb_[old] <= ub_[old] - x_[old] ? VarState::AtLower : VarState::AtUpper);
                head_[p] = slack;
                pos_[slack] = static_cast<int32_t>(p);
                st_[slack] = VarState::Basic;
            }
        }
        throw std::runtime_error("dual simplex: could not repair singular basis");
    }

    void compute_primal() {
        std::vector<double> rhs(m_, 0.0);
        for (uint32_t j = 0; j < N_; ++j)
            if (st_[j] != VarState::Basic && x_[j] != 0.0) for_col(j, [&](uint32_t i, double v) { rhs[i] -= v * x_[j]; });
        fac_.ftran(rhs);
        for (uint32_t p = 0; p < m_; ++p) x_[head_[p]] = rhs[p];
    }

    void compute_dual() {
        std::vector<double> cb(m_);
        for (uint32_t p = 0; p < m_; ++p) cb[p] = c_[head_[p]];
        fac_.btran(cb);
        y_ = cb;
        for (uint32_t j = 0; j < N_; ++j) {
            if (st_[j] == VarState::Basic) { d_[j] = 0; continue; }
            double dj = c_[j];
            for_col(j, [&](uint32_t i, double v) { dj -= v * y_[i]; });
            d_[j] = dj;
        }
    }

    double max_dual_infeasibility() const {
        double worst = 0;
        for (uint32_t j = 0; j < N_; ++j) {
            switch (st_[j]) {
                case VarState::AtLower: worst = std::max(worst, -d_[j]); break;
                case VarState::AtUpper: worst = std::max(worst, d_[j]); break;
                case VarState::Free: worst = std::max(worst, std::abs(d_[j])); break;
                default: break;
            }
        }
        return worst;
    }

    // Restores dual feasibility of nonbasic variables: flip to the other finite bound,
    // shift the cost for small numerical violations, or (initial basis only, generic
    // LPs) use an artificial bound. Returns true if primal values changed.
    bool make_dual_feasible(bool initial) {
        bool moved = false;
        const double tol = opt_.feasibility_tol;
        for (uint32_t j = 0; j < N_; ++j) {
            const VarState s = st_[j];
            if (s == VarState::Basic || s == VarState::Fixed) continue;
            const double dj = d_[j];
            const bool bad = (s == VarState::AtLower && dj < -tol) || (s == VarState::AtUpper && dj > tol) ||
                             s == VarState::Free;
            if (!bad) continue;
            const bool need_lower = dj >= 0;
            if (!initial && s != VarState::Free && std::abs(dj) <= kSmallDualViol) {
                c_[j] -= dj;
                d_[j] = 0;
                shifted_ = true;
                continue;
            }
            if (need_lower) {
                if (lb_[j] == -kInf) { lb_[j] = -opt_.artificial_bound; art_lo_[j] = 1; used_art_ = true; }
                st_[j] = VarState::AtLower;
                x_[j] = lb_[j];
            } else {
                if (ub_[j] == kInf) { ub_[j] = opt_.artificial_bound; art_up_[j] = 1; used_art_ = true; }
                st_[j] = VarState::AtUpper;
                x_[j] = ub_[j];
            }
            moved = true;
            if (initial) ++initial_flips_;
        }
        return moved;
    }

    void recompute(bool initial) {
        recompute_factors();
        compute_primal();
        compute_dual();
        if (make_dual_feasible(initial)) compute_primal();
    }

    // At optimum of the boxed problem: artificial bounds with nonzero reduced cost are
    // binding => enlarge. 0 = none binding (optimal for the real LP), 1 = enlarged, 2 = cap hit.
    int enlarge_binding_artificials() {
        int result = 0;
        for (uint32_t j = 0; j < N_; ++j) {
            const bool lo = st_[j] == VarState::AtLower && art_lo_[j];
            const bool up = st_[j] == VarState::AtUpper && art_up_[j];
            if (!lo && !up) continue;
            if (std::abs(d_[j]) <= opt_.feasibility_tol) continue;
            double& b = lo ? lb_[j] : ub_[j];
            if (std::abs(b) >= kArtificialCap) return 2;
            b *= 100;
            x_[j] = b;
            result = 1;
        }
        return result;
    }

    bool enlarge_all_artificials() {
        bool any = false;
        for (uint32_t j = 0; j < N_; ++j) {
            for (int side = 0; side < 2; ++side) {
                if (!(side == 0 ? art_lo_[j] : art_up_[j])) continue;
                double& b = side == 0 ? lb_[j] : ub_[j];
                if (std::abs(b) >= kArtificialCap) return false;
                b *= 100;
                if ((side == 0 && st_[j] == VarState::AtLower) || (side == 1 && st_[j] == VarState::AtUpper)) x_[j] = b;
                any = true;
            }
        }
        return any;
    }

    void compute_alpha_row(const std::vector<double>& rho) {
        for (uint32_t i = 0; i < m_; ++i) {
            const double ri = rho[i];
            if (ri == 0.0) continue;
            const auto cols = A_.row_cols(i);
            const auto vals = A_.row_vals(i);
            for (size_t k = 0; k < cols.size(); ++k) touch(cols[k], ri * vals[k]);
            touch(n_ + i, -ri);
        }
    }
    void touch(uint32_t j, double v) {
        if (alpha_[j] == 0.0) alpha_idx_.push_back(j);
        alpha_[j] += v;
        if (alpha_[j] == 0.0) alpha_[j] = 1e-300;  // keep index marked (value below any tolerance)
    }
    void clear_alpha() {
        for (uint32_t j : alpha_idx_) alpha_[j] = 0.0;
        alpha_idx_.clear();
    }

    // Returns entering variable (or -1 if dual unbounded); fills BFRT flip set.
    int64_t ratio_test(double s, double slope, std::vector<uint32_t>& flips) {
        struct Cand { uint32_t j; double t, at; bool done; };
        std::vector<Cand> cand;
        for (uint32_t j : alpha_idx_) {
            const VarState st = st_[j];
            if (st != VarState::AtLower && st != VarState::AtUpper) continue;  // basic / fixed (no free in dual)
            const double at = s * alpha_[j];
            if ((st == VarState::AtLower && at > opt_.pivot_tol) || (st == VarState::AtUpper && at < -opt_.pivot_tol))
                cand.push_back({j, d_[j] / at, at, false});
        }
        size_t left = cand.size();
        while (left > 0) {
            double theta_max = kInf;  // Harris pass 1: relaxed bound
            for (const auto& c : cand) {
                if (c.done) continue;
                const double relaxed = st_[c.j] == VarState::AtLower ? (d_[c.j] + opt_.harris_delta) / c.at
                                                                     : (d_[c.j] - opt_.harris_delta) / c.at;
                theta_max = std::min(theta_max, relaxed);
            }
            double drop = 0;
            int64_t q = -1;
            double best = -1;
            for (const auto& c : cand) {  // pass 2: group within theta_max, largest |alpha|
                if (c.done || c.t > theta_max) continue;
                drop += std::abs(c.at) * (ub_[c.j] - lb_[c.j]);
                if (std::abs(c.at) > best || (std::abs(c.at) == best && c.j < static_cast<uint32_t>(q))) {
                    best = std::abs(c.at);
                    q = c.j;
                }
            }
            if (q < 0) break;
            if (opt_.bound_flipping && slope - drop > 0) {  // flip the whole group, keep going
                for (auto& c : cand)
                    if (!c.done && c.t <= theta_max) { c.done = true; --left; flips.push_back(c.j); }
                slope -= drop;
                continue;
            }
            if (d_[q] * s * alpha_[q] < 0) {  // negative step from a slightly infeasible d_q: shift cost
                c_[q] -= d_[q];
                d_[q] = 0;
                shifted_ = true;
            }
            return q;
        }
        // Every candidate flipped and slope still positive, or no candidates: dual unbounded.
        // Undo nothing: flips were not applied yet.
        flips.clear();
        return -1;
    }

    void update_weights(uint32_t r, const std::vector<double>& aq, double alpha_rq, const std::vector<double>& rho,
                        const std::vector<double>& tau) {
        if (opt_.pricing == SimplexOptions::Pricing::Devex) {
            const double wr = w_[r];
            bool reset = false;
            for (uint32_t p = 0; p < m_; ++p) {
                if (p == r || aq[p] == 0.0) continue;
                const double ratio = aq[p] / alpha_rq;
                w_[p] = std::max(w_[p], ratio * ratio * wr);
                reset |= w_[p] > 1e7;
            }
            w_[r] = std::max(wr / (alpha_rq * alpha_rq), 1.0);
            if (reset) std::fill(w_.begin(), w_.end(), 1.0);  // new reference framework
        } else {
            double wr = 0;
            for (double v : rho) wr += v * v;  // exact ||rho_r||^2
            for (uint32_t p = 0; p < m_; ++p) {
                if (p == r || aq[p] == 0.0) continue;
                const double ratio = aq[p] / alpha_rq;
                w_[p] = std::max(w_[p] - 2 * ratio * tau[p] + ratio * ratio * wr, 1e-4);
            }
            w_[r] = std::max(wr / (alpha_rq * alpha_rq), 1e-4);
        }
    }

    void perturb() {
        std::uniform_real_distribution<double> U(0.5, 1.0);
        for (uint32_t j = 0; j < N_; ++j) {
            const double eps = 1e-6 * (1.0 + std::abs(c0_[j])) * U(rng_);
            if (st_[j] == VarState::AtLower) { c_[j] += eps; d_[j] += eps; }
            else if (st_[j] == VarState::AtUpper) { c_[j] -= eps; d_[j] -= eps; }
        }
        perturbed_ = true;
    }

    const CscMatrix& A_;
    SimplexOptions opt_;
    uint32_t n_, m_, N_;
    std::vector<double> c0_, c_, lb_, ub_, x_, d_, y_, w_, alpha_;
    std::vector<uint8_t> art_lo_, art_up_;
    std::vector<VarState> st_;
    std::vector<int32_t> pos_;
    std::vector<uint32_t> head_, alpha_idx_;
    BasisFactor fac_;
    std::mt19937_64 rng_;
    Clock::time_point t0_;
    uint64_t it_ = 0, primal_it_ = 0, refacts_ = 0, bound_flips_ = 0;
    std::vector<double> farkas_;
    uint32_t initial_flips_ = 0;
    bool infeasible_bounds_ = false, used_art_ = false, perturbed_ = false, shifted_ = false, force_refactor_ = false;
};

const Lp& merged(const Lp& lp, Lp& storage) {
    if (lp.A.pending_rows() == 0) return lp;
    storage = lp;
    storage.A.ensure_merged();
    return storage;
}

}  // namespace

SimplexResult solve_dual(const Lp& lp_in, const SimplexOptions& opt, const Basis* warm) {
    Lp storage;
    const Lp& lp = merged(lp_in, storage);
    Simplex s(lp, opt);
    s.init(warm);
    const auto st = s.finish_dual(s.run_dual());
    return s.result(st);
}

SimplexResult solve_primal(const Lp& lp_in, const SimplexOptions& opt, const Basis* warm) {
    Lp storage;
    const Lp& lp = merged(lp_in, storage);
    Simplex s(lp, opt);
    s.init(warm);
    return s.result(s.run_primal());
}

}  // namespace apex
