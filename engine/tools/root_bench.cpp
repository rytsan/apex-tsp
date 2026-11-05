// Root cut-and-resolve transcript (Sprint 2 gates G2.1-G2.3, G2.5).
// usage: root_bench --instance <file.tsp> --ref <best known> [--sep-blossom heuristic|full]
//                   [--no-blossom] [--time-limit s] [--pricing devex|dse] [--seed n]
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "app/telemetry.hpp"
#include "bnc/root_loop.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

int main(int argc, char** argv) {
    try {
        std::string path;
        double ref = 0, time_limit = INFINITY;
        apex::RootLoopConfig cfg;
        uint64_t seed = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string f = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + f);
                return argv[++i];
            };
            if (f == "--instance") path = val();
            else if (f == "--ref") ref = std::stod(val());
            else if (f == "--time-limit") time_limit = std::stod(val());
            else if (f == "--seed") seed = std::stoull(val());
            else if (f == "--no-blossom") cfg.use_blossom = false;
            else if (f == "--sep-blossom") {
                const auto v = val();
                if (v == "full") cfg.blossom.mode = apex::BlossomMode::Full;
                else if (v == "heuristic") cfg.blossom.mode = apex::BlossomMode::Heuristic;
                else throw std::runtime_error("--sep-blossom must be heuristic or full");
            } else if (f == "--pricing") {
                const auto v = val();
                cfg.lp.pricing = v == "dse" ? apex::SimplexOptions::Pricing::DualSteepestEdge : apex::SimplexOptions::Pricing::Devex;
            } else throw std::runtime_error("unknown flag '" + f + "'");
        }
        if (path.empty() || ref <= 0) throw std::runtime_error("need --instance and --ref");
        apex::LogSink tel(stdout);
        const auto inst = apex::read_tsplib(path);
        auto g = apex::build_candidate_graph(inst);
        auto tour = apex::hilbert_tour(inst, apex::hilbert_order(inst));
        apex::TwoOptConfig tc;
        tc.seed = seed;
        apex::two_opt_oropt(tour, inst, g, tc);
        cfg.ub = tour.cost;
        cfg.time_limit_s = time_limit;
        cfg.sec.seed = seed;
        tel.event("root_start", {{"instance", inst.name}, {"n", std::to_string(inst.n)}, {"edges", std::to_string(g.edges.size())},
                                 {"ub_2opt", std::to_string(tour.cost)}, {"ref", std::to_string(static_cast<long long>(ref))},
                                 {"blossom", cfg.use_blossom ? (cfg.blossom.mode == apex::BlossomMode::Full ? "full" : "heuristic") : "off"}});
        const auto r = apex::run_root_loop(inst, g, cfg, &tel);
        std::printf("\nSUMMARY %s n=%u status=%s converged=%d tailing_off=%d rounds=%zu\n", inst.name.c_str(), inst.n,
                    apex::status_name(r.status), r.converged ? 1 : 0, r.tailing_off ? 1 : 0, r.rounds.size());
        std::printf("  lp_value(candidate graph) = %.4f   ratio_to_ref = %.4f%%   gap_vs_ref = %.4f%%\n", r.lp_value,
                    100.0 * r.lp_value / ref, 100.0 * (ref - r.lp_value) / ref);
        std::printf("  global_lb (Farley, complete graph) = %.4f  lb_int = %lld  ratio_to_ref = %.4f%%  negative_rc_edges = %llu\n",
                    r.global_lb, static_cast<long long>(r.global_lb_int), 100.0 * r.global_lb / ref,
                    static_cast<unsigned long long>(r.negative_rc_edges));
        std::printf("  ub_2opt = %lld   gap_vs_ub = %.4f%%   honesty: global_lb <= ref: %s, lp_value <= ref: %s\n",
                    static_cast<long long>(cfg.ub), 100.0 * (cfg.ub - r.global_lb) / cfg.ub,
                    r.global_lb <= ref + 1e-6 ? "OK" : "VIOLATED", r.lp_value <= ref + 1e-6 ? "OK" : "VIOLATED");
        std::printf("  cuts: component=%llu sec=%llu blossom=%llu  pool size=%zu in_lp=%zu aged_out=%llu dup=%llu\n",
                    static_cast<unsigned long long>(r.cuts_component), static_cast<unsigned long long>(r.cuts_sec),
                    static_cast<unsigned long long>(r.cuts_blossom), r.pool.size, r.pool.in_lp,
                    static_cast<unsigned long long>(r.pool.aged_out), static_cast<unsigned long long>(r.pool.duplicates));
        std::printf("  pricing: rounds=%u priced_edges=%llu final_edges=%zu\n", r.pricing_rounds,
                    static_cast<unsigned long long>(r.priced_edges), g.edges.size());
        std::printf("  rcf: fixed0=%u fixed1=%u   time: lp=%.2fs sep=%.2fs pricing=%.2fs total=%.2fs  rss=%.1fMB\n", r.rcf_fixed0,
                    r.rcf_fixed1, r.lp_s, r.sep_s, r.pricing_s, r.total_s, apex::peak_rss_bytes() / 1048576.0);
        return r.global_lb <= ref + 1e-6 && r.lp_value <= ref + 1e-6 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
