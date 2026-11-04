#pragma once

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex {

// TSPLIB metric. EUC_2D is the default; CEIL_2D is the operator-approved
// exception for pla85900 (MASTER §2.1).
enum class Metric : uint8_t { Euc2D, Ceil2D };

struct Instance {
    std::string name;
    uint32_t n = 0;
    Metric metric = Metric::Euc2D;
    std::vector<double> xs, ys;            // ORIGINAL file coordinates
    std::vector<uint32_t> original_ids;    // 1-based ids as written in the file
};

struct TsplibError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Parse a TSPLIB symmetric TSP with EUC_2D or CEIL_2D weights.
// Any other TYPE / EDGE_WEIGHT_TYPE / section is a TsplibError.
Instance parse_tsplib(std::istream& in, const std::string& source = "<stream>");
Instance read_tsplib(const std::string& path);

// Writes coordinates with 17 significant digits (exact double round-trip).
void write_tsplib(std::ostream& out, const Instance& inst);

const char* metric_name(Metric m);

}  // namespace apex
