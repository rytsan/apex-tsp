#pragma once

#include <cmath>
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

// Canonical TSPLIB distances (MASTER §2.1, §5.3): double, exact sqrt, original coordinates.
inline int32_t euc2d(double x1, double y1, double x2, double y2) {
    const double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int32_t>(std::floor(std::sqrt(dx * dx + dy * dy) + 0.5));
}

inline int32_t ceil2d(double x1, double y1, double x2, double y2) {
    const double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int32_t>(std::ceil(std::sqrt(dx * dx + dy * dy)));
}

inline int32_t dist(const Instance& in, uint32_t i, uint32_t j) {
    return in.metric == Metric::Ceil2D ? ceil2d(in.xs[i], in.ys[i], in.xs[j], in.ys[j])
                                       : euc2d(in.xs[i], in.ys[i], in.xs[j], in.ys[j]);
}

}  // namespace apex
