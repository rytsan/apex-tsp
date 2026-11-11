// APEX-TSP engine entry point (Sprint 0: scalable mode = primal engine only).
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
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
#include "core/control.hpp"
#include "core/instance.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "graph/candidate_graph.hpp"
#include "bnc/tree.hpp"
#include "decomp/pipeline.hpp"
#include "hk/held_karp.hpp"
#include "ipc/server.hpp"
#include "primal/lab.hpp"
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

uint64_t dir_bytes(const std::string& dir) {
    std::error_code ec;
    uint64_t b = 0;
    if (dir.empty() || !std::filesystem::exists(dir, ec)) return 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) b += it->file_size(ec);
    return b;
}

std::string default_shared_tour(const std::string& endpoint) {
    std::string name = std::filesystem::path(endpoint).filename().string();
    if (const auto k = name.find_last_of("\\"); k != std::string::npos) name = name.substr(k + 1);  // pipe name
    return (std::filesystem::temp_directory_path() / (name + ".tour")).string();
}

int run(const apex::Options& opt, apex::TelemetrySink& log) {
    const auto t0 = Clock::now();
    const uint64_t mem_limit = opt.mem_limit_mb << 20;
    const auto enf = apex::enforce_memory_limit(mem_limit);  // OS backstop; the guards below stop first
    const auto inst = apex::read_tsplib(opt.instance);

    // GUI channel (M5.2). Observation hooks only: the latest tour is copied at most twice a second,
    // for the SharedTour and for checkpoints requested over IPC.
    std::mutex ck_mu;
    apex::Tour latest;
    std::string rng_snapshot;
    double prior_wall = 0;
    auto ck_last = Clock::time_point{};
    std::unique_ptr<apex::ipc::IpcServer> ipc;
    // Lab mode (M6.4): commands from the GUI are queued here and run on this thread.
    std::mutex lab_mu;
    std::condition_variable lab_cv;
    std::deque<std::string> lab_queue;
    auto write_v1 = [&](const apex::Tour& t, const std::string& rng_state, double wall) {
        apex::CheckpointV1 ck;
        ck.instance_hash = apex::instance_hash(inst);
        ck.instance_name = inst.name;
        ck.mode = opt.mode;
        ck.seed = opt.seed;
        ck.knn = opt.knn;
        ck.time_limit_s = opt.time_limit_s;
        ck.rng_state = rng_state;
        ck.wall_s = wall;
        ck.best = t;
        apex::write_checkpoint(opt.checkpoint, ck);
    };
    if (!opt.ipc.empty()) {
        apex::ipc::ServerConfig sc;
        sc.endpoint = opt.ipc;
        sc.shared_tour_path = opt.shared_tour.empty() ? default_shared_tour(opt.ipc) : opt.shared_tour;
        sc.tour_capacity = inst.n;
        sc.hello.mode = opt.mode == "exact" ? 0 : opt.mode == "scalable" ? 1 : 2;  // 2 = lab
        sc.hello.mem_limit_mb = static_cast<uint32_t>(opt.mem_limit_mb);
        sc.hello.threads = opt.threads;
        sc.forward = &log;
        sc.on_checkpoint = [&]() -> bool {
            if (opt.checkpoint.empty()) return false;
            std::lock_guard<std::mutex> lk(ck_mu);
            if (latest.order.empty()) return false;
            write_v1(latest, rng_snapshot, prior_wall + since(t0));
            return true;
        };
        if (opt.mode == "lab")
            sc.on_command = [&](std::string_view cmd) -> std::string {
                std::istringstream in{std::string(cmd)};
                std::string verb, step;
                in >> verb >> step;
                const auto& names = apex::lab_step_names();
                if (verb != "heuristic" || (step != "quit" && std::find(names.begin(), names.end(), step) == names.end()))
                    return "lab_error unknown command '" + std::string(cmd) + "'";
                {
                    std::lock_guard<std::mutex> lk(lab_mu);
                    lab_queue.push_back(std::string(cmd).substr(std::string(cmd).find(step)));
                }
                lab_cv.notify_one();
                return "lab_queued " + step;
            };
        ipc = std::make_unique<apex::ipc::IpcServer>(sc);
        if (!ipc->listening()) throw std::runtime_error("--ipc: cannot listen on " + opt.ipc);
    }
    apex::TelemetrySink& tel = ipc ? static_cast<apex::TelemetrySink&>(*ipc) : log;
    auto on_tour = [&](const apex::Tour& t) {
        if (!ipc) return;
        std::lock_guard<std::mutex> lk(ck_mu);
        const auto now = Clock::now();
        if (now - ck_last < std::chrono::milliseconds(500)) return;
        ck_last = now;
        latest = t;
        ipc->publish_tour(t, true);
    };
    auto phase = [&](const char* name, const apex::Tour& t, std::mt19937_64& r) {
        tel.event("phase", {{"name", name}});
        if (!ipc) return;
        std::ostringstream rs;
        rs << r;
        std::lock_guard<std::mutex> lk(ck_mu);
        rng_snapshot = rs.str();
        if (!t.order.empty()) {
            latest = t;
            ipc->publish_tour(t, true);
        }
    };
    uint64_t tours_found = 0;
    int64_t last_ub = INT64_MAX;
    auto count_ub = [&](int64_t ub) {
        if (ub < last_ub) ++tours_found;
        last_ub = std::min(last_ub, ub);
    };
    tel.event("instance_loaded", {{"name", inst.name}, {"n", std::to_string(inst.n)},
                                  {"metric", apex::metric_name(inst.metric)}, {"ms", fmt(since(t0) * 1e3)}});
    tel.event("config", {{"mode", opt.mode}, {"seed", std::to_string(opt.seed)},
                         {"threads", std::to_string(opt.threads)}, {"threads_used", std::to_string(opt.threads)},
                         {"mem_limit_mb", std::to_string(opt.mem_limit_mb)}, {"mem_limit_enforced", enf.enforced ? "true" : "false"},
                         {"mem_limit_method", enf.method},
                         {"time_limit_s", fmt(opt.time_limit_s)}, {"knn", std::to_string(opt.knn)}});

    std::mt19937_64 rng(opt.seed);
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
    count_ub(tour.cost);

    // Graceful MEM_LIMIT (M4.5): guards at phase boundaries (RSS >= 90% of --mem-limit-mb) and
    // allocation failures under the OS cap both end in: best tour so far -> checkpoint + report.
    apex::Tour best = tour;
    std::string status = "done";
    std::optional<int64_t> lb;
    apex::BnCResult bnc_result;
    bool ran_bnc = false;
    try {
    apex::check_memory(mem_limit, "start");
    phase("candidate_graph", tour, rng);
    const auto tg = Clock::now();
    auto g = apex::build_candidate_graph(inst, {opt.knn, true});
    apex::check_memory(mem_limit, "candidate_graph");
    tel.event("candidate_graph", {{"edges", std::to_string(g.edges.size())},
                                  {"fallback_edges", std::to_string(g.fallback_edges)},
                                  {"ms", fmt(since(tg) * 1e3)}});

    if (opt.mode == "lab") {
        // Heuristics lab: start from the space-filling-curve tour, run the --lab steps, then (with
        // a GUI) every queued command until QUIT / SHUTDOWN. Every tour is published.
        phase("lab", tour, rng);
        std::string names;
        for (const auto& n : apex::lab_step_names()) names += (names.empty() ? "" : ",") + n;
        tel.event("lab_ready", {{"steps", names}, {"ub", std::to_string(tour.cost)}});
        auto publish = [&](const apex::Tour& t) {
            if (!ipc) return;
            std::lock_guard<std::mutex> lk(ck_mu);
            latest = t;
            ipc->publish_tour(t, true);
        };
        auto run_step = [&](const std::string& cmd) {
            apex::LabStep st;
            try {
                st = apex::run_lab_step(cmd, tour, inst, g, rng(), opt.threads, [&](const apex::Tour& t) { on_tour(t); });
            } catch (const std::logic_error& e) {  // bad numeric argument
                st.name = cmd;
                st.error = e.what();
            }
            if (!st.ok) {
                tel.event("lab_error", {{"step", st.name}, {"msg", st.error}});
                return;
            }
            publish(tour);
            count_ub(tour.cost);
            if (tour.cost < best.cost || best.order.empty()) best = tour;
            tel.event("lab_step", {{"name", st.name}, {"heuristic", "true"}, {"before", std::to_string(st.before)},
                                   {"after", std::to_string(st.after)}, {"delta", std::to_string(st.after - st.before)},
                                   {"moves", std::to_string(st.moves)}, {"ms", fmt(st.ms)}, {"best", std::to_string(best.cost)}});
            if (apex::cancel_requested() && !(ipc && ipc->shutdown_requested())) {
                apex::engine_control().reset();  // cancel stops the running step only
                tel.event("lab_step_cancelled", {{"name", st.name}});
            }
        };
        publish(tour);
        std::string steps = opt.lab;
        std::replace(steps.begin(), steps.end(), ',', ' ');
        std::replace(steps.begin(), steps.end(), ':', ' ');
        {
            std::istringstream in(steps);
            std::vector<std::string> toks{std::istream_iterator<std::string>(in), {}};
            for (size_t i = 0; i < toks.size(); ++i) {
                std::string cmd = toks[i];
                if (cmd == "ils" && i + 1 < toks.size() && std::isdigit(static_cast<unsigned char>(toks[i + 1][0]))) cmd += " " + toks[++i];
                run_step(cmd);
            }
        }
        while (ipc && !ipc->shutdown_requested()) {
            std::string cmd;
            {
                std::unique_lock<std::mutex> lk(lab_mu);
                lab_cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return !lab_queue.empty(); });
                if (lab_queue.empty()) continue;
                cmd = lab_queue.front();
                lab_queue.pop_front();
            }
            if (cmd.rfind("quit", 0) == 0) break;
            run_step(cmd);
        }
        tour = best;
    } else {
    // heuristic (M4.7): density clusters solved in parallel, merged level by level with
    // re-evaluation (or greedy + 2-opt for small n). UB only; no bound from this phase exists.
    if (!opt.resume) {
        phase("construct", tour, rng);
        apex::DecompConfig dc;
        dc.max_leaf = opt.leaf_size;
        dc.leaf.exact_limit = opt.leaf_exact_limit;
        dc.leaf.threads = opt.threads;
        dc.leaf.seed = rng();
        if (ipc && opt.mode == "scalable") {
            dc.leaf.on_cluster = [&](uint32_t id, uint8_t s) { ipc->cluster_state(id, s); };
            dc.leaf.on_cluster_box = [&](uint32_t id, double x0, double y0, double x1, double y1) {
                tel.event("cluster_box", {{"id", std::to_string(id)}, {"x0", fmt(x0, 1)}, {"y0", fmt(y0, 1)},
                                          {"x1", fmt(x1, 1)}, {"y1", fmt(y1, 1)}});
            };
        }
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
        count_ub(tour.cost);
        apex::check_memory(mem_limit, "construct");
    }

    phase("two_opt", tour, rng);
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
    count_ub(tour.cost);
    apex::check_memory(mem_limit, "two_opt");

    // Held-Karp (M4.4): Lagrangian LB of the scalable mode (lb_source lagrangian); ascent gets
    // 10% of the remaining budget, the exact complete-graph 1-tree comes on top.
    if (opt.mode == "scalable" && opt.time_limit_s > 0) {
        phase("held_karp", tour, rng);
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
        phase("polish", tour, rng);
        apex::PolishConfig pc;
        pc.threads = opt.threads;
        pc.seed = rng();
        if (left > 0) pc.time_limit_s = exact ? 0.1 * left : left;
        if (exact) pc.stall_per_node = 50;  // warm-up ends when the ILS stops improving
        if (ipc) pc.on_tour = on_tour;
        const auto ps = apex::polish_tour(tour, inst, g, pc);
        tel.event("polish", {{"heuristic", "true"}, {"ub", std::to_string(tour.cost)}, {"start", std::to_string(ps.start_cost)},
                             {"parallel", ps.parallel ? "true" : "false"}, {"segments", std::to_string(ps.segments)},
                             {"rounds", std::to_string(ps.rounds)}, {"iterations", std::to_string(ps.iterations)},
                             {"ms", fmt(ps.seconds * 1e3)}});
        best = tour;
        count_ub(tour.cost);
        apex::check_memory(mem_limit, "polish");
    }

    if (exact) {  // branch-and-cut from the heuristic UB (Sprint 3 + M4.7 hand-off)
        phase("bnc", tour, rng);
        const uint32_t added = apex::add_tour_edges(g, inst, tour);
        apex::BnCConfig bc;
        bc.parallel_lps = std::clamp<uint32_t>(opt.threads, 1, 6);
        bc.root.dp.threads = opt.threads;  // root separation; parallel tree batches drop to 1 per node
        // The heuristic phase already gives a near-optimal UB: no dive needed to find one, the tree
        // only has to raise the LB, so best-bound from the root.
        bc.select = apex::BnCConfig::Select::BestBound;
        bc.mem_limit_bytes = mem_limit;
        bc.seed = opt.seed;
        bc.tour_path = opt.out;
        if (ipc) bc.on_tour = [&](const apex::Tour& t) {
            std::lock_guard<std::mutex> lk(ck_mu);
            latest = t;
            ipc->publish_tour(t, true);
        };
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
        tours_found += r.ub_improvements;
        last_ub = std::min(last_ub, tour.cost);
        bnc_result = r;
        ran_bnc = true;
        status = r.status == apex::BnCResult::Status::MemLimit ? "MEM_LIMIT" : apex::status_name(r.status);
    }
    }  // not lab

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

    if (status == "done" && apex::cancel_requested()) status = "CANCELLED";
    if (apex::tour_cost(inst, tour) != tour.cost) throw std::runtime_error("internal error: tour cost drift");

    const double wall = prior_wall + since(t0);
    if (!opt.out.empty()) apex::write_tour_file(opt.out, inst, tour, "APEX-TSP length " + std::to_string(tour.cost));
    if (!opt.checkpoint.empty()) {
        std::ostringstream rs;
        rs << rng;
        {
            std::lock_guard<std::mutex> lk(ck_mu);
            write_v1(tour, rs.str(), wall);
        }
        tel.event("checkpoint_written", {{"dir", opt.checkpoint}});
    }
    if (ipc) ipc->publish_tour(tour, true);
    apex::Report rep;
    rep.instance = inst.name;
    rep.mode = opt.mode;
    rep.ub = tour.cost;
    rep.wall_s = wall;
    rep.seed = opt.seed;
    rep.ram_peak_bytes = apex::peak_rss_bytes();
    rep.status = status;
    rep.threads = opt.threads;
    rep.disk_bytes = dir_bytes(opt.checkpoint);
    rep.tours_found = tours_found;
    rep.hardware = apex::hardware_string();
    rep.mem_limit_mb = opt.mem_limit_mb;
    if (ran_bnc) {
        rep.iterations = bnc_result.lp_iterations;
        rep.refactorizations = bnc_result.refactorizations;
        rep.cuts_sec = bnc_result.cuts_sec;
        rep.cuts_blossom = bnc_result.cuts_blossom;
        rep.cuts_comb = bnc_result.cuts_comb;
        rep.cuts_dp = bnc_result.cuts_dp;
        rep.cuts_component = bnc_result.cuts_component;
        rep.verification_rounds = bnc_result.verification_pricing_calls;
    }
    const char* lb_source = opt.mode == "exact" ? "lp" : "lagrangian";
    if (lb) {
        rep.lb = *lb;
        rep.lb_source = lb_source;
    }
    const std::string report = apex::report_json(rep);
    if (!opt.report.empty()) apex::write_report_file(opt.report, rep);
    tel.event("done", {{"status", status}, {"mode", opt.mode}, {"ub", std::to_string(tour.cost)}, {"lb", lb ? std::to_string(*lb) : "null"},
                       {"gap_pct", lb ? fmt(100.0 * (tour.cost - *lb) / tour.cost, 4) : "null"},
                       {"lb_source", lb ? lb_source : "null"}, {"wall_s", fmt(wall)},
                       {"ram_peak_bytes", std::to_string(rep.ram_peak_bytes)}});
    if (ipc) ipc->finish(report);  // FINISHED = the --report bytes
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
