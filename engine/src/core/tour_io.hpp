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
    std::string status = "done";        // "done" | "MEM_LIMIT" | B&C status | "CANCELLED"
    // Sprint 5 schema (M5.2): the IPC FINISHED payload is this JSON, byte for byte.
    uint64_t disk_bytes = 0;            // checkpoint directory size at the end
    uint64_t iterations = 0;            // simplex iterations (exact mode; 0 in scalable mode)
    uint64_t refactorizations = 0;
    uint64_t cuts_sec = 0, cuts_blossom = 0, cuts_component = 0, cuts_comb = 0, cuts_dp = 0;  // cuts_by_family
    uint64_t tours_found = 0;           // UB improvements over the run
    std::string hardware;               // CPU brand string + logical cores
    uint64_t mem_limit_mb = 0;
    uint64_t verification_rounds = 0;   // complete-graph pricing passes (exact mode)
};

// gap_pct = 100 * (ub - lb) / ub when lb is present, else null.
std::string report_json(const Report& r);
void write_report_json(std::ostream& out, const Report& r);
void write_report_file(const std::string& path, const Report& r);

// Peak resident set size of this process (getrusage / GetProcessMemoryInfo).
uint64_t peak_rss_bytes();

// "<CPU brand string> (<n> logical cores)" via CPUID on x86-64, "unknown" elsewhere.
std::string hardware_string();

}  // namespace apex
