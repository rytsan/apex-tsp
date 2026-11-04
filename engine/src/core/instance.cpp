#include "core/instance.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <istream>
#include <string_view>
#include <system_error>

namespace apex {
namespace {

std::string_view trim(std::string_view s) {
    const auto ws = " \t\r\n\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

[[noreturn]] void fail(const std::string& source, size_t line, const std::string& msg) {
    throw TsplibError(source + ":" + std::to_string(line) + ": " + msg);
}

// Split "KEY : VALUE", "KEY: VALUE" or bare "KEY".
void split_key(std::string_view line, std::string_view& key, std::string_view& value) {
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
        key = trim(line.substr(0, colon));
        value = trim(line.substr(colon + 1));
        return;
    }
    const auto sp = line.find_first_of(" \t");
    key = trim(line.substr(0, sp));
    value = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));
}

// Next whitespace-separated token from s (advances s).
std::string_view next_token(std::string_view& s) {
    const auto ws = " \t\r\n\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) { s = {}; return {}; }
    s.remove_prefix(b);
    const auto e = s.find_first_of(ws);
    const auto tok = s.substr(0, e);
    s.remove_prefix(e == std::string_view::npos ? s.size() : e);
    return tok;
}

template <class T>
bool parse_num(std::string_view tok, T& out) {
    if (tok.empty()) return false;
    if (tok.front() == '+') tok.remove_prefix(1);
    const auto* end = tok.data() + tok.size();
    const auto r = std::from_chars(tok.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

}  // namespace

const char* metric_name(Metric m) {
    return m == Metric::Ceil2D ? "CEIL_2D" : "EUC_2D";
}

Instance parse_tsplib(std::istream& in, const std::string& source) {
    Instance inst;
    bool have_type = false, have_dim = false, have_ewt = false, have_coords = false;
    std::string raw;
    size_t lineno = 0;

    while (std::getline(in, raw)) {
        ++lineno;
        const auto line = trim(raw);
        if (line.empty()) continue;

        std::string_view key, value;
        split_key(line, key, value);

        if (key == "EOF") break;
        if (key == "NAME") { inst.name = std::string(value); continue; }
        if (key == "COMMENT") continue;
        if (key == "TYPE") {
            if (value != "TSP") fail(source, lineno, "unsupported TYPE '" + std::string(value) + "' (only TSP)");
            have_type = true;
            continue;
        }
        if (key == "DIMENSION") {
            uint32_t d = 0;
            if (!parse_num(value, d) || d < 3) fail(source, lineno, "invalid DIMENSION '" + std::string(value) + "'");
            inst.n = d;
            have_dim = true;
            continue;
        }
        if (key == "EDGE_WEIGHT_TYPE") {
            if (value == "EUC_2D") inst.metric = Metric::Euc2D;
            else if (value == "CEIL_2D") inst.metric = Metric::Ceil2D;
            else fail(source, lineno, "unsupported EDGE_WEIGHT_TYPE '" + std::string(value) + "' (only EUC_2D, CEIL_2D)");
            have_ewt = true;
            continue;
        }
        if (key == "NODE_COORD_SECTION") {
            if (!have_type || !have_dim || !have_ewt)
                fail(source, lineno, "NODE_COORD_SECTION before TYPE/DIMENSION/EDGE_WEIGHT_TYPE");
            if (have_coords) fail(source, lineno, "duplicate NODE_COORD_SECTION");
            inst.xs.resize(inst.n);
            inst.ys.resize(inst.n);
            inst.original_ids.resize(inst.n);
            std::vector<uint8_t> seen(inst.n, 0);
            for (uint32_t k = 0; k < inst.n; ++k) {
                if (!std::getline(in, raw)) fail(source, lineno, "unexpected end of file in NODE_COORD_SECTION");
                ++lineno;
                std::string_view rest = raw;
                if (trim(rest).empty()) { --k; continue; }
                uint32_t id = 0;
                double x = 0, y = 0;
                const auto t_id = next_token(rest), t_x = next_token(rest), t_y = next_token(rest);
                if (!parse_num(t_id, id) || !parse_num(t_x, x) || !parse_num(t_y, y) || !trim(rest).empty())
                    fail(source, lineno, "malformed coordinate line '" + std::string(trim(raw)) + "'");
                if (id < 1 || id > inst.n) fail(source, lineno, "node id " + std::to_string(id) + " out of range 1.." + std::to_string(inst.n));
                if (seen[id - 1]) fail(source, lineno, "duplicate node id " + std::to_string(id));
                seen[id - 1] = 1;
                inst.xs[k] = x;
                inst.ys[k] = y;
                inst.original_ids[k] = id;
            }
            have_coords = true;
            continue;
        }
        if (key == "DISPLAY_DATA_SECTION")
            fail(source, lineno, "DISPLAY_DATA_SECTION is not supported (coordinates must come from NODE_COORD_SECTION)");
        fail(source, lineno, "unsupported keyword '" + std::string(key) + "'");
    }

    if (!have_type) fail(source, lineno, "missing TYPE");
    if (!have_dim) fail(source, lineno, "missing DIMENSION");
    if (!have_ewt) fail(source, lineno, "missing EDGE_WEIGHT_TYPE");
    if (!have_coords) fail(source, lineno, "missing NODE_COORD_SECTION");
    return inst;
}

Instance read_tsplib(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw TsplibError(path + ": cannot open file");
    return parse_tsplib(f, path);
}

void write_tsplib(std::ostream& out, const Instance& inst) {
    out << "NAME : " << inst.name << "\nTYPE : TSP\nDIMENSION : " << inst.n
        << "\nEDGE_WEIGHT_TYPE : " << metric_name(inst.metric) << "\nNODE_COORD_SECTION\n";
    char buf[96];
    for (uint32_t k = 0; k < inst.n; ++k) {
        std::snprintf(buf, sizeof buf, "%u %.17g %.17g\n", inst.original_ids[k], inst.xs[k], inst.ys[k]);
        out << buf;
    }
    out << "EOF\n";
}

}  // namespace apex
