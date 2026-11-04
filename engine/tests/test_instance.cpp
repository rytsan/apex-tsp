#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "core/instance.hpp"

using apex::Instance;
using apex::Metric;

namespace {
Instance parse(const std::string& text) {
    std::istringstream in(text);
    return apex::parse_tsplib(in, "test");
}
}  // namespace

TEST_CASE("T_tsplib_roundtrip_minimal") {
    Instance a;
    a.name = "tiny";
    a.n = 4;
    a.metric = Metric::Euc2D;
    a.xs = {0.0, 1e6, -3.25, 0.1};
    a.ys = {0.0, 2.5, 7.0 / 3.0, 1e-9};
    a.original_ids = {1, 2, 3, 4};

    std::ostringstream out;
    apex::write_tsplib(out, a);
    const auto b = parse(out.str());

    CHECK(b.name == a.name);
    CHECK(b.n == a.n);
    CHECK(b.metric == a.metric);
    CHECK(b.xs == a.xs);  // bit-exact
    CHECK(b.ys == a.ys);
    CHECK(b.original_ids == a.original_ids);
}

TEST_CASE("T_tsplib_tolerates_crlf_and_whitespace") {
    const auto inst = parse(
        "NAME:  t3\r\nCOMMENT : hello world\r\nTYPE: TSP\r\nDIMENSION :3\r\n"
        "EDGE_WEIGHT_TYPE\t:\tCEIL_2D\r\nNODE_COORD_SECTION\r\n"
        "  1   1.5e+02\t2\r\n\r\n2 3 4\r\n 3 -5 6.0  \r\nEOF\r\n");
    CHECK(inst.name == "t3");
    CHECK(inst.n == 3);
    CHECK(inst.metric == Metric::Ceil2D);
    CHECK(inst.xs[0] == 150.0);
    CHECK(inst.ys[2] == 6.0);
}

TEST_CASE("T_tsplib_rejects_non_euc2d") {
    const std::string tail = "DIMENSION: 3\nNODE_COORD_SECTION\n1 0 0\n2 1 1\n3 2 2\nEOF\n";
    CHECK_THROWS_AS(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: GEO\n" + tail), apex::TsplibError);
    CHECK_THROWS_AS(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: ATT\n" + tail), apex::TsplibError);
    CHECK_THROWS_AS(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: EXPLICIT\n" + tail), apex::TsplibError);
    CHECK_THROWS_AS(parse("TYPE: ATSP\nEDGE_WEIGHT_TYPE: EUC_2D\n" + tail), apex::TsplibError);
    CHECK_THROWS_AS(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: EUC_2D\nDIMENSION: 3\n"
                          "DISPLAY_DATA_SECTION\n1 0 0\n2 1 1\n3 2 2\nEOF\n"),
                    apex::TsplibError);
    CHECK_NOTHROW(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: EUC_2D\n" + tail));
}

TEST_CASE("T_tsplib_rejects_malformed") {
    const std::string head = "TYPE: TSP\nEDGE_WEIGHT_TYPE: EUC_2D\nDIMENSION: 3\nNODE_COORD_SECTION\n";
    CHECK_THROWS_AS(parse(head + "1 0 0\n2 1 1\n"), apex::TsplibError);           // truncated
    CHECK_THROWS_AS(parse(head + "1 0 0\n1 1 1\n3 2 2\nEOF\n"), apex::TsplibError);  // duplicate id
    CHECK_THROWS_AS(parse(head + "1 0 0\n2 1 1\n4 2 2\nEOF\n"), apex::TsplibError);  // id out of range
    CHECK_THROWS_AS(parse(head + "1 0 0\n2 1 x\n3 2 2\nEOF\n"), apex::TsplibError);  // bad number
    CHECK_THROWS_AS(parse("TYPE: TSP\nDIMENSION: 3\nNODE_COORD_SECTION\n1 0 0\n2 1 1\n3 2 2\n"),
                    apex::TsplibError);  // missing EDGE_WEIGHT_TYPE
    CHECK_THROWS_AS(parse("TYPE: TSP\nEDGE_WEIGHT_TYPE: EUC_2D\nDIMENSION: 3\nCAPACITY: 5\n"),
                    apex::TsplibError);  // unknown keyword
    CHECK_THROWS_AS(apex::read_tsplib("/nonexistent/file.tsp"), apex::TsplibError);
}

// MASTER §7: every reference instance loads; DIMENSION matches the table (G0.1).
TEST_CASE("T_tsplib_reference_instances") {
    struct Ref { const char* name; uint32_t n; Metric metric; };
    const Ref refs[] = {
        {"eil51", 51, Metric::Euc2D},        {"berlin52", 52, Metric::Euc2D},
        {"a280", 280, Metric::Euc2D},        {"pr1002", 1002, Metric::Euc2D},
        {"fl3795", 3795, Metric::Euc2D},     {"d15112", 15112, Metric::Euc2D},
        {"d18512", 18512, Metric::Euc2D},    {"usa13509", 13509, Metric::Euc2D},
        {"pla85900", 85900, Metric::Ceil2D}, {"mona-lisa100K", 100000, Metric::Euc2D},
    };
    for (const auto& r : refs) {
        CAPTURE(r.name);
        const auto inst = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + r.name + ".tsp");
        CHECK(inst.n == r.n);
        CHECK(inst.xs.size() == r.n);
        CHECK(inst.metric == r.metric);
    }
}
