#pragma once

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

#include "lp/dual_simplex.hpp"

namespace apex {

struct MpsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class MpsFormat { Free, Fixed };

struct MpsModel {
    std::string name;
    Lp lp;                       // constraint rows only (objective and free N rows removed)
    double obj_offset = 0;       // constant term: objective = c^T x + obj_offset
    bool maximize = false;       // OBJSENSE MAX: lp.c already negated
    std::vector<std::string> row_names, col_names;
    // Counts as in the Netlib readme: rows incl. the objective row, nonzeros incl. objective.
    uint32_t file_rows = 0, file_cols = 0;
    uint64_t file_nonzeros = 0;
    uint32_t ranged_rows = 0;    // each RANGES row is split into a Ge and a Le row
};

// Sections: NAME, ROWS, COLUMNS, RHS, RANGES, BOUNDS (UP LO FX FR MI PL BV LI UI), OBJSENSE, ENDATA.
// Free format splits on whitespace (names without spaces); fixed format uses the
// classic column fields (2-3, 5-12, 15-22, 25-36, 40-47, 50-61).
MpsModel parse_mps(std::istream& in, MpsFormat fmt = MpsFormat::Free, const std::string& source = "<stream>");
MpsModel read_mps(const std::string& path, MpsFormat fmt = MpsFormat::Free);

}  // namespace apex
