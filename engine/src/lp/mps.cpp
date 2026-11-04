#include "lp/mps.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <istream>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace apex {
namespace {

std::string_view trim(std::string_view s) {
    const auto ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::vector<std::string> fields(std::string_view line, MpsFormat fmt) {
    std::vector<std::string> out;
    if (fmt == MpsFormat::Fixed) {
        static constexpr size_t kStart[] = {1, 4, 14, 24, 39, 49}, kLen[] = {2, 8, 8, 12, 8, 12};
        for (int f = 0; f < 6; ++f) {
            if (kStart[f] >= line.size()) break;
            out.emplace_back(trim(line.substr(kStart[f], kLen[f])));
        }
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    }
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > b) out.emplace_back(line.substr(b, i - b));
    }
    return out;
}

}  // namespace

MpsModel parse_mps(std::istream& in, MpsFormat fmt, const std::string& src) {
    MpsModel mdl;
    enum class Sec { None, Rows, Columns, Rhs, Ranges, Bounds, ObjSense, End } sec = Sec::None;
    struct RowInfo { char type; double rhs = 0, range = 0; bool has_range = false; int32_t lp_row = -1; };
    std::vector<RowInfo> rows;  // all non-objective N rows included (free rows dropped later)
    std::unordered_map<std::string, int32_t> row_of;  // -1 = objective
    std::unordered_map<std::string, uint32_t> col_of;
    std::string obj_name;
    struct Nz { uint32_t row, col; double v; };
    std::vector<Nz> nz;
    std::vector<double> c, lo, up;
    std::string line;
    size_t lineno = 0;
    auto fail = [&](const std::string& msg) -> void { throw MpsError(src + ":" + std::to_string(lineno) + ": " + msg); };
    auto num = [&](const std::string& s) {
        double v = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) fail("invalid number '" + s + "'");
        return v;
    };
    auto row_index = [&](const std::string& name) -> int32_t {
        const auto it = row_of.find(name);
        if (it == row_of.end()) fail("unknown row '" + name + "'");
        return it->second;
    };
    auto col_index = [&](const std::string& name) -> uint32_t {
        const auto it = col_of.find(name);
        if (it == col_of.end()) fail("unknown column '" + name + "'");
        return it->second;
    };

    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '*') continue;
        if (line[0] != ' ' && line[0] != '\t') {  // section header
            const auto f = fields(line, MpsFormat::Free);
            if (f.empty()) continue;
            const auto& h = f[0];
            if (h == "NAME") { mdl.name = f.size() > 1 ? f[1] : ""; sec = Sec::None; }
            else if (h == "ROWS") sec = Sec::Rows;
            else if (h == "COLUMNS") sec = Sec::Columns;
            else if (h == "RHS") sec = Sec::Rhs;
            else if (h == "RANGES") sec = Sec::Ranges;
            else if (h == "BOUNDS") sec = Sec::Bounds;
            else if (h == "OBJSENSE") {
                sec = Sec::ObjSense;
                if (f.size() > 1) mdl.maximize = f[1] == "MAX" || f[1] == "MAXIMIZE";
            } else if (h == "ENDATA") { sec = Sec::End; break; }
            else fail("unknown section '" + h + "'");
            continue;
        }
        auto f = fields(line, fmt);
        // Fixed format: field 1 (cols 2-3) is only used by ROWS and BOUNDS.
        if (fmt == MpsFormat::Fixed && (sec == Sec::Columns || sec == Sec::Rhs || sec == Sec::Ranges) && !f.empty() &&
            f[0].empty())
            f.erase(f.begin());
        if (f.empty()) continue;
        switch (sec) {
            case Sec::ObjSense:
                mdl.maximize = f[0] == "MAX" || f[0] == "MAXIMIZE";
                break;
            case Sec::Rows: {
                if (f.size() < 2) fail("ROWS line needs type and name");
                const char t = f[0].empty() ? '?' : f[0][0];
                if (t == 'N') {
                    if (obj_name.empty()) { obj_name = f[1]; row_of[f[1]] = -1; }
                    else row_of[f[1]] = -2;  // extra free row: ignored
                } else if (t == 'E' || t == 'L' || t == 'G') {
                    if (row_of.count(f[1])) fail("duplicate row '" + f[1] + "'");
                    row_of[f[1]] = static_cast<int32_t>(rows.size());
                    rows.push_back({t});
                    mdl.row_names.push_back(f[1]);
                } else {
                    fail("unknown row type '" + f[0] + "'");
                }
                ++mdl.file_rows;
                break;
            }
            case Sec::Columns: {
                if (f.size() >= 3 && f[1] == "'MARKER'") break;  // integer markers: LP relaxation
                if (f.size() != 3 && f.size() != 5) fail("COLUMNS line needs 3 or 5 fields");
                auto it = col_of.find(f[0]);
                if (it == col_of.end()) {
                    it = col_of.emplace(f[0], static_cast<uint32_t>(c.size())).first;
                    mdl.col_names.push_back(f[0]);
                    c.push_back(0);
                    lo.push_back(0);
                    up.push_back(kInf);
                }
                const uint32_t j = it->second;
                for (size_t k = 1; k + 1 < f.size(); k += 2) {
                    const int32_t r = row_index(f[k]);
                    const double v = num(f[k + 1]);
                    if (v != 0) ++mdl.file_nonzeros;
                    if (r == -1) c[j] += v;
                    else if (r >= 0) nz.push_back({static_cast<uint32_t>(r), j, v});
                }
                break;
            }
            case Sec::Rhs:
            case Sec::Ranges: {
                const size_t first = (f.size() % 2 == 1) ? 1 : 0;  // optional set name
                for (size_t k = first; k + 1 < f.size(); k += 2) {
                    const int32_t r = row_index(f[k]);
                    const double v = num(f[k + 1]);
                    if (sec == Sec::Rhs) {
                        if (r == -1) mdl.obj_offset = -v;  // RHS on objective = -constant
                        else if (r >= 0) rows[r].rhs = v;
                    } else if (r >= 0) {
                        rows[r].range = v;
                        rows[r].has_range = true;
                    }
                }
                break;
            }
            case Sec::Bounds: {
                if (f.size() < 3) fail("BOUNDS line needs type, set and column");
                const std::string& t = f[0];
                const uint32_t j = col_index(f[2]);
                const bool needs_value = !(t == "FR" || t == "MI" || t == "PL" || t == "BV");
                if (needs_value && f.size() < 4) fail("bound " + t + " needs a value");
                const double v = needs_value ? num(f[3]) : 0;
                if (t == "UP" || t == "UI") {
                    up[j] = v;
                    if (v < 0 && lo[j] == 0) lo[j] = -kInf;  // classic MPS convention
                } else if (t == "LO" || t == "LI") lo[j] = v;
                else if (t == "FX") lo[j] = up[j] = v;
                else if (t == "FR") { lo[j] = -kInf; up[j] = kInf; }
                else if (t == "MI") lo[j] = -kInf;
                else if (t == "PL") up[j] = kInf;
                else if (t == "BV") { lo[j] = 0; up[j] = 1; }
                else fail("unknown bound type '" + t + "'");
                break;
            }
            default:
                fail("data line outside a section");
        }
    }
    if (sec != Sec::End) fail("missing ENDATA");
    if (obj_name.empty()) fail("no objective (N) row");

    // Build constraint rows; a ranged row becomes a Ge and a Le row.
    std::vector<RowData> rd;
    std::vector<Triplet> t;
    std::vector<std::vector<uint32_t>> lp_rows_of(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) {
        auto& ri = rows[r];
        auto add = [&](Sense s, double rhs) {
            lp_rows_of[r].push_back(static_cast<uint32_t>(rd.size()));
            rd.push_back({s, rhs});
        };
        if (!ri.has_range) {
            add(ri.type == 'E' ? Sense::Eq : ri.type == 'L' ? Sense::Le : Sense::Ge, ri.rhs);
            continue;
        }
        const double R = std::abs(ri.range);
        double l = ri.rhs, u = ri.rhs;
        if (ri.type == 'G') u = ri.rhs + R;
        else if (ri.type == 'L') l = ri.rhs - R;
        else if (ri.range > 0) u = ri.rhs + R;
        else l = ri.rhs - R;
        ++mdl.ranged_rows;
        add(Sense::Ge, l);
        add(Sense::Le, u);
    }
    for (const auto& e : nz)
        for (uint32_t lr : lp_rows_of[e.row]) t.push_back({lr, e.col, e.v});
    mdl.file_cols = static_cast<uint32_t>(c.size());
    mdl.lp.A = CscMatrix::from_triplets(static_cast<uint32_t>(rd.size()), mdl.file_cols, t, rd);
    if (mdl.maximize)
        for (auto& v : c) v = -v;
    mdl.lp.c = std::move(c);
    mdl.lp.lower = std::move(lo);
    mdl.lp.upper = std::move(up);
    return mdl;
}

MpsModel read_mps(const std::string& path, MpsFormat fmt) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw MpsError(path + ": cannot open file");
    return parse_mps(f, fmt, path);
}

}  // namespace apex
