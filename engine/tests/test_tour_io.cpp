#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "core/instance.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {
apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}
apex::Tour parse(const std::string& text, const apex::Instance& in) {
    std::istringstream s(text);
    return apex::parse_tour(s, in, "test");
}
}  // namespace

// Published TSPLIB optimal tours must evaluate to the MASTER §7 optima with our metric.
TEST_CASE("T_tour_io_tsplib_opt_tours") {
    struct Ref { const char* name; int64_t opt; };
    for (const auto& r : {Ref{"eil51", 426}, Ref{"berlin52", 7542}, Ref{"a280", 2579}, Ref{"pr1002", 259045}}) {
        CAPTURE(r.name);
        const auto in = load(r.name);
        const auto t = apex::read_tour(std::string(APEX_DATA_DIR) + "/" + r.name + ".opt.tour", in);
        CHECK(apex::is_valid_tour(t, in.n));
        CHECK(t.cost == r.opt);
    }
}

TEST_CASE("T_tour_io_roundtrip") {
    const auto in = load("a280");
    auto t = apex::hilbert_tour(in, apex::hilbert_order(in));
    std::ostringstream out;
    apex::write_tour(out, in, t, "roundtrip");
    const auto back = parse(out.str(), in);
    CHECK(back.order == t.order);
    CHECK(back.cost == t.cost);
}

TEST_CASE("T_tour_io_rejects_invalid") {
    apex::Instance in;
    in.name = "t";
    in.n = 4;
    in.xs = {0, 1, 1, 0};
    in.ys = {0, 0, 1, 1};
    in.original_ids = {1, 2, 3, 4};
    const std::string head = "TYPE : TOUR\nDIMENSION : 4\nTOUR_SECTION\n";
    CHECK(parse(head + "1 2\n3 4\n-1\nEOF\n", in).cost == 4);           // several ids per line
    CHECK_THROWS_AS(parse(head + "1\n2\n3\n3\n-1\n", in), apex::TsplibError);  // duplicate
    CHECK_THROWS_AS(parse(head + "1\n2\n3\n-1\n", in), apex::TsplibError);     // missing node
    CHECK_THROWS_AS(parse(head + "1\n2\n3\n5\n-1\n", in), apex::TsplibError);  // unknown id
    CHECK_THROWS_AS(parse(head + "1\n2\n3\n4\n", in), apex::TsplibError);      // no -1
    CHECK_THROWS_AS(parse("TYPE : TOUR\nDIMENSION : 5\nTOUR_SECTION\n1\n2\n3\n4\n-1\n", in), apex::TsplibError);
    CHECK_THROWS_AS(parse("TYPE : TSP\nDIMENSION : 4\nTOUR_SECTION\n1\n2\n3\n4\n-1\n", in), apex::TsplibError);
}

TEST_CASE("T_report_json") {
    apex::Report r;
    r.instance = "mona \"lisa\"\n";
    r.mode = "scalable";
    r.ub = 5938927;
    r.wall_s = 1.23456;
    r.seed = 42;
    r.threads = 1;
    r.ram_peak_bytes = 123;
    r.hardware = "cpu";
    r.mem_limit_mb = 4096;
    std::ostringstream a;
    apex::write_report_json(a, r);
    CHECK(a.str() ==
          "{\"instance\":\"mona \\\"lisa\\\"\\n\",\"mode\":\"scalable\",\"status\":\"done\",\"ub\":5938927,\"lb\":null,"
          "\"lb_source\":null,\"gap_pct\":null,\"wall_s\":1.235,\"ram_peak_bytes\":123,\"disk_bytes\":0,\"iterations\":0,"
          "\"refactorizations\":0,\"cuts_by_family\":{\"sec\":0,\"blossom\":0,\"component\":0,\"comb\":0,\"domino\":0},\"tours_found\":0,"
          "\"seed\":42,\"threads\":1,\"hardware\":\"cpu\",\"mem_limit_mb\":4096,\"verification_rounds\":0}\n");
    CHECK(a.str() == apex::report_json(r));
    r.lb = 5757084;
    r.lb_source = "lp";
    std::ostringstream b;
    apex::write_report_json(b, r);
    CHECK(b.str().find("\"lb\":5757084,\"lb_source\":\"lp\",\"gap_pct\":3.061883") != std::string::npos);
    CHECK(apex::hardware_string().find("logical cores") != std::string::npos);
    CHECK(apex::peak_rss_bytes() > 0);
}
