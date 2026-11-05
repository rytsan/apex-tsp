#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

#include "core/instance.hpp"
#include "graph/tours.hpp"

namespace apex {

// TSPLIB .tour: node ids written as Instance::original_ids, terminated by -1.
void write_tour(std::ostream& out, const Instance& inst, const Tour& tour, const std::string& comment = "");
// Atomic: written to path + ".tmp", then renamed over path.
void write_tour_file(const std::string& path, const Instance& inst, const Tour& tour, const std::string& comment = "");

// Parses a .tour for inst; validates TYPE, DIMENSION and that the ids form a
// permutation of the instance's nodes. Cost is recomputed with the instance metric.
Tour parse_tour(std::istream& in, const Instance& inst, const std::string& source = "<stream>");
Tour read_tour(const std::string& path, const Instance& inst);

struct Report {
    std::string instance;
    std::string mode;                   // "exact" | "scalable"
    int64_t ub = 0;
    std::optional<int64_t> lb;          // null until Sprints 1-3
    std::optional<std::string> lb_source;  // "lp" | "lagrangian"
    double wall_s = 0;
    uint64_t seed = 0;
    uint32_t threads = 1;
    uint64_t ram_peak_bytes = 0;
};

// gap_pct = 100 * (ub - lb) / ub when lb is present, else null.
void write_report_json(std::ostream& out, const Report& r);
void write_report_file(const std::string& path, const Report& r);

// Peak resident set size of this process (getrusage / GetProcessMemoryInfo).
uint64_t peak_rss_bytes();

}  // namespace apex
