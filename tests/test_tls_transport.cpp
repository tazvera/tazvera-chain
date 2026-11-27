#include "taz/tls_transport.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static_assert(std::is_base_of_v<Transport, TlsTransport>);
static_assert(!std::is_copy_constructible_v<TlsTransport>);
static_assert(!std::is_copy_assignable_v<TlsTransport>);

static void t01_transport_interface_is_complete_without_a_handshake() {
    Socket socket;
    TlsTransport transport(socket, nullptr);
    std::string error = "stale";
    std::string received = "stale";

    CHECK(!transport.send_all("hello", &error, no_deadline()));
    CHECK(error == "TLS connection is not established");

    error = "stale";
    CHECK(!transport.recv_exact(5, received, &error, no_deadline()));
    CHECK(error == "TLS connection is not established");
    CHECK(received.empty());

    // prepare_send is intentionally inherited from Transport: no handshake or socket work.
    error.clear();
    CHECK(transport.prepare_send(no_deadline(), &error));
    CHECK(error.empty());

    // Must be safe when no TLS session/socket has been established.
    transport.shutdown();
}

int main() {
    t01_transport_interface_is_complete_without_a_handshake();
    if (failures == 0) {
        std::cout << "TLS transport tests: all passed (1 case)\n";
        return 0;
    }
    std::cerr << "TLS transport tests: " << failures << " failure(s)\n";
    return 1;
}
