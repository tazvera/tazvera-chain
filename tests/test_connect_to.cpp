// connect_to(address, port): connect to a named IPv4 address instead of 127.0.0.1, with a time limit.
#include "taz/net.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)
using clk = std::chrono::steady_clock;

int main() {
    std::string e; if (!net_startup(&e)) return EXIT_FAILURE;
    // 1. it connects to a listener on 127.0.0.1, and bytes really flow
    {
        Listener l; CHECK(listen_loopback(0, l, &e));
        Socket c; std::string err;
        CHECK(connect_to("127.0.0.1", l.port, c, &err, std::chrono::milliseconds(3000)) && c.valid());
        Socket s; CHECK(accept_one(l.socket, s, &err, clk::now() + std::chrono::seconds(3)));
        CHECK(send_all(c, "hi", &err));
        std::string got; CHECK(recv_exact(s, 2, got, &err) && got == "hi");
    }
    // 2. an address that is not a dotted quad is refused before any socket is made, and the message names it
    for (const char* bad : {"", "localhost", "1.2.3", "1.2.3.4.5", "256.0.0.1", "01.2.3.4", "::1", "1.2.3.4:80", " 127.0.0.1", "127.1"}) {
        Listener l; CHECK(listen_loopback(0, l, &e));
        Socket c; std::string err;
        CHECK(!connect_to(bad, l.port, c, &err) && !c.valid());
        CHECK(!err.empty());
        Socket s; std::string e2;
        CHECK(!accept_one(l.socket, s, &e2, clk::now() + std::chrono::milliseconds(50)));   // nothing reached the listener
    }
    // 3. nobody listening: a clear failure that names address and port, and it is quick
    {
        Listener l; CHECK(listen_loopback(0, l, &e)); const std::uint16_t port = l.port; l.socket.close();
        Socket c; std::string err; const auto t0 = clk::now();
        CHECK(!connect_to("127.0.0.1", port, c, &err) && !c.valid());
        CHECK(err.find("127.0.0.1:" + std::to_string(port)) != std::string::npos);
        CHECK(clk::now() - t0 < std::chrono::seconds(5));
    }
    // 4. an address that never answers (TEST-NET-3, reserved for documentation) ends at the time limit, not at the OS limit
    {
        Socket c; std::string err; const auto t0 = clk::now();
        CHECK(!connect_to("203.0.113.77", 9, c, &err, std::chrono::milliseconds(1500)) && !c.valid());
        CHECK(clk::now() - t0 < std::chrono::seconds(6));
        CHECK(err.find("203.0.113.77:9") != std::string::npos);
    }
    // 5. the old function is unchanged
    {
        Listener l; CHECK(listen_loopback(0, l, &e)); const std::uint16_t port = l.port; l.socket.close();
        Socket c; std::string err;
        CHECK(!connect_loopback(port, c, &err) && err == "could not connect to 127.0.0.1:" + std::to_string(port));
        Listener m; CHECK(listen_loopback(0, m, &e));
        CHECK(connect_loopback(m.port, c, &err) && c.valid());
    }
    net_cleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "connect-to tests: all passed (5 cases)\n";
    return EXIT_SUCCESS;
}
