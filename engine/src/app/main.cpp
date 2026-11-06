// APEX-TSP engine entry point (Sprint 0: scalable mode = primal engine only).
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "app/mem.hpp"
#include "app/telemetry.hpp"
#include "bnc/checkpoint.hpp"
#include "core/instance.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "graph/candidate_graph.hpp"
#include "bnc/tree.hpp"
#include "decomp/pipeline.hpp"
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
    const uint64_t mem_limit = opt.mem_limit_mb << 20;
    const auto enf = apex::enforce_memory_limit(mem_limit);  // OS backstop; the guards below stop first
    const auto inst = apex::read_tsplib(opt.instance);
    tel.event("instance_loaded", {{"name", inst.name}, {"n", std::to_string(inst.n)},
                                  {"metric", apex::metric_name(inst.metric)}, {"ms", fmt(since(t0) * 1e3)}});
    tel.event("config", {{"mode", opt.mode}, {"seed", std::to_string(opt.seed)},
                         {"threads", std::to_string(opt.threads)}, {"threads_used", std::to_string(opt.threads)},
                         {"mem_limit_mb", std::to_string(opt.mem_limit_mb)}, {"mem_limit_enforced", enf.enforced ? "true" : "false"},
                         {"mem_limit_method", enf.method},
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

    // Graceful MEM_LIMIT (M4.5): guards at phase boundaries (RSS >= 90% of --mem-limit-mb) and
    // allocation failures under the OS cap both end in: best tour so far -> checkpoint + report.
    apex::Tour best = tour;
    std::string status = "done";
    std::optional<int64_t> lb;
    try {
    apex::check_memory(mem_limit, "start");
    const auto tg = Clock::now();
    auto g = apex::build_candidate_graph(inst, {opt.knn, true});
    apex::check_memory(mem_limit, "candidate_graph");
    tel.event("candidate_graph", {{"edges", std::to_string(g.edges.size())},
                                  {"fallback_edges", std::to_string(g.fallback_edges)},
                                  {"ms", fmt(since(tg) * 1e3)}});

    // heuristic (M4.7): density clusters solved in parallel, merged level by level with
    // re-evaluation (or greedy + 2-opt for small n). UB only; no bound from this phase exists.
    if (!opt.resume) {
        apex::DecompConfig dc;
        dc.max_leaf = opt.leaf_size;
        dc.leaf.exact_limit = opt.leaf_exact_limit;
        dc.leaf.threads = opt.threads;
        dc.leaf.seed = rng();
        apex::ConstructStats cs;
        auto dt = apex::construct_tour(inst, g, dc, &cs);
        const auto& ds = cs.decomp;
        tel.event("construct", {{"phase", cs.decomposed ? "decomposition (heuristic)" : "greedy (heuristic)"}, {"heuristic", "true"},
                                {"leaves", std::to_string(ds.leaf.leaves)}, {"levels", std::to_string(ds.levels)},
                                {"leaf_min", std::to_string(ds.leaf.min_size)}, {"leaf_max", std::to_string(ds.leaf.max_size)},
                                {"exact_leaves", std::to_string(ds.leaf.exact_leaves)}, {"leaf_sum", std::to_string(ds.leaf_sum)},
                                {"stitch_cost", std::to_string(ds.stitch_cost)}, {"reeval_gain", std::to_string(ds.reeval_gain)},
                                {"ub", std::to_string(dt.cost)}, {"ms", fmt(cs.seconds * 1e3)}});
        if (dt.cost < tour.cost) tour = std::move(dt);
        best = tour;
        apex::check_memory(mem_limit, "construct");
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
    best = tour;
    apex::check_memory(mem_limit, "two_opt");

    // Held-Karp (M4.4): Lagrangian LB of the scalable mode (lb_source lagrangian); ascent gets
    // 10% of the remaining budget, the exact complete-graph 1-tree comes on top.
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
        apex::check_memory(mem_limit, "held_karp");
    }

    // Polish (M4.1 ILS; M4.7 parallel segments with --threads > 1). Scalable: the rest of the
    // budget. Exact: a warm-up (10% of the budget, or 2 iterations per node without a limit)
    // whose tour is the UB the branch-and-cut starts from.
    const bool exact = opt.mode == "exact";
    const double left = opt.time_limit_s > 0 ? opt.time_limit_s - prior_wall - since(t0) : 0;
    if (exact || left > 0) {
        apex::PolishConfig pc;
        pc.threads = opt.threads;
        pc.seed = rng();
        if (left > 0) pc.time_limit_s = exact ? 0.1 * left : left;
        if (exact) pc.stall_per_node = 50;  // warm-up ends when the ILS stops improving
        const auto ps = apex::polish_tour(tour, inst, g, pc);
        tel.event("polish", {{"heuristic", "true"}, {"ub", std::to_string(tour.cost)}, {"start", std::to_string(ps.start_cost)},
                             {"parallel", ps.parallel ? "true" : "false"}, {"segments", std::to_string(ps.segments)},
                             {"rounds", std::to_string(ps.rounds)}, {"iterations", std::to_string(ps.iterations)},
                             {"ms", fmt(ps.seconds * 1e3)}});
        best = tour;
        apex::check_memory(mem_limit, "polish");
    }

    if (exact) {  // branch-and-cut from the heuristic UB (Sprint 3 + M4.7 hand-off)
        const uint32_t added = apex::add_tour_edges(g, inst, tour);
        apex::BnCConfig bc;
        bc.parallel_lps = std::clamp<uint32_t>(opt.threads, 1, 6);
        // The heuristic phase already gives a near-optimal UB: no dive needed to find one, the tree
        // only has to raise the LB, so best-bound from the root.
        bc.select = apex::BnCConfig::Select::BestBound;
        bc.mem_limit_bytes = mem_limit;
        bc.seed = opt.seed;
        bc.tour_path = opt.out;
        if (opt.time_limit_s > 0) bc.time_limit_s = std::max(0.0, opt.time_limit_s - prior_wall - since(t0));
        if (!opt.checkpoint.empty()) {
            bc.checkpoint_dir = opt.checkpoint + "/bnc";
            bc.resume = opt.resume && std::filesystem::exists(bc.checkpoint_dir + "/LATEST");
        }
        std::unique_ptr<apex::BackgroundIls> bg;
        if (opt.threads > 1) {
            apex::IlsConfig ic;
            ic.seed = rng();
            bg = std::make_unique<apex::BackgroundIls>(inst, g, tour, ic);
            bc.primal = bg.get();
        }
        tel.event("bnc_start", {{"ub", std::to_string(tour.cost)}, {"tour_edges_added", std::to_string(added)},
                                {"edges", std::to_string(g.edges.size())}, {"parallel_lps", std::to_string(bc.parallel_lps)},
                                {"background_ils", bg ? "true" : "false"}});
        const auto r = apex::run_bnc(inst, g, bc, tour, &tel);
        if (bg) bg->stop();
        tour = r.best;
        lb = r.lb_int;
        status = r.status == apex::BnCResult::Status::MemLimit ? "MEM_LIMIT" : apex::status_name(r.status);
    }

    best = tour;
    } catch (const std::exception& e) {
        const bool alloc = dynamic_cast<const std::bad_alloc*>(&e) != nullptr;
        if (!alloc && !dynamic_cast<const apex::MemLimitExceeded*>(&e)) throw;
        status = "MEM_LIMIT";
        tour = best;
        tel.event("mem_limit", {{"status", "MEM_LIMIT"}, {"cause", alloc ? "allocation_failed" : "budget"},
                                {"msg", e.what()}, {"rss_bytes", std::to_string(apex::current_rss_bytes())},
                                {"limit_bytes", std::to_string(mem_limit)}, {"ub", std::to_string(tour.cost)}});
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
    rep.ram_peak_bytes = apex::peak_rss_bytes();
    rep.status = status;
    rep.threads = opt.threads;
    const char* lb_source = opt.mode == "exact" ? "lp" : "lagrangian";
    if (lb) {
        rep.lb = *lb;
        rep.lb_source = lb_source;
    }
    if (!opt.report.empty()) apex::write_report_file(opt.report, rep);
    tel.event("done", {{"status", status}, {"mode", opt.mode}, {"ub", std::to_string(tour.cost)}, {"lb", lb ? std::to_string(*lb) : "null"},
                       {"gap_pct", lb ? fmt(100.0 * (tour.cost - *lb) / tour.cost, 4) : "null"},
                       {"lb_source", lb ? lb_source : "null"}, {"wall_s", fmt(wall)},
                       {"ram_peak_bytes", std::to_string(rep.ram_peak_bytes)}});
    return status == "MEM_LIMIT" ? 4 : 0;
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
    try {
        return run(opt, tel);
    } catch (const std::exception& e) {
        tel.event("error", {{"kind", "runtime"}, {"msg", e.what()}});
        return 1;
    }
}
