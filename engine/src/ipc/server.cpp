#include "ipc/server.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

#include "app/mem.hpp"
#include "core/control.hpp"

namespace apex::ipc {

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool parse_double(std::string_view s, double& out) {
    if (s.empty() || s == "null") return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{};
}

// Phase ids of the TELEMETRY packet (event "phase" from the app).
uint32_t phase_id(std::string_view name) {
    static constexpr std::string_view names[] = {"load", "candidate_graph", "construct", "two_opt",
                                                 "held_karp", "polish", "bnc", "done", "lab"};
    for (uint32_t k = 0; k < std::size(names); ++k)
        if (names[k] == name) return k;
    return UINT32_MAX;
}

Frame make(MsgType t, std::vector<uint8_t> payload) {
    Frame f;
    f.type = t;
    f.payload = std::move(payload);
    return f;
}

std::vector<uint8_t> text(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace

IpcServer::IpcServer(ServerConfig cfg) : cfg_(std::move(cfg)), t0_(Clock::now()) {
    pkt_.magic_version = kTelemetryMagic;
    pkt_.mode = cfg_.hello.mode;
    pkt_.lb_source = cfg_.hello.mode == 0 ? 0 : 1;
    pkt_.upper_bound = pkt_.lower_bound = pkt_.gap_pct = kNaN;
    pkt_.threads_lp = cfg_.hello.threads;
    if (!cfg_.shared_tour_path.empty() && cfg_.tour_capacity > 0) {
        tour_file_ = MappedFile::create(cfg_.shared_tour_path, shared_tour_bytes(cfg_.tour_capacity));
        if (tour_file_) {
            tour_ = SharedTourView(tour_file_->data());
            tour_.init(cfg_.tour_capacity);
            pkt_.cold_bytes = tour_file_->size();
        }
    }
    listener_ = listen(cfg_.endpoint);
    if (listener_) th_ = std::thread([this] { run(); });
}

IpcServer::~IpcServer() {
    stop_.store(true, std::memory_order_release);
    queue_.close();
    if (th_.joinable()) th_.join();
}

// ---- engine side --------------------------------------------------------------------------

uint64_t IpcServer::now_us() const {
    return static_cast<uint64_t>(std::chrono::duration<double, std::micro>(Clock::now() - t0_).count());
}

std::vector<uint8_t> IpcServer::enc(MsgType t, std::span<const uint8_t> payload) const {
    const uint16_t v = version_.load(std::memory_order_acquire);
    return encode(t, payload, false, v ? v : kProtocolMin);
}

template <class T>
static std::vector<uint8_t> bytes_of(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    return {p, p + sizeof(T)};
}

void IpcServer::push_event(uint8_t kind, double value, bool heuristic, std::string_view detail) {
    EventHeader h{now_us(), value, kind, static_cast<uint8_t>(heuristic ? 1 : 0)};
    auto p = bytes_of(h);
    p.insert(p.end(), detail.begin(), detail.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(detail.size(), 256)));
    queue_.push(make(MsgType::Event, std::move(p)));
}

bool IpcServer::wants_lp_solution() {
    if (peer_version() < 3) return false;
    const auto now = static_cast<int64_t>(now_us());
    auto last = last_lp_us_.load(std::memory_order_relaxed);
    if (now - last < static_cast<int64_t>(cfg_.lp_interval_s * 1e6)) return false;
    return last_lp_us_.compare_exchange_strong(last, now, std::memory_order_relaxed);
}

void IpcServer::lp_solution(const std::vector<LpSupportEdge>& edges) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(edges.size(), kMaxLpEdges));
    std::vector<uint8_t> p = pack_u64(++lp_seq_);
    p.reserve(12 + size_t{n} * sizeof(LpEdgeWire));
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(n >> (8 * i)));
    for (uint32_t k = 0; k < n; ++k) {
        const LpEdgeWire w{edges[k].u, edges[k].v, static_cast<float>(edges[k].x)};
        const auto* b = reinterpret_cast<const uint8_t*>(&w);
        p.insert(p.end(), b, b + sizeof w);
    }
    queue_.push(make(MsgType::LpSolution, std::move(p)));
}

void IpcServer::tree_node(const TreeNodeInfo& n) {
    tree_started_.store(true, std::memory_order_release);
    TreeNodePacket t{};
    t.timestamp_us = now_us();
    t.id = n.id;
    t.parent = n.parent;
    t.depth = n.depth;
    t.state = static_cast<uint8_t>(n.state);
    t.branch_dir = n.branch_dir;
    t.branch_u = n.branch_u;
    t.branch_v = n.branch_v;
    t.bound = n.bound;
    t.lp_value = n.lp_value;
    t.cuts_sec = static_cast<uint32_t>(n.cuts_sec);
    t.cuts_blossom = static_cast<uint32_t>(n.cuts_blossom);
    t.cuts_component = static_cast<uint32_t>(n.cuts_component);
    queue_.push(make(MsgType::TreeNode, bytes_of(t)));
}

void IpcServer::event(std::string_view name, std::initializer_list<KV> fields) {
    if (cfg_.forward) cfg_.forward->event(name, fields);
    auto field = [&](std::string_view key) -> std::string_view {
        for (const auto& [k, v] : fields)
            if (k == key) return v;
        return {};
    };
    auto num = [&](std::string_view key) {
        double x = 0;
        return parse_double(field(key), x) ? x : 0.0;
    };
    if (name == "cut_round") {
        CutRoundPacket c{};
        c.timestamp_us = now_us();
        c.round = static_cast<uint32_t>(num("round"));
        c.rows = static_cast<uint32_t>(num("rows"));
        c.lp_value = num("lp");
        c.sec = static_cast<uint32_t>(num("sec"));
        c.blossom = static_cast<uint32_t>(num("blossom"));
        c.component = static_cast<uint32_t>(num("comp"));
        c.pool = static_cast<uint32_t>(num("pool"));
        c.aged = static_cast<uint32_t>(num("aged"));
        c.lp_iterations = static_cast<uint32_t>(num("iters"));
        c.lp_s = static_cast<float>(num("lp_s"));
        c.sep_s = static_cast<float>(num("sep_s"));
        queue_.push(make(MsgType::CutRound, bytes_of(c)));
        if (c.sec + c.blossom + c.component + c.pool == 0)
            push_event(static_cast<uint8_t>(EventKind::NoViolatedCuts), c.lp_value, false, "");
    } else if (name == "phase") {
        push_event(static_cast<uint8_t>(EventKind::Phase), 0, false, field("name"));
    } else if (name == "root_done" && field("tailing_off") == "true") {
        push_event(static_cast<uint8_t>(EventKind::TailingOff), num("lp_value"), false, "");
    } else if (name == "rcf_root") {
        push_event(static_cast<uint8_t>(EventKind::Rcf), num("pruned_edges_rcf"), false, "");
    }
    std::string line(name);
    for (const auto& [k, v] : fields) {
        line += ' ';
        line += k;
        line += '=';
        line += LogSink::escape(v);
    }
    if (line.size() > 65536) line.resize(65536);
    queue_.push(make(MsgType::Log, text(line)));

    std::lock_guard<std::mutex> lk(state_mu_);
    const double ub0 = pkt_.upper_bound, lb0 = pkt_.lower_bound;
    for (const auto& [k, v] : fields) {
        double x;
        if (k == "ub" && parse_double(v, x) && !(x >= pkt_.upper_bound)) pkt_.upper_bound = x;  // NaN-safe min
        // lb / lb_int: Lagrangian, tree or final bound; global_lb: Farley bound of the root loop,
        // globally valid at every root pricing round (MASTER §5.5) but only valid for its subtree
        // once the loop runs tree nodes, so accepted only before the first tree node. Max-only.
        else if ((k == "lb" || k == "lb_int" || (k == "global_lb" && !tree_started_.load(std::memory_order_acquire))) &&
                 parse_double(v, x) && !(x <= pkt_.lower_bound))
            pkt_.lower_bound = x;
        else if (k == "open" && parse_double(v, x)) pkt_.open_nodes = static_cast<uint32_t>(x);
        else if (k == "edges" && parse_double(v, x)) pkt_.active_edges = static_cast<uint32_t>(x);
        else if (k == "pruned_edges_rcf" && parse_double(v, x)) pkt_.pruned_edges_rcf = static_cast<uint32_t>(x);
        else if (k == "pool_cuts" && parse_double(v, x)) pkt_.cut_pool_size = static_cast<uint32_t>(x);
        else if (k == "iters" && parse_double(v, x)) pkt_.simplex_iterations += static_cast<uint64_t>(x);
        else if (name == "phase" && k == "name") pkt_.phase = phase_id(v);
    }
    const bool lagr = name == "held_karp" || field("lb_source") == "lagrangian";
    if (pkt_.upper_bound != ub0 && !std::isnan(pkt_.upper_bound)) {
        const auto src = field("source");
        push_event(static_cast<uint8_t>(EventKind::UbImproved), pkt_.upper_bound, field("heuristic") != "false",
                   src.empty() ? name : src);
    }
    if (pkt_.lower_bound != lb0 && !std::isnan(pkt_.lower_bound)) {
        if (lagr) pkt_.lb_source = 1;
        push_event(static_cast<uint8_t>(EventKind::LbImproved), pkt_.lower_bound, false, lagr ? "lagrangian" : "lp");
    }
    if (!proven_sent_ && (field("status") == "OPTIMAL_PROVEN" ||
                          (!std::isnan(pkt_.lower_bound) && std::ceil(pkt_.lower_bound - 1e-6) >= pkt_.upper_bound))) {
        proven_sent_ = true;
        push_event(static_cast<uint8_t>(EventKind::Proven), pkt_.upper_bound, false, "");
    }
    push_telemetry_locked(false);
}

void IpcServer::push_telemetry_locked(bool force) {
    const auto now = Clock::now();
    const double ub = std::isnan(pkt_.upper_bound) ? INFINITY : pkt_.upper_bound;
    const double lb = std::isnan(pkt_.lower_bound) ? -INFINITY : pkt_.lower_bound;
    if (!throttle_.should_emit(now, ub, lb) && !force) return;
    const double secs = std::chrono::duration<double>(now - t0_).count();
    ++pkt_.seq;
    pkt_.timestamp_us = static_cast<uint64_t>(secs * 1e6);
    pkt_.gap_pct = std::isnan(pkt_.lower_bound) || std::isnan(pkt_.upper_bound) || pkt_.upper_bound == 0
                       ? kNaN
                       : 100.0 * (pkt_.upper_bound - pkt_.lower_bound) / pkt_.upper_bound;
    pkt_.iter_per_sec = secs > 0 ? static_cast<double>(pkt_.simplex_iterations) / secs : 0;
    pkt_.ram_bytes = current_rss_bytes();
    queue_.push(make(MsgType::Telemetry, pack(pkt_)));
}

void IpcServer::publish_tour(const Tour& t, bool force) {
    if (!tour_file_) return;
    std::lock_guard<std::mutex> lk(tour_mu_);  // single writer of the seqlock
    const auto now = Clock::now();
    if (!force && std::chrono::duration<double>(now - last_tour_).count() < cfg_.tour_interval_s) return;
    last_tour_ = now;
    const uint64_t epoch = tour_.write(t.order);
    std::vector<uint8_t> p = pack_u64(epoch);
    const auto count = static_cast<uint32_t>(std::min<size_t>(t.order.size(), cfg_.tour_capacity));
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(count >> (8 * i)));
    queue_.push(make(MsgType::TourUpdate, std::move(p)));
}

void IpcServer::cluster_state(uint32_t id, uint8_t state) {
    {
        std::lock_guard<std::mutex> lk(cluster_mu_);
        if (id >= clusters_.size()) clusters_.resize(id + 1, 0);
        clusters_[id] = state;
        clusters_dirty_ = true;
    }
    flush_clusters(false);
}

void IpcServer::flush_clusters(bool force) {
    std::vector<uint8_t> p;
    {
        std::lock_guard<std::mutex> lk(cluster_mu_);
        const auto now = Clock::now();
        if (!clusters_dirty_ || (!force && now - last_clusters_ < std::chrono::seconds(1))) return;
        last_clusters_ = now;
        clusters_dirty_ = false;
        const auto n = static_cast<uint32_t>(clusters_.size());
        p.reserve(4 + 5 * size_t{n});
        for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(n >> (8 * i)));
        for (uint32_t id = 0; id < n; ++id) {
            for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(id >> (8 * i)));
            p.push_back(clusters_[id]);
        }
    }
    queue_.push(make(MsgType::ClusterMap, std::move(p)));
}

bool IpcServer::finish(const std::string& report_json, int wait_ms) {
    if (!listener_) return false;
    flush_clusters(true);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        pkt_.phase = phase_id("done");
        push_telemetry_locked(true);
    }
    queue_.push(make(MsgType::Finished, text(report_json)));
    const auto end = Clock::now() + std::chrono::milliseconds(wait_ms);
    while (!finished_sent_.load(std::memory_order_acquire) && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return finished_sent_.load(std::memory_order_acquire);
}

// ---- server thread ------------------------------------------------------------------------

void IpcServer::run() {
    while (!stop_.load(std::memory_order_acquire)) {
        auto t = listener_->accept(100);
        if (!t) continue;
        FrameDecoder dec;
        if (handshake(*t, dec)) {
            connected_.store(true, std::memory_order_release);
            serve(*t);
            connected_.store(false, std::memory_order_release);
            version_.store(0, std::memory_order_release);
        }
    }
}

bool IpcServer::handshake(Transport& t, FrameDecoder& dec) {
    // Hello carries the lowest version in its header, so any supported peer decodes it.
    const auto hello = encode(MsgType::Hello, pack(cfg_.hello), false, kProtocolMin);
    if (!t.write_all(hello.data(), hello.size())) return false;
    uint8_t buf[4096];
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < end && !stop_.load(std::memory_order_acquire)) {
        const long got = t.read(buf, sizeof buf, 50);
        if (got < 0) return false;
        if (got > 0) dec.feed({buf, static_cast<size_t>(got)});
        while (auto f = dec.next()) {
            if (f->type != MsgType::Hello) continue;  // nothing else before the handshake
            const auto peer = unpack_hello(f->payload);
            const uint16_t v = peer ? negotiate(cfg_.hello, *peer) : 0;
            if (v == 0) {
                std::string why = "version mismatch: engine " + std::to_string(cfg_.hello.protocol_min) + ".." +
                                  std::to_string(cfg_.hello.version);
                if (peer) why += ", gui " + std::to_string(peer->protocol_min) + ".." + std::to_string(peer->version);
                const auto c = encode(MsgType::Close, text(why), false, kProtocolMin);
                t.write_all(c.data(), c.size());
                return false;
            }
            version_.store(v, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void IpcServer::serve(Transport& t) {
    uint8_t buf[4096];
    FrameDecoder dec;
    for (;;) {
        // Outgoing first: everything queued, without waiting.
        while (auto f = queue_.pop(std::chrono::milliseconds(0))) {
            const uint16_t v = version_.load(std::memory_order_acquire);
            if (first_version(f->type) > v) continue;  // the peer does not know this type
            f->version = v;
            const auto w = encode(*f);
            if (!t.write_all(w.data(), w.size())) return;
            if (f->type == MsgType::Finished) finished_sent_.store(true, std::memory_order_release);
        }
        if (stop_.load(std::memory_order_acquire)) return;
        const long got = t.read(buf, sizeof buf, 20);
        if (got < 0) return;  // client gone: back to accept
        if (got > 0) dec.feed({buf, static_cast<size_t>(got)});
        while (auto f = dec.next()) handle(t, *f);
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            push_telemetry_locked(false);  // 1 Hz heartbeat
        }
        flush_clusters(false);
    }
}

void IpcServer::handle(Transport& t, const Frame& f) {
    if (f.type == MsgType::Command && f.payload.size() >= 8) {
        const auto seq = unpack_u64(std::span(f.payload).first(8));
        const auto ack = enc(MsgType::Ack, pack_u64(*seq));
        t.write_all(ack.data(), ack.size());
        const std::string_view cmd(reinterpret_cast<const char*>(f.payload.data()) + 8, f.payload.size() - 8);
        const std::string reply = cfg_.on_command ? cfg_.on_command(cmd) : "command_unsupported " + std::string(cmd);
        const auto lg = enc(MsgType::Log, text(reply));
        t.write_all(lg.data(), lg.size());
        return;
    }
    if (!is_control(f.type)) return;  // otherwise the GUI sends only control (and the handshake)
    const auto seq = unpack_u64(f.payload);
    if (!seq) return;
    auto& c = engine_control();
    switch (f.type) {
        case MsgType::Pause: c.pause(); break;
        case MsgType::Resume: c.resume(); break;
        case MsgType::Cancel: c.cancel(); break;
        case MsgType::Checkpoint: c.request_checkpoint(); break;
        case MsgType::Shutdown: shutdown_.store(1, std::memory_order_release); c.cancel(); break;
        default: break;
    }
    const auto ack = enc(MsgType::Ack, pack_u64(*seq));
    t.write_all(ack.data(), ack.size());
    if (f.type == MsgType::Pause) {
        // Measured in the engine: time until a solver thread sleeps in pause_point (<= 1 s wait).
        const auto t0 = Clock::now();
        while (c.paused() && c.waiting() == 0 && Clock::now() - t0 < std::chrono::seconds(1))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        char b[96];
        std::snprintf(b, sizeof b, "paused stalled_threads=%u after_ms=%.1f", c.waiting(), ms);
        const auto lg = enc(MsgType::Log, text(b));
        t.write_all(lg.data(), lg.size());
    } else if (f.type == MsgType::Checkpoint) {
        const bool ok = cfg_.on_checkpoint && cfg_.on_checkpoint();
        const auto lg = enc(MsgType::Log, text(ok ? "checkpoint_written" : "checkpoint_skipped no_checkpoint_dir"));
        t.write_all(lg.data(), lg.size());
    }
}

}  // namespace apex::ipc
