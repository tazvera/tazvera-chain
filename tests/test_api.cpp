#include "taz/api.hpp"
#include "taz/crypto.hpp"
#include "taz/net.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address address{}; };
static Key make_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) std::exit(2);
    k.address = address_from_pubkey(k.pk);
    return k;
}
static std::string request(std::uint16_t port, const std::string& bytes) {
    Socket socket;
    std::string error;
    if (!connect_loopback(port, socket, &error)) return {};
    std::string wire = bytes;
    if (wire.find("Host:") == std::string::npos) {
        const std::size_t end = wire.find("\r\n\r\n");
        if (end != std::string::npos)
            wire.insert(end + 2, "Host: localhost:" + std::to_string(port) + "\r\n");
    }
    if (!send_all(socket, wire, &error)) return {};
    std::string response, one;
    while (recv_exact(socket, 1, one, nullptr)) response += one;
    return response;
}
static std::string status_line(const std::string& response) {
    const auto end = response.find("\r\n");
    return response.substr(0, end);
}
static std::string body(const std::string& response) {
    const auto split = response.find("\r\n\r\n");
    return split == std::string::npos ? std::string{} : response.substr(split + 4);
}

int main() {
    Key owner = make_key(), other = make_key();
    Chain chain;
    CHECK(chain.init_genesis(owner.address, 1000000, other.address, 500));
    Transaction asset;
    asset.kind = TxKind::RegisterAsset;
    asset.from = owner.address; asset.owner = owner.address; asset.pubkey = owner.pk;
    asset.asset_kind = static_cast<std::uint8_t>(AssetKind::Land);
    for (std::size_t i = 0; i < asset.doc_hash.size(); ++i) asset.doc_hash[i] = static_cast<std::uint8_t>(i + 1);
    CHECK(sign(owner.sk, asset.signable_bytes(), asset.signature));
    CHECK(chain.append_block({asset}, 1).ok);

    ApiServer server;
    std::string error;
    CHECK(server.start(chain, 0, &error));
    if (!server.running()) return 1;
    const std::uint16_t port = server.port();

    const std::string status = request(port, "GET /status HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n");
    CHECK(status_line(status).find("200") != std::string::npos);
    CHECK(body(status).find("\"height\": \"") != std::string::npos);
    CHECK(body(status).find("\"height\": 0") == std::string::npos);
    CHECK(body(status).find(to_hex(chain.genesis_commitment())) != std::string::npos);
    CHECK(status.find("Content-Type: application/json") != std::string::npos);
    CHECK(status.find("Access-Control-Allow-Origin:") == std::string::npos);
    CHECK(status.find("Access-Control-Allow-Credentials") == std::string::npos);
    const std::string default_origin = request(port, "GET /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(port) + "\r\nOrigin: https://app.tazvera.com\r\n\r\n");
    CHECK(status_line(default_origin).find("200") != std::string::npos);
    CHECK(default_origin.find("Access-Control-Allow-Origin:") == std::string::npos);

    const std::string host_ip = request(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\n\r\n");
    CHECK(status_line(host_ip).find("200") != std::string::npos);
    const std::string host_localhost = request(port, "GET /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(port) + "\r\n\r\n");
    CHECK(status_line(host_localhost).find("200") != std::string::npos);
    const std::string evil_host = request(port, "GET /status HTTP/1.1\r\nHost: evil.com\r\n\r\n");
    CHECK(status_line(evil_host).find("400") != std::string::npos);
    CHECK(evil_host.find("Connection: close") != std::string::npos);

    const std::string options = request(port, "OPTIONS /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(port) + "\r\nOrigin: https://evil.example\r\n\r\n");
    CHECK(status_line(options).find("204") != std::string::npos);
    CHECK(options.find("Access-Control-Allow-Methods: GET") != std::string::npos);
    CHECK(options.find("Allow: GET") != std::string::npos);
    CHECK(options.find("Access-Control-Allow-Methods: POST") == std::string::npos);
    CHECK(options.find("Access-Control-Allow-Credentials") == std::string::npos);
    CHECK(options.find("Access-Control-Allow-Origin:") == std::string::npos);

    ApiServer cors_server;
    const std::vector<std::string> allowed_origins{"https://app.tazvera.com", "https://admin.tazvera.com"};
    CHECK(cors_server.start(chain, 0, allowed_origins, &error));
    const std::uint16_t cors_port = cors_server.port();
    const std::string allowed_origin = request(cors_port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(cors_port) + "\r\nOrigin: https://app.tazvera.com\r\n\r\n");
    CHECK(status_line(allowed_origin).find("200") != std::string::npos);
    CHECK(allowed_origin.find("Access-Control-Allow-Origin: https://app.tazvera.com") != std::string::npos);
    const std::string second_allowed = request(cors_port, "GET /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(cors_port) + "\r\nOrigin: https://admin.tazvera.com\r\n\r\n");
    CHECK(second_allowed.find("Access-Control-Allow-Origin: https://admin.tazvera.com") != std::string::npos);
    const std::string denied_origin = request(cors_port, "GET /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(cors_port) + "\r\nOrigin: https://evil.example\r\n\r\n");
    CHECK(status_line(denied_origin).find("200") != std::string::npos);
    CHECK(denied_origin.find("Access-Control-Allow-Origin:") == std::string::npos);
    CHECK(denied_origin.find("Access-Control-Allow-Credentials") == std::string::npos);
    const std::string allowed_options = request(cors_port, "OPTIONS /status HTTP/1.1\r\nHost: localhost:" +
        std::to_string(cors_port) + "\r\nOrigin: https://app.tazvera.com\r\n\r\n");
    CHECK(status_line(allowed_options).find("204") != std::string::npos);
    CHECK(allowed_options.find("Access-Control-Allow-Origin: https://app.tazvera.com") != std::string::npos);
    CHECK(allowed_options.find("Access-Control-Allow-Methods: GET") != std::string::npos);
    CHECK(allowed_options.find("Access-Control-Allow-Credentials") == std::string::npos);
    cors_server.stop();

    ApiServer wildcard_server;
    const std::vector<std::string> wildcard_origins{"*"};
    CHECK(!wildcard_server.start(chain, 0, wildcard_origins, &error));

    const std::string balance = request(port, "GET /balance/" + to_hex(owner.address) + " HTTP/1.1\r\n\r\n");
    CHECK(status_line(balance).find("200") != std::string::npos);
    CHECK(body(balance).find("\"balance\":\"999000\"") != std::string::npos);
    CHECK(body(balance).find("\"nonce\":\"1\"") != std::string::npos);
    const std::string bad_address = request(port, "GET /balance/not-an-address HTTP/1.1\r\n\r\n");
    CHECK(status_line(bad_address).find("400") != std::string::npos);

    const std::string assets = request(port, "GET /assets/" + to_hex(owner.address) + " HTTP/1.1\r\n\r\n");
    CHECK(status_line(assets).find("200") != std::string::npos);
    CHECK(body(assets).find("\"asset_kind\":1") != std::string::npos);
    CHECK(body(assets).find(to_hex(asset.doc_hash)) != std::string::npos);
    CHECK(body(assets).find("\"created_at\":\"1\"") != std::string::npos);
    const Hash32 registered_id = asset_id(CHAIN_ID, owner.address, 0);
    CHECK(body(assets).find(to_hex(registered_id)) != std::string::npos);
    const std::string one_asset = request(port, "GET /asset/" + to_hex(registered_id) + " HTTP/1.1\r\n\r\n");
    CHECK(status_line(one_asset).find("200") != std::string::npos);
    CHECK(body(one_asset).find(to_hex(asset.doc_hash)) != std::string::npos);
    const std::string missing_asset = request(port, "GET /asset/" + std::string(64, 'f') + " HTTP/1.1\r\n\r\n");
    CHECK(status_line(missing_asset).find("404") != std::string::npos);
    const std::string missing_block = request(port, "GET /block/999 HTTP/1.1\r\n\r\n");
    CHECK(status_line(missing_block).find("404") != std::string::npos);
    const std::string block = request(port, "GET /block/0 HTTP/1.1\r\n\r\n");
    CHECK(status_line(block).find("200") != std::string::npos);
    CHECK(body(block).find("\"height\": \"0\"") != std::string::npos);
    CHECK(body(block).find("\"height\": \"") != std::string::npos);
    CHECK(body(block).find("\"height\": 0") == std::string::npos);

    const std::string post = request(port, "POST /status HTTP/1.1\r\n\r\n");
    CHECK(status_line(post).find("405") != std::string::npos);
    CHECK(body(post).find("Only GET") != std::string::npos);

    std::string oversized = "GET /";
    oversized.append(8200, 'x');
    oversized += " HTTP/1.1\r\n\r\n";
    const std::string too_long = request(port, oversized);
    CHECK(status_line(too_long).find("400") != std::string::npos);

    // Eight stalled requests occupy all workers. The ninth connection is accepted and closed immediately.
    std::vector<Socket> held(8);
    for (auto& socket : held) CHECK(connect_loopback(port, socket, &error));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Socket ninth;
    CHECK(connect_loopback(port, ninth, &error));
    (void)send_all(ninth, "GET /status HTTP/1.1\r\n\r\n", nullptr);
    std::string ninth_byte;
    const bool ninth_received = recv_exact(ninth, 1, ninth_byte, nullptr);
    CHECK(!ninth_received);
    for (auto& socket : held) {
        (void)send_all(socket, "GET /status HTTP/1.1\r\n\r\n", nullptr);
        std::string response, one;
        while (recv_exact(socket, 1, one, nullptr)) response += one;
        CHECK(status_line(response).find("200") != std::string::npos);
    }

    server.stop();
    if (failures) { std::cerr << failures << " API check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "read-only HTTP API tests: all passed\n";
    return EXIT_SUCCESS;
}
