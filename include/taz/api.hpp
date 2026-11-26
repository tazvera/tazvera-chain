#pragma once
#include "taz/chain.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace taz {

// Read-only HTTP API. The server binds only to 127.0.0.1 and serves one request per connection.
class ApiServer {
public:
    ApiServer();
    ~ApiServer();
    ApiServer(ApiServer&&) noexcept;
    ApiServer& operator=(ApiServer&&) noexcept;
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    bool start(const Chain& chain, std::uint16_t port, std::string* error = nullptr);
    bool start(const Chain& chain, std::uint16_t port,
               const std::vector<std::string>& allowed_origins, std::string* error = nullptr);
    void wait();
    void stop();
    bool running() const noexcept;
    std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taz
