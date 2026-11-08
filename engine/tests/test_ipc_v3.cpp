// Protocol v3 (Sprint 6 M6.1): typed solver events, v2 compatibility, LP view rate limit,
// tree events consistent with the B&C result.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "bnc/tree.hpp"
#include "geo/hilbert.hpp"
#include "ipc/protocol.hpp"
#include "ipc/server.hpp"
#include "ipc/transport.hpp"
#include "primal/two_opt.hpp"

namespace fs = std::filesystem;
using namespace apex::ipc;
using Clock = std::chrono::steady_clock;

namespace {

template <class T>
std::vector<uint8_t> bytes_of(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    return {p, p + sizeof(T)};
}

template <class T>
T read_as(const std::vector<uint8_t>& p, size_t off = 0) {
    T v;
    std::memcpy(&v, p.data() + off, sizeof(T));
    return v;
}

std::string endpoint_for(const fs::path& dir, const char* tag) {
#ifdef _WIN32
    (void)dir;
    return std::string("apex_v3_") + tag + "_" + std::to_string(Clock::now().time_since_epoch().count() % 1000000);
#else
    (void)tag;
    return (dir / "e.sock").string();
#endif
}

// Raw client: handshake with the given version range, then collect frames for `secs`.
struct RawClient {
    std::unique_ptr<Transport> t;
    FrameDecoder d;
    bool connect(const std::string& ep, uint16_t version, uint16_t min) {
        t = apex::ipc::connect(ep, 5000);
        if (!t) return false;
        Hello h;
        h.version = version;
        h.protocol_min = min;
        const auto w = encode(MsgType::Hello, pack(h), false, min);
        return t->write_all(w.data(), w.size());
    }
    std::vector<Frame> collect(double secs) {
        std::vector<Frame> out;
        const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(secs));
        uint8_t buf[65536];
        while (Clock::now() < end) {
            const long got = t->read(buf, sizeof buf, 20);
            if (got < 0) break;
            if (got > 0) d.feed({buf, static_cast<size_t>(got)});
            while (auto f = d.next()) out.push_back(std::move(*f));
        }
        return out;
    }
};

struct TempDir {
    fs::path p;
    explicit TempDir(const char* tag)
        : p(fs::temp_directory_path() / (std::string("apex_v3_") + tag + "_" + std::to_string(Clock::now().time_since_epoch().count()))) {
        fs::create_directories(p);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
};

void wait_connected(const IpcServer& s) {
    for (int i = 0; i < 500 && !s.connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(s.connected());
}

void emit_solver_events(IpcServer& s) {
    s.event("phase", {{"name", "bnc"}});
    s.event("ub_improved", {{"ub", "2600"}, {"source", "lp_guided"}, {"heuristic", "true"}});
    s.event("cut_round", {{"round", "3"}, {"lp", "2500.5"}, {"rows", "300"}, {"comp", "1"}, {"sec", "4"},
                          {"blossom", "2"}, {"pool", "0"}, {"aged", "1"}, {"iters", "77"}, {"lp_s", "0.01"}, {"sep_s", "0.02"}});
    s.event("cut_round", {{"round", "4"}, {"lp", "2510"}, {"rows", "300"}, {"comp", "0"}, {"sec", "0"},
                          {"blossom", "0"}, {"pool", "0"}, {"aged", "0"}, {"iters", "5"}, {"lp_s", "0.01"}, {"sep_s", "0.02"}});
    s.event("pricing", {{"round", "1"}, {"global_lb", "2505.25"}});
    apex::TreeNodeInfo n;
    n.id = 0;
    n.state = apex::TreeNodeState::Branched;
    n.bound = 2510;
    s.tree_node(n);
    s.event("pricing", {{"round", "2"}, {"global_lb", "2570"}});  // tree started: node bound, not global
    s.lp_solution({{0, 1, 1.0}, {1, 2, 0.5}});
}

}  // namespace

TEST_CASE("T_v3_roundtrip_all_types") {
    CutRoundPacket c{};
    c.round = 9;
    c.lp_value = 123.25;
    c.sec = 4;
    TreeNodePacket tn{};
    tn.id = 17;
    tn.parent = 8;
    tn.bound = 99.5;
    EventHeader eh{5, 42.0, static_cast<uint8_t>(EventKind::UbImproved), 1};
    auto ev = bytes_of(eh);
    for (char ch : std::string("two_opt")) ev.push_back(static_cast<uint8_t>(ch));
    std::vector<uint8_t> lp = pack_u64(3);
    for (int i = 0; i < 4; ++i) lp.push_back(i == 0 ? 2 : 0);
    for (const LpEdgeWire w : {LpEdgeWire{0, 1, 1.0f}, LpEdgeWire{1, 2, 0.5f}}) {
        const auto b = bytes_of(w);
        lp.insert(lp.end(), b.begin(), b.end());
    }
    std::vector<uint8_t> cmd = pack_u64(11);
    for (char ch : std::string("heuristic two_opt")) cmd.push_back(static_cast<uint8_t>(ch));

    const std::vector<std::pair<MsgType, std::vector<uint8_t>>> frames = {
        {MsgType::CutRound, bytes_of(c)}, {MsgType::TreeNode, bytes_of(tn)}, {MsgType::Event, ev},
        {MsgType::LpSolution, lp}, {MsgType::Command, cmd}};
    FrameDecoder d;
    for (const auto& [t, p] : frames) {
        CHECK(first_version(t) == 3);
        CHECK(payload_size_ok(t, p.size()));
        const auto w = encode(t, p, true, 3);
        d.feed(w);
    }
    for (const auto& [t, p] : frames) {
        const auto f = d.next();
        REQUIRE(f);
        CHECK(f->type == t);
        CHECK(f->version == 3);
        CHECK(f->payload == p);
    }
    CHECK(read_as<CutRoundPacket>(bytes_of(c)).lp_value == 123.25);
    // Size checks reject truncated / misaligned payloads.
    CHECK(!payload_size_ok(MsgType::CutRound, sizeof(CutRoundPacket) - 1));
    CHECK(!payload_size_ok(MsgType::LpSolution, 12 + 5));
    CHECK(!payload_size_ok(MsgType::Event, sizeof(EventHeader) + 257));
    CHECK(first_version(MsgType::Telemetry) == 2);
    // Telemetry packet layout version stays 2 (v2 GUIs check it).
    CHECK((kTelemetryMagic & 0xFFFF) == 2);
}

TEST_CASE("T_v2_client_gets_only_v2") {
    for (const uint16_t peer : {uint16_t{2}, uint16_t{3}}) {
        CAPTURE(peer);
        TempDir dir("compat");
        ServerConfig cfg;
        cfg.endpoint = endpoint_for(dir.p, "compat");
        IpcServer s(cfg);
        REQUIRE(s.listening());
        RawClient c;
        REQUIRE(c.connect(cfg.endpoint, peer, 2));
        wait_connected(s);
        CHECK(s.peer_version() == peer);
        emit_solver_events(s);
        const auto frames = c.collect(1.5);
        std::map<MsgType, int> count;
        bool versions_ok = true;
        for (const auto& f : frames) {
            ++count[f.type];
            if (f.type != MsgType::Hello && f.version != peer) versions_ok = false;
        }
        CHECK(versions_ok);
        CHECK(count[MsgType::Hello] == 1);
        CHECK(count[MsgType::Log] >= 6);
        if (peer == 2) {
            for (const auto& f : frames) CHECK(first_version(f.type) <= 2);
        } else {
            CHECK(count[MsgType::CutRound] == 2);
            CHECK(count[MsgType::TreeNode] == 1);
            CHECK(count[MsgType::LpSolution] == 1);
            std::vector<std::pair<EventKind, std::string>> evs;
            double lb_seen = NAN;
            for (const auto& f : frames) {
                if (f.type != MsgType::Event) continue;
                const auto h = read_as<EventHeader>(f.payload);
                evs.push_back({static_cast<EventKind>(h.kind), std::string(f.payload.begin() + sizeof(EventHeader), f.payload.end())});
                if (static_cast<EventKind>(h.kind) == EventKind::LbImproved) lb_seen = h.value;
                if (static_cast<EventKind>(h.kind) == EventKind::UbImproved) CHECK(h.heuristic == 1);
            }
            auto has = [&](EventKind k, const std::string& d) {
                for (const auto& e : evs)
                    if (e.first == k && e.second == d) return true;
                return false;
            };
            CHECK(has(EventKind::Phase, "bnc"));
            CHECK(has(EventKind::UbImproved, "lp_guided"));
            CHECK(has(EventKind::NoViolatedCuts, ""));
            CHECK(has(EventKind::LbImproved, "lp"));
            CHECK(lb_seen == 2505.25);  // the post-root Farley bound (2570) is not a global LB
            for (const auto& f : frames)
                if (f.type == MsgType::CutRound && read_as<CutRoundPacket>(f.payload).round == 3) {
                    const auto cr = read_as<CutRoundPacket>(f.payload);
                    CHECK(cr.sec == 4);
                    CHECK(cr.blossom == 2);
                    CHECK(cr.component == 1);
                    CHECK(cr.lp_value == 2500.5);
                }
        }
    }
}

TEST_CASE("T_lp_solution_cap_and_coalesce") {
    // Rate limit: a v3 peer gets at most one LP per interval; no peer / a v2 peer gets none.
    TempDir dir("lp");
    ServerConfig cfg;
    cfg.endpoint = endpoint_for(dir.p, "lp");
    cfg.lp_interval_s = 0.3;
    IpcServer s(cfg);
    REQUIRE(s.listening());
    CHECK(!s.wants_lp_solution());  // nobody connected
    RawClient c;
    REQUIRE(c.connect(cfg.endpoint, 3, 2));
    wait_connected(s);
    CHECK(s.wants_lp_solution());
    CHECK(!s.wants_lp_solution());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(s.wants_lp_solution());

    // Coalescing: many LPs pushed back to back leave only the newest pending.
    OutQueue q;
    for (int k = 1; k <= 100; ++k) {
        Frame f;
        f.type = MsgType::LpSolution;
        f.payload = pack_u64(static_cast<uint64_t>(k));
        for (int i = 0; i < 4; ++i) f.payload.push_back(0);
        q.push(std::move(f));
    }
    const auto f = q.pop(std::chrono::milliseconds(10));
    REQUIRE(f);
    CHECK(unpack_u64(std::span(f->payload).first(8)) == 100u);
    CHECK(q.coalesced() == 99);
    CHECK(!q.pop(std::chrono::milliseconds(0)));

    // Cap: an LP support above kMaxLpEdges is truncated, never rejected by the GUI's size check.
    std::vector<apex::LpSupportEdge> big(kMaxLpEdges + 10, {0, 1, 0.5});
    s.lp_solution(big);
    std::vector<uint8_t> payload;
    for (const auto& fr : c.collect(1.0))
        if (fr.type == MsgType::LpSolution) payload = fr.payload;
    REQUIRE(!payload.empty());
    CHECK(read_as<uint32_t>(payload, 8) == kMaxLpEdges);
    CHECK(payload_size_ok(MsgType::LpSolution, payload.size()));
}

namespace {
struct RecordingSink final : apex::TelemetrySink {
    std::vector<apex::TreeNodeInfo> nodes;
    size_t lp_views = 0;
    bool support_ok = true;
    uint32_t n = 0;
    void event(std::string_view, std::initializer_list<apex::KV>) override {}
    bool wants_lp_solution() override { return true; }
    void lp_solution(const std::vector<apex::LpSupportEdge>& e) override {
        ++lp_views;
        double deg = 0;
        for (const auto& x : e) {
            if (x.u >= n || x.v >= n || !(x.x > 0) || x.x > 1 + 1e-6) support_ok = false;
            deg += x.x;
        }
        if (std::abs(deg - n) > 1e-4 * n) support_ok = false;  // degree-2 rows: sum x = n
    }
    void tree_node(const apex::TreeNodeInfo& i) override { nodes.push_back(i); }
};
}  // namespace

TEST_CASE("T_tree_events_match_bnc_result") {
    const auto inst = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/a280.tsp");
    for (const uint32_t lps : {1u, 3u}) {
        CAPTURE(lps);
        auto g = apex::build_candidate_graph(inst);
        auto t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
        apex::two_opt_oropt(t, inst, g, apex::TwoOptConfig{});
        apex::BnCConfig cfg;
        cfg.parallel_lps = lps;
        RecordingSink sink;
        sink.n = inst.n;
        const auto res = apex::run_bnc(inst, g, cfg, t, &sink);
        REQUIRE(res.status == apex::BnCResult::Status::Optimal);
        std::map<apex::TreeNodeState, uint64_t> by_state;
        std::map<uint32_t, apex::TreeNodeInfo> opened;
        std::set<uint32_t> concluded;
        bool parents_ok = true;
        for (const auto& nd : sink.nodes) {
            ++by_state[nd.state];
            if (nd.state == apex::TreeNodeState::Open) {
                opened[nd.id] = nd;
                if (!concluded.count(nd.parent)) parents_ok = false;  // children appear after their parent concludes
            } else {
                concluded.insert(nd.id);
                if (nd.id != 0 && !opened.count(nd.id)) parents_ok = false;
            }
        }
        CHECK(parents_ok);
        CHECK(by_state[apex::TreeNodeState::Branched] == res.branched);
        CHECK(by_state[apex::TreeNodeState::Pruned] == res.pruned);
        CHECK(by_state[apex::TreeNodeState::Infeasible] == res.infeasible);
        CHECK(by_state[apex::TreeNodeState::Integral] == res.integral);
        CHECK(by_state[apex::TreeNodeState::Open] == 2 * res.branched);
        CHECK(sink.nodes.front().id == 0);
        CHECK(sink.nodes.front().parent == UINT32_MAX);
        CHECK(sink.lp_views >= 1);
        CHECK(sink.support_ok);
        MESSAGE("a280 parallel_lps=" << lps << " tree events=" << sink.nodes.size() << " branched=" << res.branched
                                     << " pruned=" << res.pruned << " lp views=" << sink.lp_views);
    }
}
