#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace apex::ipc {

// Byte stream between engine and GUI: Unix domain socket (Linux) or named pipe (Windows), one
// interface. Endpoint: a socket path (Linux) or a pipe name "\\.\pipe\<name>" (Windows; a bare
// name gets the prefix).
class Transport {
public:
    virtual ~Transport() = default;
    // > 0 bytes read, 0 timeout, -1 closed / error.
    virtual long read(uint8_t* buf, size_t cap, int timeout_ms) = 0;
    virtual bool write_all(const uint8_t* data, size_t n) = 0;
};

class Listener {
public:
    virtual ~Listener() = default;
    // One client; nullptr on timeout.
    virtual std::unique_ptr<Transport> accept(int timeout_ms) = 0;
};

std::unique_ptr<Listener> listen(const std::string& endpoint);   // nullptr on failure
std::unique_ptr<Transport> connect(const std::string& endpoint, int timeout_ms);  // client side

}  // namespace apex::ipc
