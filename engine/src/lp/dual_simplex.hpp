#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "lp/csc.hpp"
#include "lp/eta.hpp"

namespace apex {

inline constexpr double kInf = std::numeric_limits<double>::infinity();

// minimize c^T x  s.t.  rows of A (sense, rhs from A.row_data()),  lower <= x <= upper.
struct Lp {
    CscMatrix A;
    std::vector<double> c, lower, upper;
};

enum class VarState : uint8_t { Basic, AtLower, AtUpper, Fixed, Free };

// Warm-start basis: state of every structural column and every row's logical variable.
// Exactly num_rows entries must be Basic.
struct Basis {
    std::vector<VarState> col, row;
};

struct SimplexOptions {
    enum class Pricing { Devex, DualSteepestEdge } pricing = Pricing::Devex;
    double feasibility_tol = 1e-7;   // 1e-9 in final-proof mode
    double zero_tol = 1e-11;
    double pivot_tol = 1e-5;
    double harris_delta = 1e-7;
    bool bound_flipping = true;      // BFRT
    uint32_t refactor_interval = 100;  // dynamic, see M1.3
    uint64_t iteration_limit = UINT64_MAX;
    double time_limit_s = INFINITY;
    uint32_t stall_window = 500;     // iterations without dual-bound progress
    uint64_t seed = 1;               // deterministic cost perturbation
    // Artificial box for dual phase 1 (operator-approved, generic LPs only): used only
    // when a nonbasic variable needs an infinite bound to be dual feasible. Never
    // triggered by TSP LPs (c >= 0, 0 <= x <= 1).
    double artificial_bound = 1e6;
    // M4.6: basis update (Forrest-Tomlin default, PFI fallback) and hypersparse FTRAN/BTRAN.
    BasisUpdateKind basis_update = BasisUpdateKind::ForrestTomlin;
    bool hypersparse = false;  // measured slower on stress + fl3795 (README §9); --hypersparse
};

struct SimplexResult {
    enum class Status { Optimal, IterationLimit, TimeLimit, Unbounded, Infeasible, StallRecovered } status;
    double objective = 0;              // primal value c^T x (= dual bound while dual feasible)
    std::vector<double> x;             // structural values
    std::vector<double> duals;         // row duals y
    std::vector<double> reduced_costs; // structural d = c - A^T y
    std::vector<uint8_t> var_state;    // per column: static_cast<uint8_t>(VarState)
    Basis basis;
    uint64_t iterations = 0, refactorizations = 0, bound_flips = 0, primal_iterations = 0;
    double solve_time_s = 0;
    bool used_artificial_bounds = false;
    bool perturbed = false;
    // Status Infeasible from the dual ratio test: ray f over the rows. A column a (new or
    // existing, at lower bound) can only restore feasibility if f^T a > 0; none exists => the
    // LP stays infeasible after adding any such columns (Farkas pricing, MASTER §5.6).
    std::vector<double> farkas;
    uint32_t initial_dual_flips = 0;  // nonbasic moves needed to make the start dual feasible
    uint64_t peak_factor_nnz = 0;     // max nnz(L) + nnz(U) over the refactorizations (memory budget)
};

// Bounded dual simplex: Devex/DSE pricing, Harris two-pass ratio test + BFRT,
// PFI updates with dynamic refactorization, stall recovery by cost perturbation,
// primal simplex clean-up when removing perturbations/shifts breaks dual feasibility.
SimplexResult solve_dual(const Lp& lp, const SimplexOptions& opt, const Basis* warm = nullptr);

// Primal simplex (composite phase 1 on infeasibilities + phase 2, Dantzig pricing).
// Robustness fallback; exposed for tests.
SimplexResult solve_primal(const Lp& lp, const SimplexOptions& opt, const Basis* warm = nullptr);

const char* status_name(SimplexResult::Status s);

}  // namespace apex
