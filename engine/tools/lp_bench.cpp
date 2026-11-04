// LP benchmark harness (Sprint 1, Band E1): Netlib LPs + TSP-shaped stress LP.
// HiGHS is never linked: the comparison runs the external solver on the same MPS files
// (see docs/LP_GATE.md).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/instance.hpp"
#include "core/tour_io.hpp"
#include "geo/grid.hpp"
#include "graph/candidate_graph.hpp"
#include "lp/dual_simplex.hpp"
#include "lp/mps.hpp"

namespace {

struct NetlibRef {
    const char* file;
    double optimum;  // https://www.netlib.org/lp/data/readme (PROBLEM SUMMARY TABLE)
};
constexpr NetlibRef kNetlib[] = {
    {"adlittle", 2.2549496316E+05}, {"25fv47", 5.5018458883E+03}, {"sc205", -5.2202061212E+01},
    {"share2b", -4.1573224074E+02}, {"grow22", -1.6083433648E+08},
};

// pr1002-shaped stress LP: degree rows x(delta(i)) = 2 over the candidate graph, 0 <= x <= 1,
// plus `extra` SEC-shaped rows x(delta(S)) >= 2 for spatially local random node sets S.
apex::Lp stress_lp(const apex::Instance& in, uint32_t extra, uint64_t seed) {
    const auto g = apex::build_candidate_graph(in);
    const uint32_t n = in.n, m_edges = static_cast<uint32_t>(g.edges.size());
    std::vector<apex::Triplet> t;
    apex::Lp lp;
    for (uint32_t e = 0; e < m_edges; ++e) {
        t.push_back({g.edges[e].u, e, 1.0});
        t.push_back({g.edges[e].v, e, 1.0});
        lp.c.push_back(g.edges[e].cost);
    }
    lp.A = apex::CscMatrix::from_triplets(n, m_edges, t, std::vector<apex::RowData>(n, {apex::Sense::Eq, 2.0}));
    lp.lower.assign(m_edges, 0.0);
    lp.upper.assign(m_edges, 1.0);

    const apex::SpatialGrid grid(in);
    std::mt19937_64 rng(seed);
    std::vector<uint32_t> nb;
    std::vector<uint8_t> in_s(n, 0);
    std::vector<apex::Row> rows;
    for (uint32_t k = 0; k < extra; ++k) {
        const uint32_t center = static_cast<uint32_t>(rng() % n);
        const uint32_t size = 3 + static_cast<uint32_t>(rng() % 48);  // |S| in [4, 51]
        grid.knn(center, size, nb);
        nb.push_back(center);
        for (uint32_t v : nb) in_s[v] = 1;
        apex::Row row;
        row.sense = apex::Sense::Ge;
        row.rhs = 2;
        for (uint32_t e = 0; e < m_edges; ++e)
            if (in_s[g.edges[e].u] != in_s[g.edges[e].v]) row.entries.push_back({e, 1.0});
        for (uint32_t v : nb) in_s[v] = 0;
        rows.push_back(std::move(row));
    }
    lp.A.append_rows(rows);
    lp.A.ensure_merged();
    return lp;
}

void write_mps(const std::string& path, const apex::Lp& lp, const std::string& name) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error(path + ": cannot open for writing");
    f.precision(17);
    const auto& A = lp.A;
    f << "NAME " << name << "\nROWS\n N OBJ\n";
    for (uint32_t i = 0; i < A.num_rows(); ++i) {
        const auto s = A.row_data()[i].sense;
        f << ' ' << (s == apex::Sense::Eq ? 'E' : s == apex::Sense::Le ? 'L' : 'G') << " R" << i << '\n';
    }
    f << "COLUMNS\n";
    for (uint32_t j = 0; j < A.num_cols(); ++j) {
        if (lp.c[j] != 0) f << " C" << j << " OBJ " << lp.c[j] << '\n';
        const auto r = A.col_rows(j);
        const auto v = A.col_vals(j);
        for (size_t k = 0; k < r.size(); ++k) f << " C" << j << " R" << r[k] << ' ' << v[k] << '\n';
    }
    f << "RHS\n";
    for (uint32_t i = 0; i < A.num_rows(); ++i)
        if (A.row_data()[i].rhs != 0) f << " RHS R" << i << ' ' << A.row_data()[i].rhs << '\n';
    f << "BOUNDS\n";
    for (uint32_t j = 0; j < A.num_cols(); ++j) {
        const double lo = lp.lower[j], up = lp.upper[j];
        if (lo == up) { f << " FX BND C" << j << ' ' << lo << '\n'; continue; }
        if (lo == -apex::kInf && up == apex::kInf) { f << " FR BND C" << j << '\n'; continue; }
        if (lo == -apex::kInf) f << " MI BND C" << j << '\n';
        else if (lo != 0) f << " LO BND C" << j << ' ' << lo << '\n';
        if (up != apex::kInf) f << " UP BND C" << j << ' ' << up << '\n';
    }
    f << "ENDATA\n";
}

struct Args {
    std::string netlib_dir = "data/netlib", tsp = "data/pr1002.tsp", write_mps_path;
    uint32_t stress_rows = 5000;
    uint64_t seed = 1;
    apex::SimplexOptions::Pricing pricing = apex::SimplexOptions::Pricing::Devex;
    double time_limit = 3600;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + f);
            return argv[++i];
        };
        if (f == "--netlib-dir") a.netlib_dir = val();
        else if (f == "--tsp") a.tsp = val();
        else if (f == "--stress-rows") a.stress_rows = static_cast<uint32_t>(std::stoul(val()));
        else if (f == "--seed") a.seed = std::stoull(val());
        else if (f == "--write-mps") a.write_mps_path = val();
        else if (f == "--time-limit") a.time_limit = std::stod(val());
        else if (f == "--pricing") {
            const auto p = val();
            if (p == "devex") a.pricing = apex::SimplexOptions::Pricing::Devex;
            else if (p == "dse") a.pricing = apex::SimplexOptions::Pricing::DualSteepestEdge;
            else throw std::runtime_error("--pricing must be devex or dse");
        } else throw std::runtime_error("unknown flag '" + f + "'");
    }
    return a;
}

void row(const char* name, uint32_t m, uint32_t n, const apex::SimplexResult& r, double obj, const char* ref_s,
         const char* rel_s) {
    std::printf("%-14s %6u %6u  %-15s %22.10e %22s %10s %8llu %6llu %8llu %9.3f %10.0f %8.1f\n", name, m, n,
                apex::status_name(r.status), obj, ref_s, rel_s, static_cast<unsigned long long>(r.iterations),
                static_cast<unsigned long long>(r.refactorizations), static_cast<unsigned long long>(r.bound_flips),
                r.solve_time_s, r.solve_time_s > 0 ? r.iterations / r.solve_time_s : 0.0,
                apex::peak_rss_bytes() / 1048576.0);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args a = parse(argc, argv);
        apex::SimplexOptions opt;
        opt.pricing = a.pricing;
        opt.time_limit_s = a.time_limit;
        std::printf("pricing=%s seed=%llu\n", a.pricing == apex::SimplexOptions::Pricing::Devex ? "devex" : "dse",
                    static_cast<unsigned long long>(a.seed));
        std::printf("%-14s %6s %6s  %-15s %22s %22s %10s %8s %6s %8s %9s %10s %8s\n", "lp", "rows", "cols", "status",
                    "objective", "reference", "rel_err", "iters", "refact", "flips", "time_s", "iters/s", "rss_MB");
        int failures = 0;
        double total_time = 0;
        uint64_t total_iters = 0;
        for (const auto& ref : kNetlib) {
            const auto m = apex::read_mps(a.netlib_dir + "/" + ref.file + ".mps");
            const auto r = apex::solve_dual(m.lp, opt);
            const double obj = r.objective + m.obj_offset;
            const double rel = std::abs(obj - ref.optimum) / std::max(1.0, std::abs(ref.optimum));
            char ref_s[32], rel_s[32];
            std::snprintf(ref_s, sizeof ref_s, "%.10e", ref.optimum);
            std::snprintf(rel_s, sizeof rel_s, "%.2e", rel);
            row(m.name.c_str(), m.lp.A.num_rows(), m.lp.A.num_cols(), r, obj, ref_s, rel_s);
            const bool ok = (r.status == apex::SimplexResult::Status::Optimal ||
                             r.status == apex::SimplexResult::Status::StallRecovered) && rel <= 1e-6;
            failures += !ok;
            total_time += r.solve_time_s;
            total_iters += r.iterations;
        }
        std::printf("netlib_total time_s=%.3f iterations=%llu objective_failures=%d\n", total_time,
                    static_cast<unsigned long long>(total_iters), failures);

        const auto inst = apex::read_tsplib(a.tsp);
        const auto lp = stress_lp(inst, a.stress_rows, a.seed);
        if (!a.write_mps_path.empty()) write_mps(a.write_mps_path, lp, "STRESS_" + inst.name);
        const auto r = apex::solve_dual(lp, opt);
        const std::string name = "stress_" + inst.name;
        row(name.c_str(), lp.A.num_rows(), lp.A.num_cols(), r, r.objective, "-", "-");
        std::printf("stress nnz=%zu used_artificial_bounds=%d\n", lp.A.nnz(), r.used_artificial_bounds ? 1 : 0);
        const bool stress_ok = r.status == apex::SimplexResult::Status::Optimal ||
                               r.status == apex::SimplexResult::Status::StallRecovered;
        return failures == 0 && stress_ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
