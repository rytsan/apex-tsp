#pragma once

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace apex {

using KV = std::pair<std::string_view, std::string>;

// Telemetry events. Log-backed now; pipe-backed implementation arrives in Sprint 5.
class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;
    virtual void event(std::string_view name, std::initializer_list<KV> fields) = 0;
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
