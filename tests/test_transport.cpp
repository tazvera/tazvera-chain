// The transport layer: send_message / recv_message / exchange_hello / exchange_authenticated_hello are written against
// the Transport interface (send_all, recv_exact, plus prepare_send and shutdown), and PlainTransport is the old socket
// behaviour behind it. Nothing about the wire changes: the same bytes, the same refusals.
//
// Two kinds of evidence here: (1) PlainTransport over a real loopback socket gives the byte sequences that the old
// code put on the wire; (2) a transport that is NOT a socket (two in-memory pipes) carries the whole message layer,
// which proves that the layer no longer depends on sockets, and lets the tests see exactly what the layer asks of
// the transport (when it cuts the connection, which deadline it passes on, how often it prepares a send).
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/net.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

// ---- an in-memory transport: two pipes, one per direction ----
struct Pipe { std::mutex m; std::condition_variable cv; std::string data; bool closed{false}; };

class MemoryTransport final : public Transport {
public:
    MemoryTransport(Pipe& out, Pipe& in) : out_(out), in_(in) {}
    bool send_all(std::string_view bytes, std::string* error, Deadline) override {
        std::lock_guard<std::mutex> lk(out_.m);
        if (out_.closed) { if (error) *error = "the connection closed while sending"; return false; }
        out_.data.append(bytes.data(), bytes.size()); sent_total += bytes.size();
        out_.cv.notify_all();
        return true;
    }
    bool recv_exact(std::size_t count, std::string& out, std::string* error, Deadline deadline) override {
        deadlines_seen.push_back(deadline);
        out.clear();
        std::unique_lock<std::mutex> lk(in_.m);
        auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
        if (deadline != no_deadline()) limit = std::min(limit, deadline);
        in_.cv.wait_until(lk, limit, [&] { return in_.data.size() >= count || in_.closed; });
        if (in_.data.size() >= count) { out = in_.data.substr(0, count); in_.data.erase(0, count); return true; }
        if (error) *error = in_.closed ? "the connection closed while receiving" : "timed out waiting for the peer";
        return false;
    }
    bool prepare_send(Deadline deadline, std::string*) override {
        ++prepare_calls; last_prepare_deadline = deadline; sent_at_last_prepare = sent_total; return true;
    }
    void shutdown() noexcept override {
        ++shutdowns;
        for (Pipe* p : {&out_, &in_}) { std::lock_guard<std::mutex> lk(p->m); p->closed = true; p->cv.notify_all(); }
    }
    int prepare_calls{0}, shutdowns{0};
    std::size_t sent_total{0}, sent_at_last_prepare{0};
    Deadline last_prepare_deadline{};
    std::vector<Deadline> deadlines_seen;
private:
    Pipe& out_; Pipe& in_;
};
struct Link {
    Pipe a_to_b, b_to_a;
    MemoryTransport a{a_to_b, b_to_a}, b{b_to_a, a_to_b};
};

static std::string be32(std::uint32_t v) { std::string s(4, '\0'); for (int i = 3; i >= 0; --i) { s[3 - i] = static_cast<char>((v >> (i * 8)) & 0xff); } return s; }
static std::string frame(std::uint8_t type, const std::string& payload) { return be32(static_cast<std::uint32_t>(payload.size())) + std::string(1, static_cast<char>(type)) + payload; }
static std::string chunk_frame(std::uint8_t original, std::uint32_t stream, std::uint32_t index, std::uint32_t total, const std::string& data) {
    return frame(kMsgChunk, std::string(1, static_cast<char>(original)) + be32(stream) + be32(index) + be32(total) + data);
}
static Chain make_chain(Amount first_balance) {
    PrivKey sk{}, sk2{}; PubKey pk{}, pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) std::exit(2);
    Chain c;
    if (!c.init_genesis(address_from_pubkey(pk), first_balance, address_from_pubkey(pk2), 0)) std::exit(2);
    return c;
}
static bool connected_pair(Listener& l, Socket& server, Socket& client, std::string& err) {
    if (!listen_loopback(0, l, &err)) return false;
    if (!connect_loopback(l.port, client, &err)) return false;
    return accept_one(l.socket, server, &err);
}

// t1: PlainTransport over a real socket gives exactly the old wire bytes and the old refusals
static void t1_plain_transport_over_a_socket() {
    Listener l; Socket server, client; std::string err;
    CHECK(connected_pair(l, server, client, err));
    if (!server.valid() || !client.valid()) return;
    PlainTransport pc(client), ps(server);
    CHECK(pc.send_all("abcdef", &err, no_deadline()));
    std::string got; CHECK(ps.recv_exact(6, got, &err, no_deadline()) && got == "abcdef");
    // a small message is one legacy frame: u32 length, u8 type, payload
    CHECK(send_message(pc, kMsgBlocks, "hello", &err));
    std::string raw; CHECK(recv_exact(server, 10, raw, &err));
    CHECK(raw == frame(kMsgBlocks, "hello") && raw == std::string("\0\0\0\5\3hello", 10));
    // and a frame written by hand is read back as the same message
    CHECK(send_all(client, frame(kMsgWant, "42"), &err));
    std::uint8_t type = 0; std::string payload;
    CHECK(recv_message(ps, type, payload, &err) && type == kMsgWant && payload == "42");
    // a large message is split into bounded frames and put back together
    const std::string big(200000, 'q');
    bool sent = false;
    std::thread sender([&] { std::string e; sent = send_message(pc, kMsgBlocks, big, &e); });
    std::string back; const bool received = recv_message(ps, type, back, &err, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    sender.join();
    CHECK(sent && received && type == kMsgBlocks && back == big);
    // a deadline that passes while the peer is silent is an error, not a hang
    std::string why;
    CHECK(!recv_message(ps, type, payload, &why, std::chrono::steady_clock::now() + std::chrono::milliseconds(200)));
    CHECK(!why.empty());
    // shutdown() cuts the connection: the other side sees it closed
    ps.shutdown();
    std::string one; CHECK(!recv_exact(client, 1, one, &err));
}

// t2: the whole message layer runs over a transport that is not a socket
static void t2_messages_over_memory() {
    Link k; std::string err; std::uint8_t type = 0; std::string payload;
    CHECK(send_message(k.a, kMsgHello, "small", &err) && recv_message(k.b, type, payload, &err) && type == kMsgHello && payload == "small");
    CHECK(send_message(k.a, kMsgNoBlock, "", &err) && recv_message(k.b, type, payload, &err) && type == kMsgNoBlock && payload.empty());
    const std::string big(200000, 'z');
    CHECK(send_message(k.a, kMsgBlocks, big, &err));
    CHECK(k.a_to_b.data.size() == 200000 + 4 * (5 + 13));          // 4 frames, each with the length/type prefix and the chunk header
    CHECK(recv_message(k.b, type, payload, &err) && type == kMsgBlocks && payload == big);
    CHECK(k.a_to_b.data.empty());
    // the other direction, and a message too large to send at all
    CHECK(send_message(k.b, kMsgWant, "7", &err) && recv_message(k.a, type, payload, &err) && type == kMsgWant && payload == "7");
    const std::size_t before = k.a.sent_total;
    std::string why; CHECK(!send_message(k.a, kMsgBlocks, std::string(kMaxStreamBytes + 1u, 'x'), &why) && why.find("message too large") == 0);
    CHECK(k.a.sent_total == before);                                   // nothing of it was written
    CHECK(!send_message(k.a, kMsgChunk, "x", &why));                   // the chunk type is the layer's own, not for callers
}

// t3: hello exchange over memory, both orders
static void t3_hello_over_memory() {
    Link k; const Chain chain = make_chain(1000);
    const Hello ha = hello_for(chain), hb = hello_for(chain);
    Hello got_a, got_b; std::string ea, eb, raw_a, raw_b; bool ok_a = false, ok_b = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread t([&] { ok_b = exchange_hello(k.b, hb, got_b, false, &eb, deadline, &raw_b); });
    ok_a = exchange_hello(k.a, ha, got_a, true, &ea, deadline, &raw_a);
    t.join();
    CHECK(ok_a && ok_b);
    CHECK(encode_hello(got_a) == encode_hello(hb) && encode_hello(got_b) == encode_hello(ha));
    CHECK(raw_a == encode_hello(hb) && raw_b == encode_hello(ha));    // the bytes that arrived
    // a peer that sends something else first is refused
    Link k2; std::string err; Hello got;
    CHECK(send_message(k2.a, kMsgWant, "1", &err));
    CHECK(!exchange_hello(k2.b, hb, got, false, &err, no_deadline(), nullptr));
}

// t4: the authenticated handshake over memory
static void t4_authenticated_hello_over_memory() {
    const Chain chain = make_chain(1000);
    PrivKey ka{}, kb{}; PubKey pa{}, pb{};
    CHECK(generate_keypair(ka, pa)); CHECK(generate_keypair(kb, pb));
    {
        Link k; Hello ha = hello_for(chain), hb = hello_for(chain), got_a, got_b; std::string ea, eb; bool ok_a = false, ok_b = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::thread t([&] { ok_b = exchange_authenticated_hello(k.b, hb, kb, got_b, false, &eb, deadline); });
        ok_a = exchange_authenticated_hello(k.a, ha, ka, got_a, true, &ea, deadline);
        t.join();
        CHECK(ok_a && ok_b);
        CHECK(got_a.pubkey == pb && got_b.pubkey == pa);
        CHECK(hello_signature_ok(ha) && hello_signature_ok(hb));
    }
    {   // a peer that proves no identity is refused
        Link k; Hello ha = hello_for(chain), got_a; const Hello anonymous = hello_for(chain), unused = hello_for(chain);
        Hello got_b; std::string ea, eb; bool ok_a = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::thread t([&] { (void)exchange_hello(k.b, anonymous, got_b, false, &eb, deadline, nullptr); (void)unused; });
        ok_a = exchange_authenticated_hello(k.a, ha, ka, got_a, true, &ea, deadline);
        t.join();
        CHECK(!ok_a && ea.find("identity") != std::string::npos);
    }
}

// t5: the layer cuts the connection (shutdown) exactly when the peer breaks the protocol, and not otherwise
static void t5_shutdown_only_on_protocol_violations() {
    std::uint8_t type = 0; std::string payload, err;
    {   // announced length over the wire ceiling
        Link k; CHECK(k.a.send_all(be32(70000) + std::string(1, '\2'), nullptr, no_deadline()));
        CHECK(!recv_message(k.b, type, payload, &err) && err.find("message too large") == 0);
        CHECK(k.b.shutdowns == 1 && k.a_to_b.closed && k.b_to_a.closed);
    }
    {   // a chunked stream that does not start at index 0
        Link k; CHECK(k.a.send_all(chunk_frame(kMsgBlocks, 5, 1, 2, "x"), nullptr, no_deadline()));
        CHECK(!recv_message(k.b, type, payload, &err) && err == "chunk out of order");
        CHECK(k.b.shutdowns == 1);
    }
    {   // a stream whose second frame is not a chunk
        Link k; CHECK(k.a.send_all(chunk_frame(kMsgBlocks, 5, 0, 2, "x") + frame(kMsgHello, "y"), nullptr, no_deadline()));
        CHECK(!recv_message(k.b, type, payload, &err) && err == "incomplete stream");
        CHECK(k.b.shutdowns == 1);
    }
    {   // silence until the deadline: a timeout is not a protocol violation
        Link k; CHECK(!recv_message(k.b, type, payload, &err, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
        CHECK(k.b.shutdowns == 0);
    }
    {   // the peer closes in the middle of a frame: not a violation either
        Link k; CHECK(k.a.send_all(be32(10) + std::string(1, '\3') + "abc", nullptr, no_deadline()));
        { std::lock_guard<std::mutex> lk(k.a_to_b.m); k.a_to_b.closed = true; k.a_to_b.cv.notify_all(); }
        CHECK(!recv_message(k.b, type, payload, &err) && k.b.shutdowns == 0);
    }
    {   // a good message never cuts anything
        Link k; CHECK(send_message(k.a, kMsgHello, "ok", &err) && recv_message(k.b, type, payload, &err));
        CHECK(k.a.shutdowns == 0 && k.b.shutdowns == 0);
    }
}

// t6: prepare_send is called once per message sent (also for a split one), before its first byte, with the caller's deadline;
//     a deadline that has already passed stops the message before the transport is asked for anything
static void t6_prepare_send_once_per_message() {
    Link k; std::string err;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    CHECK(send_message(k.a, kMsgHello, "one", &err, deadline));
    CHECK(k.a.prepare_calls == 1 && k.a.sent_at_last_prepare == 0 && k.a.last_prepare_deadline == deadline);
    const std::size_t sent_before = k.a.sent_total;
    CHECK(send_message(k.a, kMsgBlocks, std::string(200000, 'p'), &err, deadline));
    CHECK(k.a.prepare_calls == 2 && k.a.sent_at_last_prepare == sent_before && k.a.last_prepare_deadline == deadline);   // 4 frames, one prepare
    const std::size_t total = k.a.sent_total;
    std::string why;
    CHECK(!send_message(k.a, kMsgHello, "late", &why, std::chrono::steady_clock::now() - std::chrono::seconds(1)));
    CHECK(why.find("took longer") != std::string::npos && k.a.prepare_calls == 2 && k.a.sent_total == total);
}

// t7: the deadline given to recv_message reaches every read of the transport (also for a split message)
static void t7_recv_deadline_reaches_every_read() {
    Link k; std::string err; std::uint8_t type = 0; std::string payload;
    CHECK(send_message(k.a, kMsgHello, "one", &err) && send_message(k.a, kMsgBlocks, std::string(150000, 'd'), &err));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    CHECK(recv_message(k.b, type, payload, &err, deadline) && type == kMsgHello);
    CHECK(k.b.deadlines_seen.size() == 3 && std::all_of(k.b.deadlines_seen.begin(), k.b.deadlines_seen.end(), [&](Deadline d) { return d == deadline; }));
    k.b.deadlines_seen.clear();
    CHECK(recv_message(k.b, type, payload, &err, deadline) && type == kMsgBlocks && payload.size() == 150000);
    CHECK(!k.b.deadlines_seen.empty() && std::all_of(k.b.deadlines_seen.begin(), k.b.deadlines_seen.end(), [&](Deadline d) { return d == deadline; }));
}

int main() {
    t1_plain_transport_over_a_socket();
    t2_messages_over_memory();
    t3_hello_over_memory();
    t4_authenticated_hello_over_memory();
    t5_shutdown_only_on_protocol_violations();
    t6_prepare_send_once_per_message();
    t7_recv_deadline_reaches_every_read();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "transport tests: all passed (7 cases)\n";
    return EXIT_SUCCESS;
}
