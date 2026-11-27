// listen_on(): the listening address is a parameter. listen_loopback() is listen_on("127.0.0.1", ...) and behaves as before.
#include "taz/net.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

// t1: which texts are an IPv4 address (strictly)
static void t1_address_syntax() {
    for (const char* ok : {"127.0.0.1", "0.0.0.0", "10.1.2.3", "192.168.0.255", "255.255.255.255", "1.2.3.4", "100.64.0.1"})
        CHECK(is_ipv4_address(ok));
    for (const char* bad : {"", " ", "localhost", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256", "01.2.3.4", "1.02.3.4", "1.2.3.04", "00.0.0.0",
                            "1.2.3.-4", "+1.2.3.4", "1.2.3.4 ", " 1.2.3.4", "1.2.3.4\n", "1.2.3.4:80", "1.2.3.4/8", "::1", "::ffff:1.2.3.4", "a.b.c.d",
                            "1..3.4", ".1.2.3", "1.2.3.", "0x7f.0.0.1", "127.1", "2130706433", "999999999999.1.1.1", "1.2.3.4a", "1,2,3,4", "١.٢.٣.٤"})
        CHECK(!is_ipv4_address(bad));
}

// t2: listening on loopback with an explicit address is the same as listen_loopback: a client reaches it, the port is reported
static void t2_listen_on_loopback() {
    std::string err; Listener l;
    CHECK(listen_on("127.0.0.1", 0, l, &err));
    CHECK(l.socket.valid() && l.port != 0);
    if (!l.socket.valid()) return;
    Socket client, server;
    CHECK(connect_loopback(l.port, client, &err));
    CHECK(accept_one(l.socket, server, &err));
    CHECK(client.valid() && server.valid());
    Listener old; CHECK(listen_loopback(0, old, &err) && old.port != 0 && old.port != l.port);
}

// t3: 0.0.0.0 listens on every interface of this machine; loopback reaches it
static void t3_listen_on_every_interface() {
    std::string err; Listener l;
    CHECK(listen_on("0.0.0.0", 0, l, &err));
    CHECK(l.socket.valid() && l.port != 0);
    if (!l.socket.valid()) return;
    Socket client, server;
    CHECK(connect_loopback(l.port, client, &err));
    CHECK(accept_one(l.socket, server, &err, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

// t4: a bad or foreign address is an error and says which address; nothing is left listening
static void t4_bad_addresses() {
    for (const char* bad : {"", "localhost", "1.2.3", "256.0.0.1", "01.2.3.4", "::1"}) {
        Listener l; std::string err;
        CHECK(!listen_on(bad, 0, l, &err));
        CHECK(!l.socket.valid());
        CHECK(err.find("not a valid IPv4 address") != std::string::npos);
    }
    // 203.0.113.0/24 is documentation space (TEST-NET-3): no interface of this machine has it, so binding it fails
    Listener l; std::string err;
    CHECK(!listen_on("203.0.113.77", 0, l, &err));
    CHECK(!l.socket.valid());
    CHECK(err.find("203.0.113.77") != std::string::npos);
}

// t5: a port that is taken is refused, on the explicit address as on loopback
static void t5_port_in_use() {
    std::string err; Listener first;
    CHECK(listen_on("127.0.0.1", 0, first, &err));
    Listener second; std::string why;
    CHECK(!listen_on("127.0.0.1", first.port, second, &why));
    CHECK(why.find("127.0.0.1:" + std::to_string(first.port)) != std::string::npos);
}

int main() {
    std::string e;
    if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return EXIT_FAILURE; }
    t1_address_syntax();
    t2_listen_on_loopback();
    t3_listen_on_every_interface();
    t4_bad_addresses();
    t5_port_in_use();
    net_cleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "listen-on tests: all passed (5 cases)\n";
    return EXIT_SUCCESS;
}
