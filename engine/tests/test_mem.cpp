#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/mem.hpp"
#include "bnc/tree.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace fs = std::filesystem;

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int run(const std::string& args, const fs::path& log) {
    const std::string cmd = std::string(APEX_BIN) + " " + args + " > \"" + log.string() + "\" 2>&1";
    const int rc = std::system(cmd.c_str());
#if defined(_WIN32)
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

}  // namespace

TEST_CASE("T_mem_limit_graceful") {
    // apex_tsp on pla85900 under a ceiling below its footprint (the scalable mode peaks near
    // 50 MB since M4.5, so the spec's 256 MB no longer triggers): MEM_LIMIT report + checkpoint,
    // then the checkpoint resumes under a normal limit.
    const fs::path dir = fs::temp_directory_path() / "apex_mem_limit_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string inst = std::string(APEX_DATA_DIR) + "/pla85900.tsp";
    const std::string ck = (dir / "ck").string(), rep = (dir / "r.json").string();
    const int rc = run("--instance " + inst + " --time-limit 30 --mem-limit-mb 20 --checkpoint " + ck + " --report " + rep,
                       dir / "a.log");
    const auto log = slurp(dir / "a.log");
    CAPTURE(log);
    CHECK(rc == 4);
    CHECK(log.find("event=mem_limit status=MEM_LIMIT") != std::string::npos);
    CHECK(log.find(apex::kSanitizerBuild ? "mem_limit_enforced=false" : "mem_limit_enforced=true") != std::string::npos);
    CHECK(slurp(rep).find("\"status\":\"MEM_LIMIT\"") != std::string::npos);
    REQUIRE(fs::exists(fs::path(ck) / "checkpoint.v1"));
    const int rc2 = run("--instance " + inst + " --time-limit 5 --checkpoint " + ck + " --resume --report " + rep, dir / "b.log");
    const auto log2 = slurp(dir / "b.log");
    CAPTURE(log2);
    CHECK(rc2 == 0);
    CHECK(log2.find("event=resumed") != std::string::npos);
    CHECK(slurp(rep).find("\"status\":\"done\"") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("T_lp_concurrency_budget") {
    const auto inst = load("a280");
    auto tour = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    SUBCASE("budget never exceeded, deterministic") {
        uint32_t seen[2] = {0, 0};
        int64_t ub[2] = {0, 0};
        for (int rep = 0; rep < 2; ++rep) {
            auto g = apex::build_candidate_graph(inst);
            apex::BnCConfig cfg;
            cfg.parallel_lps = 4;
            const auto r = apex::run_bnc(inst, g, cfg, tour);
            CHECK(r.status == apex::BnCResult::Status::Optimal);
            CHECK(r.lp_budget_violations == 0);
            CHECK(r.max_concurrent_lps <= 4);
            CHECK(r.max_concurrent_lps >= 2);
            CHECK(r.lp_working_set_bytes > 0);
            seen[rep] = r.max_concurrent_lps;
            ub[rep] = r.ub;
            MESSAGE("a280 lps=4 max_concurrent=" << r.max_concurrent_lps << " min_tokens=" << r.min_lp_tokens
                                                 << " ws_bytes=" << r.lp_working_set_bytes << " baseline=" << r.budget_baseline_bytes);
        }
        CHECK(seen[0] == seen[1]);
        CHECK(ub[0] == ub[1]);
    }
    SUBCASE("tight budget: fewer LPs, still proven") {
        auto g = apex::build_candidate_graph(inst);
        apex::BnCConfig cfg;
        cfg.parallel_lps = 6;
        cfg.mem_limit_bytes = (uint64_t(65) << 20);  // process allowance (64 MB) + ~1 MB: about one LP
        const auto r = apex::run_bnc(inst, g, cfg, tour);
        CHECK(r.lp_budget_violations == 0);
        // The RSS safety net may stop it first where the process itself is bigger (sanitizers).
        CHECK((r.status == apex::BnCResult::Status::Optimal || apex::current_rss_bytes() > cfg.mem_limit_bytes));
        CHECK(r.min_lp_tokens < 6);
        CHECK(r.max_concurrent_lps < 6);
        MESSAGE("a280 tight max_concurrent=" << r.max_concurrent_lps << " min_tokens=" << r.min_lp_tokens
                                             << " status=" << std::string(apex::status_name(r.status)));
    }
    SUBCASE("exhausted budget: MEM_LIMIT + resumable checkpoint") {
        const fs::path dir = fs::temp_directory_path() / "apex_lp_budget_test";
        fs::remove_all(dir);
        auto g = apex::build_candidate_graph(inst);
        apex::BnCConfig cfg;
        cfg.mem_limit_bytes = uint64_t(32) << 20;  // below the process allowance: 0 tokens after the root
        cfg.checkpoint_dir = dir.string();
        const auto r = apex::run_bnc(inst, g, cfg, tour);
        REQUIRE(r.status == apex::BnCResult::Status::MemLimit);
        CHECK(r.nodes == 1);
        CHECK(r.max_concurrent_lps == 1);  // the root
        auto g2 = apex::build_candidate_graph(inst);
        apex::BnCConfig rc;
        rc.checkpoint_dir = dir.string();
        rc.resume = true;
        const auto r2 = apex::run_bnc(inst, g2, rc, tour);
        CHECK(r2.status == apex::BnCResult::Status::Optimal);
        CHECK(r2.ub == 2579);
        fs::remove_all(dir);
    }
}
