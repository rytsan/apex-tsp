// APEX-TSP engine entry point (Sprint 0: scalable mode = primal engine only).
#include <optional>
#include <chrono>
#include <cstdio>
#include <exception>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "app/telemetry.hpp"
#include "bnc/checkpoint.hpp"
#include "core/instance.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "graph/candidate_graph.hpp"
#include "decomp/patching.hpp"
#include "hk/held_karp.hpp"
#include "primal/lk_lite.hpp"
#include "primal/two_opt.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

std::string fmt(double v, int prec = 3) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

int run(const apex::Options& opt, apex::TelemetrySink& tel) {
    const auto t0 = Clock::now();
    const auto inst = apex::read_tsplib(opt.instance);
    tel.event("instance_loaded", {{"name", inst.name}, {"n", std::to_string(inst.n)},
                                  {"metric", apex::metric_name(inst.metric)}, {"ms", fmt(since(t0) * 1e3)}});
    tel.event("config", {{"mode", opt.mode}, {"seed", std::to_string(opt.seed)},
                         {"threads", std::to_string(opt.threads)}, {"threads_used", std::to_string(opt.mode == "scalable" && inst.n > 2 * opt.leaf_size ? opt.threads : 1)},
                         {"mem_limit_mb", std::to_string(opt.mem_limit_mb)}, {"mem_limit_enforced", "false"},
                         {"time_limit_s", fmt(opt.time_limit_s)}, {"knn", std::to_string(opt.knn)}});

    std::mt19937_64 rng(opt.seed);
    double prior_wall = 0;
    apex::Tour tour;
    if (opt.resume) {
        const auto ck = apex::read_checkpoint(opt.checkpoint, inst);
        std::istringstream(ck.rng_state) >> rng;
        prior_wall = ck.wall_s;
        tour = ck.best;
        tel.event("resumed", {{"checkpoint", opt.checkpoint}, {"ub", std::to_string(tour.cost)},
                              {"prior_wall_s", fmt(prior_wall)}});
    } else {
        tour = apex::hilbert_tour(inst, apex::hilbert_order(inst));
        tel.event("ub0", {{"source", "hilbert"}, {"ub", std::to_string(tour.cost)}, {"ms", fmt(since(t0) * 1e3)}});
    }

    const auto tg = Clock::now();
    const auto g = apex::build_candidate_graph(inst, {opt.knn, true});
    tel.event("candidate_graph", {{"edges", std::to_string(g.edges.size())},
                                  {"fallback_edges", std::to_string(g.fallback_edges)},
                                  {"ms", fmt(since(tg) * 1e3)}});

    // heuristic: KD-tree decomposition + patching gives the scalable-mode start tour (UB only;
    // no bound from this phase exists or is used).
    if (opt.mode == "scalable" && !opt.resume && inst.n > 2 * opt.leaf_size) {
        apex::DecompConfig dc;
        dc.max_leaf = opt.leaf_size;
        dc.leaf.exact_limit = opt.leaf_exact_limit;
        dc.leaf.threads = opt.threads;
        dc.leaf.seed = rng();
        apex::DecompStats ds;
        auto dt = apex::decompose_and_patch(inst, g, dc, &ds);
        tel.event("decomposition", {{"phase", "decomposition (heuristic)"}, {"heuristic", "true"},
                                    {"leaves", std::to_string(ds.leaf.leaves)},
                                    {"leaf_min", std::to_string(ds.leaf.min_size)}, {"leaf_max", std::to_string(ds.leaf.max_size)},
                                    {"exact_leaves", std::to_string(ds.leaf.exact_leaves)},
                                    {"patched", std::to_string(ds.patched_cost)}, {"ub", std::to_string(ds.final_cost)},
                                    {"leaf_ms", fmt(ds.leaf_s * 1e3)}, {"patch_ms", fmt(ds.patch_s * 1e3)}});
        if (dt.cost < tour.cost) tour = std::move(dt);
    }

    apex::TwoOptConfig cfg;
    cfg.seed = rng();
    if (opt.time_limit_s > 0) {
        cfg.time_limit_s = opt.time_limit_s - prior_wall - since(t0);
        if (cfg.time_limit_s <= 0) cfg.time_limit_s = 1e-9;  // budget already spent
    }
    const auto tl = Clock::now();
    const auto st = apex::two_opt_oropt(tour, inst, g, cfg);
    tel.event("two_opt", {{"heuristic", "true"}, {"ub", std::to_string(tour.cost)},
                          {"moves_2opt", std::to_string(st.moves_2opt)},
                          {"moves_oropt", std::to_string(st.moves_oropt)}, {"rounds", std::to_string(st.rounds)},
                          {"neighbors", std::to_string(st.final_neighbors)},
                          {"time_limit_hit", st.time_limit_hit ? "true" : "false"}, {"ms", fmt(since(tl) * 1e3)}});

    // Held-Karp (M4.4): Lagrangian LB of the scalable mode (lb_source lagrangian); ascent gets
    // 10% of the remaining budget, the exact complete-graph 1-tree comes on top.
    std::optional<int64_t> lb;
    if (opt.mode == "scalable" && opt.time_limit_s > 0) {
        apex::HkConfig hc;
        hc.time_limit_s = 0.1 * (opt.time_limit_s - prior_wall - since(t0));
        if (hc.time_limit_s > 0) {
            const auto hk = apex::held_karp(inst, g, tour.cost, hc);
            lb = std::min<int64_t>(hk.lb_int, tour.cost);
            tel.event("held_karp", {{"lb_source", "lagrangian"}, {"lb", fmt(hk.lb, 4)}, {"lb_int", std::to_string(hk.lb_int)},
                                    {"iterations", std::to_string(hk.iterations)},
                                    {"ascent_ms", fmt(hk.ascent_s * 1e3)}, {"exact_ms", fmt(hk.exact_s * 1e3)}});
        }
    }

    // ILS (M4.1, heuristic) fills the rest of the time budget; without --time-limit the run stops
    // after 2-opt as before.
    if (opt.time_limit_s > 0) {
        apex::IlsConfig ic;
        ic.seed = rng();
        ic.time_limit_s = opt.time_limit_s - prior_wall - since(t0);
        if (ic.time_limit_s > 0) {
            const auto is = apex::iterated_local_search(tour, inst, g, ic);
            tel.event("ils", {{"heuristic", "true"}, {"ub", std::to_string(tour.cost)},
                              {"start", std::to_string(is.start_cost)}, {"iterations", std::to_string(is.iterations)},
                              {"improvements", std::to_string(is.improvements)},
                              {"strong_kicks", std::to_string(is.strong_kicks)}, {"ms", fmt(is.seconds * 1e3)}});
        }
    }

    if (apex::tour_cost(inst, tour) != tour.cost) throw std::runtime_error("internal error: tour cost drift");

    const double wall = prior_wall + since(t0);
    if (!opt.out.empty()) apex::write_tour_file(opt.out, inst, tour, "APEX-TSP length " + std::to_string(tour.cost));
    if (!opt.checkpoint.empty()) {
        apex::CheckpointV1 ck;
        ck.instance_hash = apex::instance_hash(inst);
        ck.instance_name = inst.name;
        ck.mode = opt.mode;
        ck.seed = opt.seed;
        ck.knn = opt.knn;
        ck.time_limit_s = opt.time_limit_s;
        std::ostringstream rs;
        rs << rng;
        ck.rng_state = rs.str();
        ck.wall_s = wall;
        ck.best = tour;
        apex::write_checkpoint(opt.checkpoint, ck);
        tel.event("checkpoint_written", {{"dir", opt.checkpoint}});
    }
    apex::Report rep;
    rep.instance = inst.name;
    rep.mode = opt.mode;
    rep.ub = tour.cost;
    rep.wall_s = wall;
    rep.seed = opt.seed;
    rep.threads = 1;
    rep.ram_peak_bytes = apex::peak_rss_bytes();
    if (lb) {
        rep.lb = *lb;
        rep.lb_source = "lagrangian";
    }
    if (!opt.report.empty()) apex::write_report_file(opt.report, rep);
    tel.event("done", {{"mode", opt.mode}, {"ub", std::to_string(tour.cost)}, {"lb", lb ? std::to_string(*lb) : "null"},
                       {"gap_pct", lb ? fmt(100.0 * (tour.cost - *lb) / tour.cost, 4) : "null"},
                       {"lb_source", lb ? "lagrangian" : "null"}, {"wall_s", fmt(wall)},
                       {"ram_peak_bytes", std::to_string(rep.ram_peak_bytes)}});
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    apex::LogSink tel(stdout);
    apex::Options opt;
    try {
        opt = apex::parse_cli(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const apex::CliError& e) {
        tel.event("error", {{"kind", "cli"}, {"msg", e.what()}});
        std::fputs(apex::kUsage, stderr);
        return 2;
    }
    if (opt.mode == "exact") {
        tel.event("error", {{"kind", "not_implemented"}, {"msg", "--mode exact is not implemented until Sprint 3"}});
        return 3;
    }
    try {
        return run(opt, tel);
    } catch (const std::exception& e) {
        tel.event("error", {{"kind", "runtime"}, {"msg", e.what()}});
        return 1;
    }
}
