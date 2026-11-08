#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex {

using KV = std::pair<std::string_view, std::string>;

struct LpSupportEdge {
    uint32_t u, v;
    double x;
};

enum class TreeNodeState : uint8_t { Open, Pruned, Infeasible, Integral, Branched, Reopened };

struct TreeNodeInfo {
    uint32_t id = 0, parent = UINT32_MAX, depth = 0;
    TreeNodeState state = TreeNodeState::Open;
    uint8_t branch_dir = 0;
    uint32_t branch_u = UINT32_MAX, branch_v = UINT32_MAX;
    double bound = 0, lp_value = 0;
    uint64_t cuts_sec = 0, cuts_blossom = 0, cuts_component = 0;
};

// Telemetry events: key=value events (log / IPC), plus typed solver views for the GUI (Sprint 6
// M6.1) that the log sink ignores. Typed hooks are called only from the solver's master thread.
class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;
    virtual void event(std::string_view name, std::initializer_list<KV> fields) = 0;
    // True when a viewer wants the next LP support (rate-limited by the sink): callers build the
    // support only then.
    virtual bool wants_lp_solution() { return false; }
    virtual void lp_solution(const std::vector<LpSupportEdge>&) {}
    virtual void tree_node(const TreeNodeInfo&) {}
};

// One `key=value` line per event, written with a single fwrite + fflush.
class LogSink final : public TelemetrySink {
public:
    explicit LogSink(std::FILE* out) : out_(out), t0_(std::chrono::steady_clock::now()) {}

    void event(std::string_view name, std::initializer_list<KV> fields) override {
        char ts[32];
        std::snprintf(ts, sizeof ts, "%.3f",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count());
        std::string line = "t=";
        line += ts;
        line += " event=";
        line += name;
        for (const auto& [k, v] : fields) {
            line += ' ';
            line += k;
            line += '=';
            append_value(line, v);
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), out_);
        std::fflush(out_);
    }

    static std::string escape(std::string_view v) {
        std::string s;
        append_value(s, v);
        return s;
    }

private:
    static void append_value(std::string& line, std::string_view v) {
        const bool quote = v.empty() || v.find_first_of(" \t\"=\n\r") != std::string_view::npos;
        if (!quote) { line += v; return; }
        line += '"';
        for (char c : v) {
            if (c == '"' || c == '\\') line += '\\';
            if (c == '\n') { line += "\\n"; continue; }
            if (c == '\r') { line += "\\r"; continue; }
            line += c;
        }
        line += '"';
    }

    std::FILE* out_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace apex
