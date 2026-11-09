// Root cut-and-resolve transcript (Sprint 2 gates G2.1-G2.3, G2.5).
// usage: root_bench --instance <file.tsp> --ref <best known> [--sep-blossom heuristic|full]
//                   [--no-blossom] [--time-limit s] [--pricing devex|dse] [--seed n]
//                   [--tree [--best-bound] [--parallel-lps k] [--checkpoint dir [--resume]] [--ils]] [--hk]
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "app/telemetry.hpp"
#include "bnc/root_loop.hpp"
#include "bnc/tree.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "hk/held_karp.hpp"
#include "primal/lk_lite.hpp"
#include "primal/two_opt.hpp"

int main(int argc, char** argv) {
    try {
        std::string path;
        double ref = 0, time_limit = INFINITY;
        bool tree = false, best_bound = false, resume = false, ils = false, hk = false, dive = false, no_rih = false;
        uint32_t parallel_lps = 1;
        uint64_t mem_limit_mb = 4096;
        std::string checkpoint;
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
            else if (f == "--no-comb") cfg.use_comb = false;
            else if (f == "--tree") tree = true;
            else if (f == "--best-bound") best_bound = true;
            else if (f == "--parallel-lps") parallel_lps = static_cast<uint32_t>(std::stoul(val()));
            else if (f == "--checkpoint") checkpoint = val();
            else if (f == "--resume") resume = true;
            else if (f == "--ils") ils = true;
            else if (f == "--hk") hk = true;
            else if (f == "--dive") dive = true;
            else if (f == "--mem-limit-mb") mem_limit_mb = std::stoull(val());
            else if (f == "--no-rih") no_rih = true;
            else if (f == "--sep-blossom") {
                const auto v = val();
                if (v == "full") cfg.blossom.mode = apex::BlossomMode::Full;
                else if (v == "heuristic") cfg.blossom.mode = apex::BlossomMode::Heuristic;
                else throw std::runtime_error("--sep-blossom must be heuristic or full");
            } else if (f == "--basis-update") {
                const auto v = val();
                cfg.lp.basis_update = v == "pfi" ? apex::BasisUpdateKind::Pfi : apex::BasisUpdateKind::ForrestTomlin;
            } else if (f == "--hypersparse") {
                cfg.lp.hypersparse = true;
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
        if (tree) {  // full branch-and-cut (Sprint 3)
            apex::BnCConfig bc;
            bc.root = cfg;
            bc.time_limit_s = time_limit;
            if (best_bound) bc.select = apex::BnCConfig::Select::BestBound;
            bc.parallel_lps = parallel_lps;
            bc.checkpoint_dir = checkpoint;
            bc.resume = resume;
            bc.seed = seed;
            if (dive) bc.dive.max_rounds = 5;
            bc.mem_limit_bytes = mem_limit_mb << 20;
            if (no_rih) bc.rih_iterations = 0;
            std::unique_ptr<apex::BackgroundIls> bg;
            if (ils) {  // background ILS for the whole run (M4.1, UB only)
                apex::IlsConfig ic;
                ic.seed = seed;
                bg = std::make_unique<apex::BackgroundIls>(inst, g, tour, ic);
                bc.primal = bg.get();
            }
            const auto r = apex::run_bnc(inst, g, bc, tour, &tel);
            if (bg) {
                bg->stop();
                const auto is = bg->stats();
                std::printf("\nILS iterations=%llu improvements=%llu strong_kicks=%llu best=%lld seconds=%.1f\n",
                            static_cast<unsigned long long>(is.iterations), static_cast<unsigned long long>(is.improvements),
                            static_cast<unsigned long long>(is.strong_kicks), static_cast<long long>(is.best_cost), is.seconds);
            }
            std::printf("\nSUMMARY %s n=%u status=%s ub=%lld lb=%.4f lb_int=%lld gap=%.4f%% ref=%.0f\n", inst.name.c_str(), inst.n,
                        apex::status_name(r.status), static_cast<long long>(r.ub), r.lb, static_cast<long long>(r.lb_int),
                        100.0 * (r.ub - r.lb) / r.ub, ref);
            std::printf("  certificate: ub == recomputed tour cost: %s   lb <= ref: %s   ub == ref: %s\n",
                        apex::tour_cost(inst, r.best) == r.ub ? "OK" : "VIOLATED", r.lb <= ref + 1e-6 ? "OK" : "VIOLATED",
                        r.ub == static_cast<int64_t>(ref) ? "yes" : "no");
            std::printf("  root_lb=%.4f nodes=%llu branched=%llu pruned=%llu infeasible=%llu integral=%llu depth=%llu ub_improvements=%llu\n",
                        r.root_lb, static_cast<unsigned long long>(r.nodes), static_cast<unsigned long long>(r.branched),
                        static_cast<unsigned long long>(r.pruned), static_cast<unsigned long long>(r.infeasible),
                        static_cast<unsigned long long>(r.integral), static_cast<unsigned long long>(r.max_depth),
                        static_cast<unsigned long long>(r.ub_improvements));
            std::printf("  warm=%llu cold=%llu warm_dual_infeasible=%llu  edges=%zu  rcf_root fixed0=%u fixed1=%u\n",
                        static_cast<unsigned long long>(r.warm_starts), static_cast<unsigned long long>(r.cold_starts),
                        static_cast<unsigned long long>(r.warm_dual_infeasible), g.edges.size(), r.root.rcf_fixed0, r.root.rcf_fixed1);
            std::printf("  heuristic calls=%llu time=%.2fs   strong branching: candidates=%llu pivots=%llu max_per_node=%llu\n",
                        static_cast<unsigned long long>(r.heuristic_calls), r.heuristic_s, static_cast<unsigned long long>(r.strong_candidates), static_cast<unsigned long long>(r.strong_pivots),
                        static_cast<unsigned long long>(r.max_node_strong_pivots));
            std::printf("  parallel_lps=%u batches=%llu checkpoints=%llu\n", parallel_lps,
                        static_cast<unsigned long long>(r.batches), static_cast<unsigned long long>(r.checkpoints_written));
            std::printf("  time: lp=%.2fs (%.1f%%) sep=%.2fs pricing=%.2fs total=%.2fs nodes/s=%.2f rss=%.1fMB\n", r.lp_s,
                        100.0 * r.lp_s / std::max(1e-9, r.total_s), r.sep_s, r.pricing_s, r.total_s,
                        r.nodes / std::max(1e-9, r.total_s), apex::peak_rss_bytes() / 1048576.0);
            return r.lb <= ref + 1e-6 && apex::tour_cost(inst, r.best) == r.ub ? 0 : 1;
        }
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
        std::printf("  cuts: component=%llu sec=%llu blossom=%llu comb=%llu  pool size=%zu in_lp=%zu aged_out=%llu dup=%llu\n",
                    static_cast<unsigned long long>(r.cuts_component), static_cast<unsigned long long>(r.cuts_sec),
                    static_cast<unsigned long long>(r.cuts_blossom), static_cast<unsigned long long>(r.cuts_comb), r.pool.size, r.pool.in_lp,
                    static_cast<unsigned long long>(r.pool.aged_out), static_cast<unsigned long long>(r.pool.duplicates));
        std::printf("  pricing: rounds=%u priced_edges=%llu final_edges=%zu\n", r.pricing_rounds,
                    static_cast<unsigned long long>(r.priced_edges), g.edges.size());
        uint64_t lp_iters = 0;
        for (const auto& rd : r.rounds) lp_iters += rd.lp_iterations;
        std::printf("  lp: basis_update=%s hypersparse=%d iterations=%llu refactorizations=%llu peak_lu_nnz=%llu\n",
                    cfg.lp.basis_update == apex::BasisUpdateKind::Pfi ? "pfi" : "ft", cfg.lp.hypersparse ? 1 : 0,
                    static_cast<unsigned long long>(lp_iters), static_cast<unsigned long long>(r.refactorizations), static_cast<unsigned long long>(r.peak_factor_nnz));
        std::printf("  rcf: fixed0=%u fixed1=%u   time: lp=%.2fs sep=%.2fs pricing=%.2fs total=%.2fs  rss=%.1fMB\n", r.rcf_fixed0,
                    r.rcf_fixed1, r.lp_s, r.sep_s, r.pricing_s, r.total_s, apex::peak_rss_bytes() / 1048576.0);
        bool hk_ok = true;
        if (hk) {  // M4.4 cross-check (c): converged root LP >= subtour LP >= any HK bound
            const auto h = apex::held_karp(inst, g, cfg.ub, apex::HkConfig{});
            hk_ok = !(r.converged && h.lb > r.global_lb + 1e-6);
            std::printf("  held_karp: lb=%.4f (%.4f%% of ref) iterations=%u ascent=%.2fs exact=%.2fs  root_lb >= hk: %s\n", h.lb,
                        100.0 * h.lb / ref, h.iterations, h.ascent_s, h.exact_s,
                        !r.converged ? "n/a (root not converged)" : (hk_ok ? "OK" : "BUG"));
        }
        return r.global_lb <= ref + 1e-6 && r.lp_value <= ref + 1e-6 && hk_ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
