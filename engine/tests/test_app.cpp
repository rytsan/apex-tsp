#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "app/telemetry.hpp"
#include "bnc/checkpoint.hpp"
#include "core/instance.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

using apex::CliError;
using apex::parse_cli;
using V = std::vector<std::string>;

TEST_CASE("T_cli_rejects_unknown_flags") {
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--bogus"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "-seed", "3"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "positional"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance"}), CliError);                              // missing value
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--seed", "--out", "x"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--seed", "12x"}), CliError);    // bad number
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--threads", "-1"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--threads", "0"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--mode", "fast"}), CliError);
    CHECK_THROWS_AS(parse_cli({"--instance", "a.tsp", "--resume"}), CliError);         // no --checkpoint
    CHECK_THROWS_AS(parse_cli({"--mode", "scalable"}), CliError);                      // no --instance
}

TEST_CASE("T_cli_parses_all_flags") {
    const auto o = parse_cli({"--instance", "x.tsp", "--mode", "exact", "--time-limit", "12.5", "--mem-limit-mb",
                              "2048", "--threads", "8", "--seed", "99", "--knn", "10", "--out", "r.tour",
                              "--checkpoint", "ck", "--resume", "--report", "r.json"});
    CHECK(o.instance == "x.tsp");
    CHECK(o.mode == "exact");
    CHECK(o.time_limit_s == 12.5);
    CHECK(o.mem_limit_mb == 2048);
    CHECK(o.threads == 8);
    CHECK(o.seed == 99);
    CHECK(o.knn == 10);
    CHECK(o.out == "r.tour");
    CHECK(o.checkpoint == "ck");
    CHECK(o.resume);
    CHECK(o.report == "r.json");
    const auto d = parse_cli({"--instance", "x.tsp"});
    CHECK(d.mode == "scalable");
    CHECK(d.seed == 1);
}

TEST_CASE("T_telemetry_log_format") {
    CHECK(apex::LogSink::escape("plain") == "plain");
    CHECK(apex::LogSink::escape("two words") == "\"two words\"");
    CHECK(apex::LogSink::escape("a\"b\nc") == "\"a\\\"b\\nc\"");
    CHECK(apex::LogSink::escape("") == "\"\"");
}

TEST_CASE("T_checkpoint_roundtrip") {
    namespace fs = std::filesystem;
    const auto in = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/berlin52.tsp");
    const fs::path dir = fs::temp_directory_path() / "apex_tsp_ckpt_test";
    fs::remove_all(dir);

    apex::CheckpointV1 c;
    c.instance_hash = apex::instance_hash(in);
    c.instance_name = in.name;
    c.mode = "scalable";
    c.seed = 123;
    c.knn = 8;
    c.time_limit_s = 0.1;
    std::mt19937_64 rng(123);
    rng.discard(17);
    std::ostringstream rs;
    rs << rng;
    c.rng_state = rs.str();
    c.wall_s = 1.0 / 3.0;
    c.best = apex::hilbert_tour(in, apex::hilbert_order(in));

    apex::write_checkpoint(dir.string(), c);
    CHECK_FALSE(fs::exists(dir / "checkpoint.v1.tmp"));
    apex::write_checkpoint(dir.string(), c);  // overwrite existing (atomic replace)
    const auto r = apex::read_checkpoint(dir.string(), in);
    CHECK(r.instance_hash == c.instance_hash);
    CHECK(r.instance_name == c.instance_name);
    CHECK(r.mode == c.mode);
    CHECK(r.seed == c.seed);
    CHECK(r.knn == c.knn);
    CHECK(r.time_limit_s == c.time_limit_s);
    CHECK(r.wall_s == c.wall_s);  // %.17g round-trips exactly
    CHECK(r.best.order == c.best.order);
    CHECK(r.best.cost == c.best.cost);
    std::mt19937_64 restored;
    std::istringstream(r.rng_state) >> restored;
    CHECK(restored() == rng());  // RNG state restored exactly

    // Different instance -> hash mismatch.
    const auto other = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/eil51.tsp");
    CHECK_THROWS_AS(apex::read_checkpoint(dir.string(), other), std::runtime_error);

    // Corrupted cost -> rejected.
    {
        std::ifstream f(dir / "checkpoint.v1");
        std::stringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        const auto p = s.find("best_cost ");
        s.replace(p, s.find('\n', p) - p, "best_cost 1");
        std::ofstream(dir / "checkpoint.v1", std::ios::trunc) << s;
    }
    CHECK_THROWS_AS(apex::read_checkpoint(dir.string(), in), std::runtime_error);
    fs::remove_all(dir);
}
