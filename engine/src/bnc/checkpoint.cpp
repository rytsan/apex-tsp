#include "bnc/checkpoint.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace apex {
namespace {

constexpr const char* kMagic = "APEX-TSP-CHECKPOINT v1";

void fnv(uint64_t& h, const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

[[noreturn]] void bad(const std::string& path, const std::string& msg) {
    throw std::runtime_error(path + ": invalid checkpoint: " + msg);
}

}  // namespace

uint64_t instance_hash(const Instance& inst) {
    uint64_t h = 1469598103934665603ull;
    fnv(h, &inst.n, sizeof inst.n);
    const auto m = static_cast<uint8_t>(inst.metric);
    fnv(h, &m, 1);
    for (uint32_t i = 0; i < inst.n; ++i) {
        fnv(h, &inst.xs[i], sizeof(double));
        fnv(h, &inst.ys[i], sizeof(double));
    }
    return h;
}

void write_checkpoint(const std::string& dir, const CheckpointV1& c) {
    namespace fs = std::filesystem;
    fs::create_directories(dir);
    const fs::path target = fs::path(dir) / kCheckpointFile;
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error(tmp.string() + ": cannot open for writing");
        f << kMagic << '\n'
          << "instance_hash " << c.instance_hash << '\n'
          << "instance_name " << c.instance_name << '\n'
          << "mode " << c.mode << '\n'
          << "seed " << c.seed << '\n'
          << "knn " << c.knn << '\n'
          << std::setprecision(17) << "time_limit_s " << c.time_limit_s << '\n'
          << "wall_s " << c.wall_s << '\n'
          << "rng " << c.rng_state << '\n'
          << "best_cost " << c.best.cost << '\n'
          << "n " << c.best.order.size() << '\n'
          << "tour";
        for (uint32_t v : c.best.order) f << ' ' << v;
        f << "\nend\n";
        f.flush();
        if (!f) throw std::runtime_error(tmp.string() + ": write failed");
    }
    fs::rename(tmp, target);  // atomic replace on POSIX; MoveFileEx(REPLACE_EXISTING) on Windows
}

CheckpointV1 read_checkpoint(const std::string& dir, const Instance& inst) {
    const std::string path = (std::filesystem::path(dir) / kCheckpointFile).string();
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(path + ": cannot open checkpoint");
    std::string line;
    if (!std::getline(f, line) || line != kMagic) bad(path, "bad header");

    CheckpointV1 c;
    auto field = [&](const char* key) -> std::string {
        if (!std::getline(f, line)) bad(path, std::string("missing ") + key);
        const size_t k = std::strlen(key);
        if (line.compare(0, k, key) != 0 || (line.size() > k && line[k] != ' ')) bad(path, std::string("expected ") + key);
        return line.size() > k ? line.substr(k + 1) : std::string();
    };
    auto num = [&](const char* key, auto& out) {
        std::istringstream s(field(key));
        if (!(s >> out)) bad(path, std::string("bad ") + key);
    };
    num("instance_hash", c.instance_hash);
    c.instance_name = field("instance_name");
    c.mode = field("mode");
    num("seed", c.seed);
    num("knn", c.knn);
    num("time_limit_s", c.time_limit_s);
    num("wall_s", c.wall_s);
    c.rng_state = field("rng");
    num("best_cost", c.best.cost);
    size_t n = 0;
    num("n", n);
    {
        std::istringstream s(field("tour"));
        c.best.order.resize(n);
        for (auto& v : c.best.order)
            if (!(s >> v)) bad(path, "truncated tour");
        uint32_t extra;
        if (s >> extra) bad(path, "tour longer than n");
    }
    if (!std::getline(f, line) || line != "end") bad(path, "missing end marker");

    if (c.instance_hash != instance_hash(inst)) bad(path, "instance hash mismatch (different instance?)");
    if (!is_valid_tour(c.best, inst.n)) bad(path, "tour is not a permutation of the instance nodes");
    if (tour_cost(inst, c.best) != c.best.cost) bad(path, "best_cost does not match recomputed tour cost");
    return c;
}

}  // namespace apex
