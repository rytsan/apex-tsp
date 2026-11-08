#include "ipc/transport.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace apex::ipc {

#ifdef _WIN32

namespace {

std::string pipe_name(const std::string& e) { return e.rfind("\\\\.\\pipe\\", 0) == 0 ? e : "\\\\.\\pipe\\" + e; }

// ponytail: polling (1 ms sleeps) instead of overlapped I/O; switch to OVERLAPPED if GUI latency matters.
class PipeTransport final : public Transport {
public:
    PipeTransport(HANDLE h, bool server) : h_(h), server_(server) {}
    ~PipeTransport() override {
        if (server_) DisconnectNamedPipe(h_);
        CloseHandle(h_);
    }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
            if (avail > 0) {
                DWORD got = 0;
                const DWORD want = static_cast<DWORD>(cap < avail ? cap : avail);
                if (!ReadFile(h_, buf, want, &got, nullptr)) return -1;
                return static_cast<long>(got);
            }
            if (std::chrono::steady_clock::now() >= end) return 0;
            Sleep(1);
        }
    }
    bool write_all(const uint8_t* data, size_t n) override {
        while (n > 0) {
            DWORD put = 0;
            const DWORD chunk = static_cast<DWORD>(n > (1u << 20) ? (1u << 20) : n);
            if (!WriteFile(h_, data, chunk, &put, nullptr)) return false;
            if (put == 0) { Sleep(1); continue; }  // PIPE_NOWAIT server: buffer full
            data += put;
            n -= put;
        }
        return true;
    }

private:
    HANDLE h_;
    bool server_;
};

class PipeListener final : public Listener {
public:
    explicit PipeListener(std::string name) : name_(std::move(name)) {}
    std::unique_ptr<Transport> accept(int timeout_ms) override {
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1,
                                  1 << 20, 1 << 20, 0, nullptr);
            if (h_ == INVALID_HANDLE_VALUE) return nullptr;
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (!ConnectNamedPipe(h_, nullptr)) {
                const DWORD e = GetLastError();
                if (e == ERROR_PIPE_CONNECTED) {
                    HANDLE h = h_;
                    h_ = INVALID_HANDLE_VALUE;  // next accept creates a new instance
                    return std::make_unique<PipeTransport>(h, true);
                }
                if (e == ERROR_NO_DATA) DisconnectNamedPipe(h_);  // client came and went
                else if (e != ERROR_PIPE_LISTENING) return nullptr;
            }
            if (std::chrono::steady_clock::now() >= end) return nullptr;
            Sleep(1);
        }
    }
    ~PipeListener() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

private:
    std::string name_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace

std::unique_ptr<Listener> listen(const std::string& endpoint) { return std::make_unique<PipeListener>(pipe_name(endpoint)); }

std::unique_ptr<Transport> connect(const std::string& endpoint, int timeout_ms) {
    const std::string name = pipe_name(endpoint);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return std::make_unique<PipeTransport>(h, false);
        if (std::chrono::steady_clock::now() >= end) return nullptr;
        Sleep(5);
    }
}

#else

namespace {

class SocketTransport final : public Transport {
public:
    explicit SocketTransport(int fd) : fd_(fd) {}
    ~SocketTransport() override { ::close(fd_); }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        const ssize_t got = ::recv(fd_, buf, cap, 0);
        if (got > 0) return static_cast<long>(got);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
        return -1;  // 0 = peer closed
    }
    bool write_all(const uint8_t* data, size_t n) override {
        while (n > 0) {
            const ssize_t put = ::send(fd_, data, n, MSG_NOSIGNAL);
            if (put < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            data += put;
            n -= static_cast<size_t>(put);
        }
        return true;
    }

private:
    int fd_;
};

bool make_addr(const std::string& path, sockaddr_un& a) {
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return false;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return true;
}

class SocketListener final : public Listener {
public:
    SocketListener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~SocketListener() override {
        ::close(fd_);
        ::unlink(path_.c_str());
    }
    std::unique_ptr<Transport> accept(int timeout_ms) override {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return nullptr;
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return nullptr;
        return std::make_unique<SocketTransport>(c);
    }

private:
    int fd_;
    std::string path_;
};

}  // namespace

std::unique_ptr<Listener> listen(const std::string& endpoint) {
    sockaddr_un a;
    if (!make_addr(endpoint, a)) return nullptr;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    ::unlink(endpoint.c_str());  // stale socket from a crashed run
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 1) != 0) {
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<SocketListener>(fd, endpoint);
}

std::unique_ptr<Transport> connect(const std::string& endpoint, int timeout_ms) {
    sockaddr_un a;
    if (!make_addr(endpoint, a)) return nullptr;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return std::make_unique<SocketTransport>(fd);
        ::close(fd);
        if (std::chrono::steady_clock::now() >= end) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

#endif

}  // namespace apex::ipc
