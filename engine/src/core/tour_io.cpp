#include "core/tour_io.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace apex {
namespace {

std::string_view trim(std::string_view s) {
    const auto ws = " \t\r\n\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

[[noreturn]] void fail(const std::string& src, size_t line, const std::string& msg) {
    throw TsplibError(src + ":" + std::to_string(line) + ": " + msg);
}

std::ofstream open_out(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error(path + ": cannot open for writing");
    return f;
}

void json_string(std::ostream& out, const std::string& s) {
    out << '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out << buf;
                } else {
                    out << ch;
                }
        }
    }
    out << '"';
}

}  // namespace

void write_tour(std::ostream& out, const Instance& inst, const Tour& tour, const std::string& comment) {
    out << "NAME : " << inst.name << ".tour\n";
    if (!comment.empty()) out << "COMMENT : " << comment << "\n";
    out << "TYPE : TOUR\nDIMENSION : " << tour.order.size() << "\nTOUR_SECTION\n";
    for (uint32_t v : tour.order) out << inst.original_ids[v] << "\n";
    out << "-1\nEOF\n";
}

void write_tour_file(const std::string& path, const Instance& inst, const Tour& tour, const std::string& comment) {
    const std::string tmp = path + ".tmp";
    {
        auto f = open_out(tmp);
        write_tour(f, inst, tour, comment);
        if (!f.flush()) throw std::runtime_error(tmp + ": write failed");
    }
    std::filesystem::rename(tmp, path);  // atomic replace: readers never see a partial tour
}

Tour parse_tour(std::istream& in, const Instance& inst, const std::string& src) {
    std::unordered_map<uint32_t, uint32_t> index_of;  // original id -> node index
    index_of.reserve(inst.n * 2);
    for (uint32_t i = 0; i < inst.n; ++i) index_of.emplace(inst.original_ids[i], i);

    Tour t;
    bool have_type = false, in_section = false, done = false;
    int64_t dimension = -1;
    std::vector<uint8_t> seen(inst.n, 0);
    std::string raw;
    size_t lineno = 0;
    while (!done && std::getline(in, raw)) {
        ++lineno;
        std::string_view line = trim(raw);
        if (line.empty()) continue;
        if (!in_section) {
            const auto colon = line.find(':');
            const auto key = trim(line.substr(0, colon == std::string_view::npos ? line.find_first_of(" \t") : colon));
            const auto value = colon == std::string_view::npos ? std::string_view{} : trim(line.substr(colon + 1));
            if (key == "NAME" || key == "COMMENT") continue;
            if (key == "TYPE") {
                if (value != "TOUR") fail(src, lineno, "TYPE must be TOUR");
                have_type = true;
            } else if (key == "DIMENSION") {
                const auto r = std::from_chars(value.data(), value.data() + value.size(), dimension);
                if (r.ec != std::errc{} || r.ptr != value.data() + value.size())
                    fail(src, lineno, "invalid DIMENSION");
                if (dimension != inst.n)
                    fail(src, lineno, "DIMENSION " + std::to_string(dimension) + " != instance n " + std::to_string(inst.n));
            } else if (key == "TOUR_SECTION") {
                if (!have_type) fail(src, lineno, "TOUR_SECTION before TYPE");
                in_section = true;
            } else if (key == "EOF") {
                break;
            } else {
                fail(src, lineno, "unsupported keyword '" + std::string(key) + "'");
            }
            continue;
        }
        while (!line.empty()) {  // any number of ids per line
            const auto sp = line.find_first_of(" \t");
            const auto tok = line.substr(0, sp);
            line = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));
            int64_t id = 0;
            const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), id);
            if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size())
                fail(src, lineno, "invalid node id '" + std::string(tok) + "'");
            if (id == -1) { done = true; break; }
            const auto it = id > 0 ? index_of.find(static_cast<uint32_t>(id)) : index_of.end();
            if (it == index_of.end()) fail(src, lineno, "unknown node id " + std::to_string(id));
            if (seen[it->second]) fail(src, lineno, "duplicate node id " + std::to_string(id));
            seen[it->second] = 1;
            t.order.push_back(it->second);
        }
    }
    if (!in_section) fail(src, lineno, "missing TOUR_SECTION");
    if (!done) fail(src, lineno, "TOUR_SECTION not terminated by -1");
    if (t.order.size() != inst.n)
        fail(src, lineno, "tour has " + std::to_string(t.order.size()) + " nodes, instance has " + std::to_string(inst.n));
    t.cost = tour_cost(inst, t);
    return t;
}

Tour read_tour(const std::string& path, const Instance& inst) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw TsplibError(path + ": cannot open file");
    return parse_tour(f, inst, path);
}

std::string report_json(const Report& r) {
    std::ostringstream out;
    char num[64];
    out << "{\"instance\":";
    json_string(out, r.instance);
    out << ",\"mode\":";
    json_string(out, r.mode);
    out << ",\"status\":";
    json_string(out, r.status);
    out << ",\"ub\":" << r.ub << ",\"lb\":";
    if (r.lb) out << *r.lb; else out << "null";
    out << ",\"lb_source\":";
    if (r.lb_source) json_string(out, *r.lb_source); else out << "null";
    out << ",\"gap_pct\":";
    if (r.lb && r.ub != 0) {
        std::snprintf(num, sizeof num, "%.6f", 100.0 * static_cast<double>(r.ub - *r.lb) / static_cast<double>(r.ub));
        out << num;
    } else {
        out << "null";
    }
    std::snprintf(num, sizeof num, "%.3f", r.wall_s);
    out << ",\"wall_s\":" << num << ",\"ram_peak_bytes\":" << r.ram_peak_bytes << ",\"disk_bytes\":" << r.disk_bytes
        << ",\"iterations\":" << r.iterations << ",\"refactorizations\":" << r.refactorizations
        << ",\"cuts_by_family\":{\"sec\":" << r.cuts_sec << ",\"blossom\":" << r.cuts_blossom
        << ",\"component\":" << r.cuts_component << "},\"tours_found\":" << r.tours_found << ",\"seed\":" << r.seed
        << ",\"threads\":" << r.threads << ",\"hardware\":";
    json_string(out, r.hardware);
    out << ",\"mem_limit_mb\":" << r.mem_limit_mb << ",\"verification_rounds\":" << r.verification_rounds << "}\n";
    return out.str();
}

void write_report_json(std::ostream& out, const Report& r) { out << report_json(r); }

void write_report_file(const std::string& path, const Report& r) {
    auto f = open_out(path);
    write_report_json(f, r);
    if (!f.flush()) throw std::runtime_error(path + ": write failed");
}

uint64_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.PeakWorkingSetSize;
    return 0;
#else
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(ru.ru_maxrss);  // bytes on macOS
#else
    return static_cast<uint64_t>(ru.ru_maxrss) * 1024;  // KiB on Linux
#endif
#endif
}

std::string hardware_string() {
    std::string brand;
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    unsigned int regs[12] = {};
    for (unsigned int k = 0; k < 3; ++k) {
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, static_cast<int>(0x80000002u + k));
        for (int j = 0; j < 4; ++j) regs[4 * k + j] = static_cast<unsigned int>(r[j]);
#else
        __get_cpuid(0x80000002u + k, &regs[4 * k], &regs[4 * k + 1], &regs[4 * k + 2], &regs[4 * k + 3]);
#endif
    }
    brand.assign(reinterpret_cast<const char*>(regs), sizeof regs);
    brand = std::string(trim(brand.c_str()));
#endif
    if (brand.empty()) brand = "unknown";
    return brand + " (" + std::to_string(std::thread::hardware_concurrency()) + " logical cores)";
}

}  // namespace apex
