#pragma once

// Whitespace-separated text serialization for checkpoints. Doubles travel as their IEEE bit
// pattern, so every value (incl. inf / nan) round-trips exactly.
#include <bit>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::serial {

inline void put(std::ostream& o, uint64_t v) { o << v << ' '; }
inline void put(std::ostream& o, int64_t v) { o << v << ' '; }
inline void put(std::ostream& o, uint32_t v) { o << v << ' '; }
inline void put(std::ostream& o, int32_t v) { o << v << ' '; }
inline void put(std::ostream& o, uint8_t v) { o << static_cast<uint32_t>(v) << ' '; }
inline void put(std::ostream& o, bool v) { o << (v ? 1 : 0) << ' '; }
inline void put(std::ostream& o, double v) { o << std::bit_cast<uint64_t>(v) << ' '; }

template <class T>
T get(std::istream& in) {
    if constexpr (std::is_same_v<T, double>) {
        uint64_t b = 0;
        if (!(in >> b)) throw std::runtime_error("checkpoint: truncated data");
        return std::bit_cast<double>(b);
    } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, uint8_t>) {
        uint32_t v = 0;
        if (!(in >> v)) throw std::runtime_error("checkpoint: truncated data");
        return static_cast<T>(v);
    } else {
        T v{};
        if (!(in >> v)) throw std::runtime_error("checkpoint: truncated data");
        return v;
    }
}

template <class T>
void put_vec(std::ostream& o, const std::vector<T>& v) {
    put(o, static_cast<uint64_t>(v.size()));
    for (const auto& x : v) put(o, x);
    o << '\n';
}

template <class T>
std::vector<T> get_vec(std::istream& in) {
    const auto n = get<uint64_t>(in);
    if (n > (uint64_t(1) << 34)) throw std::runtime_error("checkpoint: implausible vector size");
    std::vector<T> v;
    v.reserve(static_cast<size_t>(n));
    for (uint64_t k = 0; k < n; ++k) v.push_back(get<T>(in));
    return v;
}

// Section tag: catches a misaligned read early.
inline void tag(std::ostream& o, const char* t) { o << t << '\n'; }
inline void expect(std::istream& in, const char* t) {
    std::string s;
    if (!(in >> s) || s != t) throw std::runtime_error(std::string("checkpoint: expected section '") + t + "', got '" + s + "'");
}

}  // namespace apex::serial
