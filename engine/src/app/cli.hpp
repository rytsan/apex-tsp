#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace apex {

struct CliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string instance;
    std::string mode = "scalable";  // exact | scalable | lab (heuristics lab, M6.4)
    double time_limit_s = 0;        // 0 = unlimited
    uint64_t mem_limit_mb = 4096;   // enforced from Sprint 4 (MASTER §2.3)
    uint32_t threads = 1;
    uint64_t seed = 1;
    uint32_t knn = 8;
    std::string out;                // .tour
    std::string checkpoint;         // directory
    bool resume = false;
    std::string report;             // .json
    uint32_t leaf_size = 800;       // scalable mode: KD-tree leaf cap (decomposition, heuristic)
    uint32_t leaf_exact_limit = 300;  // leaves this small use the exact mini B&C path
    std::string ipc;                // GUI endpoint: socket path (Linux) / pipe name (Windows)
    std::string shared_tour;        // mmap tour file for the GUI (default: <temp>/<endpoint name>.tour)
    std::string lab;                // lab mode: steps run at start, comma separated ("greedy,two_opt,lk")
};

inline constexpr const char* kUsage =
    "usage: apex_tsp --instance <file.tsp> [--mode exact|scalable|lab] [--time-limit <s>]\n"
    "                [--mem-limit-mb <n>] [--threads <n>] [--seed <n>] [--knn <n>]\n"
    "                [--out <route.tour>] [--checkpoint <dir>] [--resume] [--report <report.json>]\n"
    "                [--leaf-size <n>] [--leaf-exact-limit <n>] [--ipc <endpoint>] [--shared-tour <file>]\n"
    "                [--lab <step,step,...>]   (lab: nn greedy hilbert random decomp two_opt two_opt_oropt lk ils[:s])\n";

namespace cli_detail {
template <class T>
T number(std::string_view flag, std::string_view v) {
    T x{};
    const auto r = std::from_chars(v.data(), v.data() + v.size(), x);
    if (r.ec != std::errc{} || r.ptr != v.data() + v.size())
        throw CliError("invalid value '" + std::string(v) + "' for " + std::string(flag));
    return x;
}
}  // namespace cli_detail

// Parses argv[1..]. Unknown flags, missing values and bad numbers are errors.
inline Options parse_cli(const std::vector<std::string>& args) {
    using cli_detail::number;
    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& f = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) throw CliError("missing value for " + f);
            return args[++i];
        };
        if (f == "--instance") o.instance = value();
        else if (f == "--mode") o.mode = value();
        else if (f == "--time-limit") o.time_limit_s = number<double>(f, value());
        else if (f == "--mem-limit-mb") o.mem_limit_mb = number<uint64_t>(f, value());
        else if (f == "--threads") o.threads = number<uint32_t>(f, value());
        else if (f == "--seed") o.seed = number<uint64_t>(f, value());
        else if (f == "--knn") o.knn = number<uint32_t>(f, value());
        else if (f == "--out") o.out = value();
        else if (f == "--checkpoint") o.checkpoint = value();
        else if (f == "--resume") o.resume = true;
        else if (f == "--report") o.report = value();
        else if (f == "--leaf-size") o.leaf_size = number<uint32_t>(f, value());
        else if (f == "--leaf-exact-limit") o.leaf_exact_limit = number<uint32_t>(f, value());
        else if (f == "--ipc") o.ipc = value();
        else if (f == "--shared-tour") o.shared_tour = value();
        else if (f == "--lab") o.lab = value();
        else throw CliError("unknown flag '" + f + "'");
    }
    if (o.instance.empty()) throw CliError("--instance is required");
    if (o.mode != "exact" && o.mode != "scalable" && o.mode != "lab") throw CliError("--mode must be exact, scalable or lab");
    if (!o.lab.empty() && o.mode != "lab") throw CliError("--lab needs --mode lab");
    if (o.time_limit_s < 0) throw CliError("--time-limit must be >= 0");
    if (o.threads == 0) throw CliError("--threads must be >= 1");
    if (o.knn == 0) throw CliError("--knn must be >= 1");
    if (o.leaf_size < 8) throw CliError("--leaf-size must be >= 8");
    if (o.mem_limit_mb == 0) throw CliError("--mem-limit-mb must be >= 1");
    if (o.resume && o.checkpoint.empty()) throw CliError("--resume requires --checkpoint <dir>");
    return o;
}

}  // namespace apex
