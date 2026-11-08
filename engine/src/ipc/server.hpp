#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/telemetry.hpp"
#include "graph/tours.hpp"
#include "ipc/protocol.hpp"
#include "ipc/transport.hpp"

namespace apex::ipc {

struct ServerConfig {
    std::string endpoint;            // --ipc: socket path / pipe name
    std::string shared_tour_path;    // --shared-tour: mmap file for SharedTour (empty: no tour channel)
    uint32_t tour_capacity = 0;      // n
    Hello hello;                     // engine side of the handshake (mode, mem limit, threads)
    TelemetrySink* forward = nullptr;  // events are also passed here (stdout log)
    std::function<bool()> on_checkpoint;  // CHECKPOINT: runs on the server thread after the ACK (true = written)
    double tour_interval_s = 0.5;    // SharedTour rewrite cadence (publish_tour without force)
    double lp_interval_s = 0.5;      // LP_SOLUTION cadence (v3 peers)
    // COMMAND (v3): runs on the server thread after the ACK; returns the LOG line to send back.
    std::function<std::string(std::string_view)> on_command;
};

// Engine IPC server (M5.2): one client at a time, handshake first, then control in and
// telemetry / tour / cluster map / log / finished out through the OutQueue. Also the engine's
// TelemetrySink: events become LOG frames and update the telemetry state; TELEMETRY goes out at
// 1 Hz and immediately when UB or LB improves. No call blocks the solver on the GUI.
class IpcServer final : public TelemetrySink {
public:
    explicit IpcServer(ServerConfig cfg);
    ~IpcServer() override;
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool listening() const { return listener_ != nullptr; }
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t dropped() const { return queue_.dropped(); }
    uint64_t shutdown_requested() const { return shutdown_.load(std::memory_order_acquire); }

    void event(std::string_view name, std::initializer_list<KV> fields) override;
    bool wants_lp_solution() override;
    void lp_solution(const std::vector<LpSupportEdge>& edges) override;
    void tree_node(const TreeNodeInfo& n) override;
    uint16_t peer_version() const { return version_.load(std::memory_order_acquire); }
    // Rewrites the SharedTour (at most every tour_interval_s unless force) and queues TOUR_UPDATE.
    void publish_tour(const Tour& t, bool force = false);
    // Cluster map (scalable mode): state 0 idle, 1 solving, 2 patching; sent at most once per second.
    void cluster_state(uint32_t id, uint8_t state);
    // Queues FINISHED (the report JSON, byte for byte) and waits until it is written to a client,
    // at most wait_ms (also waiting for a client to connect).
    bool finish(const std::string& report_json, int wait_ms = 5000);

private:
    void run();
    void serve(Transport& t);
    bool handshake(Transport& t, FrameDecoder& dec);
    void handle(Transport& t, const Frame& f);
    void push_telemetry_locked(bool force);
    void push_event(uint8_t kind, double value, bool heuristic, std::string_view detail);
    uint64_t now_us() const;
    std::vector<uint8_t> enc(MsgType t, std::span<const uint8_t> payload) const;
    void flush_clusters(bool force);

    ServerConfig cfg_;
    std::unique_ptr<Listener> listener_;
    std::optional<MappedFile> tour_file_;
    SharedTourView tour_;
    OutQueue queue_;
    std::thread th_;
    std::atomic<bool> stop_{false}, connected_{false}, finished_sent_{false};
    std::atomic<uint64_t> shutdown_{0};
    std::atomic<uint16_t> version_{0};   // negotiated with the connected client (0: none)
    std::atomic<int64_t> last_lp_us_{-1000000000};
    uint64_t lp_seq_ = 0;
    bool proven_sent_ = false;
    std::atomic<bool> tree_started_{false};

    std::mutex state_mu_;            // telemetry state + throttle
    TelemetryPacket pkt_{};
    TelemetryThrottle throttle_;
    std::chrono::steady_clock::time_point t0_;

    std::mutex tour_mu_;
    std::chrono::steady_clock::time_point last_tour_{};

    std::mutex cluster_mu_;
    std::vector<uint8_t> clusters_;
    bool clusters_dirty_ = false;
    std::chrono::steady_clock::time_point last_clusters_{};
};

}  // namespace apex::ipc
