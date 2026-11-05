#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "bnc/tree.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {
namespace fs = std::filesystem;

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}
apex::Tour heuristic_tour(const apex::Instance& inst, const apex::CandidateGraph& g) {
    auto t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    apex::two_opt_oropt(t, inst, g, apex::TwoOptConfig{});
    return t;
}
fs::path fresh_dir(const std::string& name) {
    const auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    return d;
}
apex::BnCResult run(const apex::Instance& inst, apex::BnCConfig cfg) {
    auto g = apex::build_candidate_graph(inst);
    return apex::run_bnc(inst, g, cfg, heuristic_tour(inst, g));
}
}  // namespace

TEST_CASE("T_checkpoint_resume_equivalence") {
    // Direct run vs. run killed mid-tree (node limit, checkpoint written) + resume: identical node
    // order, tour, bounds and counters. berlin52 (spec) is solved at the root, so the mid-tree kill
    // uses a280 and eil51, the instances here that branch.
    for (const char* name : {"a280", "eil51"}) {
        CAPTURE(name);
        const auto inst = load(name);
        const auto direct = run(inst, apex::BnCConfig{});
        REQUIRE(direct.status == apex::BnCResult::Status::Optimal);
        REQUIRE(direct.nodes >= 2);
        const auto dir = fresh_dir(std::string("apex_ckpt_") + name);
        apex::BnCConfig a;
        a.checkpoint_dir = dir.string();
        a.max_nodes = direct.nodes / 2 + 1;
        const auto first = run(inst, a);
        CHECK(first.status == apex::BnCResult::Status::NodeLimit);
        CHECK(first.checkpoints_written == 1);
        CHECK(fs::exists(dir / "LATEST"));
        apex::BnCConfig b;
        b.checkpoint_dir = dir.string();
        b.resume = true;
        const auto resumed = run(inst, b);  // a fresh graph: everything comes from the checkpoint
        CHECK(resumed.status == apex::BnCResult::Status::Optimal);
        CHECK(resumed.processing_order == direct.processing_order);
        CHECK(resumed.ub == direct.ub);
        CHECK(resumed.best.order == direct.best.order);
        CHECK(resumed.lb_int == direct.lb_int);
        CHECK(resumed.nodes == direct.nodes);
        CHECK(resumed.branched == direct.branched);
        CHECK(resumed.pruned == direct.pruned);
        CHECK(resumed.strong_pivots == direct.strong_pivots);
        MESSAGE(std::string(name) << " nodes=" << direct.nodes << " killed after " << first.nodes << ", resumed to " << resumed.nodes);
        fs::remove_all(dir);
    }
}

TEST_CASE("T_checkpoint_v2_integrity") {
    const auto inst = load("a280");
    const auto dir = fresh_dir("apex_ckpt_integrity");
    apex::BnCConfig a;
    a.checkpoint_dir = dir.string();
    a.max_nodes = 3;
    run(inst, a);
    std::string snap;
    std::ifstream(dir / "LATEST") >> snap;
    REQUIRE(fs::exists(dir / snap / "manifest.txt"));
    // Only the current snapshot survives; no temporary left behind.
    uint32_t snaps = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto f = e.path().filename().string();
        CHECK(f.find(".tmp") == std::string::npos);
        snaps += f.rfind("v2-", 0) == 0;
    }
    CHECK(snaps == 1);
    apex::BnCConfig b;
    b.checkpoint_dir = dir.string();
    b.resume = true;
    {  // flip one byte of a data file: the manifest hash must reject it
        const auto f = dir / snap / "nodes.txt";
        std::fstream io(f, std::ios::in | std::ios::out | std::ios::binary);
        io.seekp(8);
        io.put('9');
    }
    CHECK_THROWS_WITH_AS(run(inst, b), doctest::Contains("corrupt"), std::runtime_error);
    {  // other instance
        apex::BnCConfig c = b;
        const auto other = load("eil51");
        CHECK_THROWS_WITH_AS(run(other, c), doctest::Contains("instance mismatch"), std::runtime_error);
    }
    fs::remove_all(dir);
}

TEST_CASE("T_parallel_tree_deterministic") {
    // parallel_lps = 2: deterministic batches merged in worker order => identical runs, and the
    // same optimum as the serial tree.
    for (const char* name : {"eil51", "a280"}) {
        CAPTURE(name);
        const auto inst = load(name);
        apex::BnCConfig cfg;
        cfg.parallel_lps = 2;
        const auto r1 = run(inst, cfg);
        const auto r2 = run(inst, cfg);
        CHECK(r1.status == apex::BnCResult::Status::Optimal);
        CHECK(r1.processing_order == r2.processing_order);
        CHECK(r1.best.order == r2.best.order);
        CHECK(r1.ub == run(inst, apex::BnCConfig{}).ub);
        CHECK(r1.lb_int == r1.ub);
        CHECK(apex::tour_cost(inst, r1.best) == r1.ub);
        MESSAGE(std::string(name) << " parallel nodes=" << r1.nodes << " batches=" << r1.batches);
    }
    apex::BnCConfig bad;
    bad.parallel_lps = 3;
    CHECK_THROWS_AS(run(load("eil51"), bad), std::invalid_argument);
}

TEST_CASE("T_parallel_checkpoint_resume") {
    const auto inst = load("a280");
    apex::BnCConfig p;
    p.parallel_lps = 2;
    const auto direct = run(inst, p);
    const auto dir = fresh_dir("apex_ckpt_parallel");
    apex::BnCConfig a = p;
    a.checkpoint_dir = dir.string();
    a.max_nodes = direct.nodes / 2 + 1;
    run(inst, a);
    apex::BnCConfig b = p;
    b.checkpoint_dir = dir.string();
    b.resume = true;
    const auto resumed = run(inst, b);
    CHECK(resumed.processing_order == direct.processing_order);
    CHECK(resumed.ub == direct.ub);
    fs::remove_all(dir);
}
