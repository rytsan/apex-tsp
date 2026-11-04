#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "lp/mps.hpp"

using apex::kInf;
using apex::MpsFormat;
using apex::Sense;

namespace {
apex::MpsModel parse(const std::string& text, MpsFormat fmt = MpsFormat::Free) {
    std::istringstream in(text);
    return apex::parse_mps(in, fmt, "test");
}
}  // namespace

// Counts from the Netlib index, https://www.netlib.org/lp/data/readme (PROBLEM SUMMARY
// TABLE; Rows include the objective row, Nonzeros include objective coefficients).
// Files: COIN-OR Data-Netlib uncompressed copies of the same problems,
// https://github.com/coin-or-tools/Data-Netlib.
TEST_CASE("T_mps_reader_matches_names") {
    struct Ref { const char* file; const char* name; uint32_t rows, cols; uint64_t nnz; };
    const Ref refs[] = {
        {"adlittle", "ADLITTLE", 57, 97, 465},  {"25fv47", "25FV47", 822, 1571, 11127},
        {"sc205", "SC205", 206, 203, 552},      {"share2b", "SHARE2B", 97, 79, 730},
        {"grow22", "GROW22", 441, 946, 8318},
    };
    for (const auto& r : refs) {
        CAPTURE(r.file);
        const auto m = apex::read_mps(std::string(APEX_DATA_DIR) + "/netlib/" + r.file + ".mps");
        CHECK(m.name == r.name);
        CHECK(m.file_rows == r.rows);
        CHECK(m.file_cols == r.cols);
        CHECK(m.file_nonzeros == r.nnz);
        CHECK(m.lp.A.num_cols() == r.cols);
        CHECK(m.lp.c.size() == r.cols);
    }
}

TEST_CASE("T_mps_sections_and_bounds") {
    const std::string text =
        "* comment\n"
        "NAME          TINY\n"
        "OBJSENSE\n"
        "    MAX\n"
        "ROWS\n"
        " N  COST\n"
        " L  LIM1\n"
        " G  LIM2\n"
        " E  MYEQN\n"
        " N  FREE\n"
        "COLUMNS\n"
        "    X1        COST         1.0   LIM1         1.0\n"
        "    X1        LIM2         1.0   FREE         9.0\n"
        "    X2        COST         2.0   LIM1         1.0\n"
        "    X2        MYEQN       -1.0\n"
        "    X3        COST        -1.0   MYEQN        1.0\n"
        "RHS\n"
        "    RHS       COST        -5.0   LIM1         4.0\n"
        "    RHS       LIM2         1.0   MYEQN        7.0\n"
        "RANGES\n"
        "    RNG       LIM1         2.5   MYEQN       -3.0\n"
        "BOUNDS\n"
        " UP BND       X1           4.0\n"
        " MI BND       X2\n"
        " FX BND       X3           2.0\n"
        "ENDATA\n";
    const auto m = parse(text);
    CHECK(m.name == "TINY");
    CHECK(m.maximize);
    CHECK(m.lp.c == std::vector<double>{-1, -2, 1});  // negated for MAX
    CHECK(m.obj_offset == 5.0);
    CHECK(m.file_rows == 5);
    CHECK(m.ranged_rows == 2);
    REQUIRE(m.lp.A.num_rows() == 5);  // LIM1 -> 2 rows, LIM2, MYEQN -> 2 rows
    const auto& rd = m.lp.A.row_data();
    CHECK(rd[0].sense == Sense::Ge); CHECK(rd[0].rhs == 1.5);   // L with range: [4-2.5, 4]
    CHECK(rd[1].sense == Sense::Le); CHECK(rd[1].rhs == 4.0);
    CHECK(rd[2].sense == Sense::Ge); CHECK(rd[2].rhs == 1.0);
    CHECK(rd[3].sense == Sense::Ge); CHECK(rd[3].rhs == 4.0);   // E with negative range: [7-3, 7]
    CHECK(rd[4].sense == Sense::Le); CHECK(rd[4].rhs == 7.0);
    CHECK(m.lp.lower[0] == 0);   CHECK(m.lp.upper[0] == 4);
    CHECK(m.lp.lower[1] == -kInf); CHECK(m.lp.upper[1] == kInf);
    CHECK(m.lp.lower[2] == 2);   CHECK(m.lp.upper[2] == 2);
    CHECK(m.file_nonzeros == 9);  // incl. objective and the free row entry
}

TEST_CASE("T_mps_fixed_format_names_with_spaces") {
    const std::string text =
        "NAME          FIXED\n"
        "ROWS\n"
        " N  OBJ ROW\n"
        " L  CAP A\n"
        "COLUMNS\n"
        "    COL ONE   OBJ ROW            -1.   CAP A               1.\n"
        "RHS\n"
        "    RHS       CAP A               3.\n"
        "BOUNDS\n"
        " UP BND       COL ONE            10.\n"
        "ENDATA\n";
    const auto m = parse(text, MpsFormat::Fixed);
    CHECK(m.col_names == std::vector<std::string>{"COL ONE"});
    CHECK(m.row_names == std::vector<std::string>{"CAP A"});
    CHECK(m.lp.c[0] == -1);
    CHECK(m.lp.A.row_data()[0].rhs == 3);
    CHECK(m.lp.upper[0] == 10);
    const auto r = apex::solve_dual(m.lp, {});
    CHECK(r.objective == doctest::Approx(-3));
}

TEST_CASE("T_mps_rejects_malformed") {
    CHECK_THROWS_AS(parse("NAME X\nROWS\n N OBJ\nCOLUMNS\n X1 NOPE 1\nENDATA\n"), apex::MpsError);
    CHECK_THROWS_AS(parse("NAME X\nROWS\n N OBJ\nCOLUMNS\n X1 OBJ abc\nENDATA\n"), apex::MpsError);
    CHECK_THROWS_AS(parse("NAME X\nROWS\n N OBJ\nCOLUMNS\n X1 OBJ 1\n"), apex::MpsError);  // no ENDATA
    CHECK_THROWS_AS(parse("NAME X\nROWS\n Q R1\nENDATA\n"), apex::MpsError);
    CHECK_THROWS_AS(parse("NAME X\nROWS\n N OBJ\nCOLUMNS\n X1 OBJ 1\nBOUNDS\n ZZ B X1 1\nENDATA\n"), apex::MpsError);
}
