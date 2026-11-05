// =============================================================================
//  httpmech 单元测试（自带的极简断言框架，不引第三方）
// =============================================================================
#include <httpmech/httpmech.hpp>

// 只有 linger_zero 那个用例需要直接构造 netmech 的 Mechanism
#include <netmech/mechanism.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <string_view>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif


namespace {

int g_checks = 0;
int g_fails  = 0;
const char* g_case = "";

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  %s\n", g_case, __FILE__, __LINE__, \
                        #expr);                                                  \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!((a) == (b))) {                                                     \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  %s != %s\n", g_case, __FILE__,      \
                        __LINE__, #a, #b);                                       \
        }                                                                        \
    } while (0)

/// 条件为假时记录失败并**直接返回**（后续代码不可执行时用）
#define REQUIRE(expr)                                                            \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  REQUIRE %s\n", g_case, __FILE__,    \
                        __LINE__, #expr);                                        \
            return;                                                              \
        }                                                                        \
    } while (0)

using httpmech::span;

std::string to_str(span<const std::uint8_t> s)
{
    return std::string{reinterpret_cast<const char*>(s.data()), s.size()};
}

span<const std::uint8_t> bytes(const std::string& s)
{
    return span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

/// 把整段数据一次喂完，返回事件序列（聚合模式应当一次给出 message）。
struct FeedOnce {
    httpmech::Parser::Result result;
    std::error_code          ec;
    std::string              path;
    std::string              body;
    std::size_t              header_count = 0;
};

FeedOnce feed_all(httpmech::Parser& p, const std::string& data)
{
    FeedOnce out{};
    out.result = p.feed(bytes(data), out.ec);
    out.path   = std::string{p.request().path};
    out.body = to_str(p.request().body);
    out.header_count = p.request().headers.size();
    return out;
}

// =============================================================================
//  用例
// =============================================================================

void test_simple_get()
{
    g_case = "simple GET";
    httpmech::Parser p{httpmech::Parser::Role::server};
    auto r = feed_all(p, "GET /api/v1/status?x=1 HTTP/1.1\r\nHost: a\r\n\r\n");

    CHECK(r.result == httpmech::Parser::Result::message);
    CHECK(!r.ec);
    CHECK_EQ(p.request().method, httpmech::Method::get);
    CHECK_EQ(r.path, std::string{"/api/v1/status"});
    CHECK_EQ(std::string{p.request().query}, std::string{"x=1"});
    CHECK_EQ(std::string{p.request().host()}, std::string{"a"});
    CHECK_EQ(r.header_count, std::size_t{1});
    CHECK(p.request().body.empty());
    CHECK(p.request().keep_alive);
    CHECK_EQ(p.messages(), std::uint64_t{1});
}

void test_content_length_body()
{
    g_case = "POST with Content-Length";
    httpmech::Parser p{httpmech::Parser::Role::server};
    auto r = feed_all(p,
                      "POST /cfg HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n"
                      "Content-Type: application/json; charset=utf-8\r\n\r\n"
                      "{\"a\":12345}");

    CHECK(r.result == httpmech::Parser::Result::message);
    CHECK_EQ(r.body, std::string{"{\"a\":12345}"});
    CHECK_EQ(std::string{p.request().content_type()}, std::string{"application/json"});
    CHECK(p.request().content_length().has_value());
    CHECK_EQ(*p.request().content_length(), std::uint64_t{11});
}

void test_chunked_body()
{
    g_case = "chunked body";
    httpmech::Parser p{httpmech::Parser::Role::server};
    auto r = feed_all(p,
                      "POST /up HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");

    CHECK(r.result == httpmech::Parser::Result::message);
    CHECK_EQ(r.body, std::string{"Wikipedia"});
    CHECK(p.request().is_chunked());
}

void test_split_feed()
{
    g_case = "1 byte at a time";
    const std::string wire =
        "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";

    httpmech::Parser p{httpmech::Parser::Role::server};
    std::error_code ec;
    httpmech::Parser::Result last = httpmech::Parser::Result::need_more;

    for (char c : wire) {
        const std::uint8_t b = static_cast<std::uint8_t>(c);
        last = p.feed(span<const std::uint8_t>{&b, 1}, ec);
        CHECK(!ec);
    }
    CHECK(last == httpmech::Parser::Result::message);
    CHECK_EQ(to_str(p.request().body), std::string{"hello"});
}

void test_pipelining()
{
    g_case = "two requests in one buffer";
    httpmech::Parser p{httpmech::Parser::Role::server};
    std::error_code ec;

    auto r = p.feed(bytes("GET /a HTTP/1.1\r\nHost: h\r\n\r\n"
                          "GET /b HTTP/1.1\r\nHost: h\r\n\r\n"),
                    ec);
    CHECK(r == httpmech::Parser::Result::message);
    CHECK(!ec);
    CHECK_EQ(std::string{p.request().path}, std::string{"/a"});

    p.reset();
    CHECK(p.pending() > 0);

    r = p.next(ec);
    CHECK(r == httpmech::Parser::Result::message);
    CHECK_EQ(std::string{p.request().path}, std::string{"/b"});
    CHECK_EQ(p.messages(), std::uint64_t{2});
}

void test_stream_mode()
{
    g_case = "stream body events";
    httpmech::Parser::Config cfg;
    cfg.body_mode = httpmech::Parser::BodyMode::stream;

    httpmech::Parser p{httpmech::Parser::Role::server, cfg};
    std::error_code ec;

    auto r = p.feed(bytes("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n"
                          "0123456789"),
                    ec);
    CHECK(r == httpmech::Parser::Result::headers);
    CHECK(p.request().body_streamed == false);   // 该字段由 server 层填，解析器不管

    std::string got;
    for (;;) {
        r = p.next(ec);
        if (r == httpmech::Parser::Result::body_chunk) {
            CHECK_EQ(p.body_chunk().size(), std::size_t{10});
            got.append(to_str(p.body_chunk()));
            continue;
        }
        CHECK(r == httpmech::Parser::Result::message_end);
        break;
    }
    CHECK_EQ(got, std::string{"0123456789"});
}

void test_keep_alive_semantics()
{
    g_case = "keep-alive flags";
    {
        httpmech::Parser p{httpmech::Parser::Role::server};
        feed_all(p, "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
        CHECK(!p.request().keep_alive);
    }
    {
        httpmech::Parser p{httpmech::Parser::Role::server};
        feed_all(p, "GET / HTTP/1.0\r\nHost: a\r\n\r\n");
        CHECK(!p.request().keep_alive);
    }
    {
        httpmech::Parser p{httpmech::Parser::Role::server};
        feed_all(p, "GET / HTTP/1.0\r\nHost: a\r\nConnection: keep-alive\r\n\r\n");
        CHECK(p.request().keep_alive);
    }
}

void test_limits()
{
    g_case = "header too large";
    {
        httpmech::Parser::Config cfg;
        cfg.limits.max_header_bytes = 32;
        httpmech::Parser p{httpmech::Parser::Role::server, cfg};
        std::error_code ec;
        p.feed(bytes("GET / HTTP/1.1\r\nHost: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n"), ec);
        CHECK_EQ(ec, httpmech::make_error_code(httpmech::Errc::header_too_large));
    }
    g_case = "body too large";
    {
        httpmech::Parser::Config cfg;
        cfg.limits.max_body_bytes = 4;
        httpmech::Parser p{httpmech::Parser::Role::server, cfg};
        std::error_code ec;
        p.feed(bytes("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n0123456789"), ec);
        CHECK_EQ(ec, httpmech::make_error_code(httpmech::Errc::body_too_large));
    }
    g_case = "CL + TE conflict";
    {
        httpmech::Parser p{httpmech::Parser::Role::server};
        std::error_code ec;
        p.feed(bytes("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
                     "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
               ec);
        CHECK_EQ(ec, httpmech::make_error_code(httpmech::Errc::conflicting_length));
    }
    g_case = "bad request line";
    {
        httpmech::Parser p{httpmech::Parser::Role::server};
        std::error_code ec;
        p.feed(bytes("GARBAGE\r\n\r\n"), ec);
        CHECK(ec);
    }
}

void test_proxy_headers()
{
    g_case = "proxy headers";
    httpmech::Parser p{httpmech::Parser::Role::server};
    feed_all(p,
             "GET / HTTP/1.1\r\nHost: a\r\nX-Real-IP: 10.1.2.3\r\nX-Real-PORT: 5555\r\n"
             "X-AGENT-SSL: 1\r\n\r\n");

    // 未标记为可信代理时一律不采信
    CHECK(p.request().real_ip().empty());
    CHECK_EQ(p.request().real_port(), std::uint16_t{0});
    CHECK(!p.request().is_forwarded_https());
}

void test_headers_container()
{
    g_case = "Headers (owning)";
    httpmech::Headers h;
    h.set("Content-Type", "text/plain");
    h.add("Set-Cookie", "a=1");
    h.add("Set-Cookie", "b=2");
    CHECK_EQ(h.count("set-cookie"), std::size_t{2});
    CHECK_EQ(std::string{h.get("CONTENT-TYPE")}, std::string{"text/plain"});

    auto v = h.view();
    CHECK_EQ(v.size(), std::size_t{3});
    CHECK_EQ(std::string{v.get("SET-COOKIE")}, std::string{"a=1"});

    h.set("Set-Cookie", "c=3");   // set 覆盖同名
    CHECK_EQ(h.count("Set-Cookie"), std::size_t{1});
    CHECK_EQ(h.size(), std::size_t{2});
}

void test_websocket_accept()
{
    g_case = "websocket accept";
    // RFC 6455 §1.3 的官方例子
    CHECK_EQ(httpmech::make_websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
             std::string{"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="});
}

void test_status()
{
    g_case = "status helpers";
    CHECK_EQ(std::string{httpmech::reason_phrase(httpmech::Status::not_found)},
             std::string{"Not Found"});
    CHECK(httpmech::status_from(404) == httpmech::Status::not_found);
    CHECK(httpmech::status_from(999) == httpmech::Status::other);
    CHECK(httpmech::is_server_error(httpmech::Status::bad_gateway));
    CHECK(!httpmech::is_success(httpmech::Status::not_found));
    CHECK_EQ(std::string{httpmech::to_string(httpmech::Method::delete_)}, std::string{"DELETE"});
    CHECK(httpmech::method_from("PATCH") == httpmech::Method::patch);
}

// =============================================================================
//  端到端：起一个真服务端，用阻塞 socket 发真请求
// =============================================================================
#if !defined(_WIN32)

int tcp_connect(const char* ip, std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, ip, &addr.sin_addr);

    timeval tv{};
    tv.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string tcp_roundtrip(int fd, const std::string& req)
{
    if (::send(fd, req.data(), req.size(), 0) < 0) {
        return {};
    }
    std::string resp;
    char        buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        resp.append(buf, static_cast<std::size_t>(n));
        // 简单判据：收到 Content-Length 指定的字节数就停
        const auto hdr_end = resp.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            const auto cl_pos = resp.find("Content-Length: ");
            if (cl_pos != std::string::npos && cl_pos < hdr_end) {
                const std::size_t cl = std::strtoul(resp.c_str() + cl_pos + 16, nullptr, 10);
                if (resp.size() >= hdr_end + 4 + cl) {
                    break;
                }
            } else {
                break;
            }
        }
    }
    return resp;
}

// --- WebSocket 用的原始收发 ---

bool tcp_send_all(int fd, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool tcp_recv_exact(int fd, char* out, std::size_t n)
{
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, out + got, n - got, 0);
        if (r <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
}

/// 构造一帧（客户端 → 服务端必须打掩码）
std::string ws_build_frame(std::uint8_t opcode, const std::string& payload, bool fin = true)
{
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));

    const std::uint8_t mask_bit = 0x80;
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(mask_bit | payload.size()));
    } else {
        f.push_back(static_cast<char>(mask_bit | 126));
        f.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    const std::uint8_t key[4] = {0x37, 0xFA, 0x21, 0x3D};
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4]));
    }
    return f;
}

/// 读一帧；失败返回 false
bool ws_read_frame(int fd, std::uint8_t& opcode, std::string& payload)
{
    unsigned char hdr[2];
    if (!tcp_recv_exact(fd, reinterpret_cast<char*>(hdr), 2)) {
        return false;
    }
    opcode = static_cast<std::uint8_t>(hdr[0] & 0x0F);
    const bool masked = (hdr[1] & 0x80) != 0;
    std::uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (!tcp_recv_exact(fd, reinterpret_cast<char*>(ext), 2)) {
            return false;
        }
        len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!tcp_recv_exact(fd, reinterpret_cast<char*>(ext), 8)) {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | ext[i];
        }
    }
    unsigned char key[4] = {0, 0, 0, 0};
    if (masked && !tcp_recv_exact(fd, reinterpret_cast<char*>(key), 4)) {
        return false;
    }
    payload.assign(static_cast<std::size_t>(len), '\0');
    if (len != 0 && !tcp_recv_exact(fd, &payload[0], static_cast<std::size_t>(len))) {
        return false;
    }
    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4]);
        }
    }
    return true;
}

// =============================================================================
//  Connection: close —— 应答必须完整送达，且以 FIN 收尾（不能是 RST）
// =============================================================================
void test_connection_close_delivery()
{
    g_case = "connection close delivery";

    httpmech::ServerConfig cfg;
    cfg.local     = {"127.0.0.1", 0};
    cfg.emit_date = false;

    auto router = std::make_shared<httpmech::Router>();

    // 8 KiB 的 body：足够大到需要分多次 write，逼近内核缓冲区边界
    const std::string payload(8 * 1024, 'z');
    router->get("/big", [payload](const httpmech::HttpRequest&,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("text/plain");
        out.write(payload);
        out.end();
    });

    auto srv  = httpmech::HttpServer::create(cfg, router);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    const int fd = tcp_connect("127.0.0.1", srv->local().port());
    REQUIRE(fd >= 0);

    REQUIRE(tcp_send_all(fd,
        "GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"));

    // 一直读到 EOF
    std::string resp;
    char        buf[4096];
    bool        clean_eof = false;
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            resp.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        // n == 0 → 正常 FIN；n < 0 → 超时或 RST
        clean_eof = (n == 0);
        break;
    }

    CHECK(clean_eof);   // 如果是 RST，这里会是 false（ECONNRESET）
    CHECK(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(resp.find("Connection: close") != std::string::npos);
    // 正文必须一个字节都不少
    CHECK(resp.size() >= payload.size());
    CHECK(resp.compare(resp.size() - payload.size(), payload.size(), payload) == 0);

    ::close(fd);
    srv->stop();
}

// =============================================================================
//  linger_zero 拦截：SO_LINGER{1,0} 会丢最后那一包，必须在启动时拦住
// =============================================================================
void test_linger_zero_guard()
{
    g_case = "linger_zero guard";

    auto router = std::make_shared<httpmech::Router>();
    router->get("/", [](const httpmech::HttpRequest&, httpmech::ResponseWriter& out) {
        out.end_status(httpmech::Status::ok, "ok");
    });

    // 用 linger_zero = true 建 Mechanism：HTTP 会拒绝启动
    netmech::Mechanism::Config mcfg;
    mcfg.linger_zero = true;
    auto mech_bad    = netmech::Mechanism::create(mcfg);

    httpmech::ServerConfig cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = httpmech::HttpServer::create_with_mechanism(mech_bad, cfg, router);
    REQUIRE(srv != nullptr);
    const auto ec = srv->start();
    CHECK(ec == httpmech::make_error_code(httpmech::Errc::linger_zero_enabled));
    CHECK(!srv->running());

    // 显式放开后允许启动（自己承担丢应答的风险）
    httpmech::ServerConfig cfg2 = cfg;
    cfg2.allow_linger_zero      = true;
    auto srv2 = httpmech::HttpServer::create_with_mechanism(mech_bad, cfg2, router);
    REQUIRE(srv2 != nullptr);
    CHECK(!srv2->start());
    srv2->stop();
}

// =============================================================================
//  客户端
// =============================================================================

/// 同步等待帮手：回调在 io 线程，测试线程在这里等。
struct Waiter {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;

    void signal()
    {
        std::lock_guard<std::mutex> lk(mu);
        done = true;
        cv.notify_all();
    }
    bool wait(int ms = 5000)
    {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return done; });
    }
};

/// 只收整条的处理器
class SyncHandler : public httpmech::ResponseHandler {
public:
    explicit SyncHandler(std::shared_ptr<Waiter> w) : waiter(std::move(w)) {}

    void on_response(const httpmech::HttpResponse& resp) override
    {
        status = resp.status_code;
        body.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    }
    void on_complete(std::error_code e) override
    {
        ec = e;
        waiter->signal();
    }

    std::shared_ptr<Waiter> waiter;
    std::string             body;
    std::uint16_t           status = 0;
    std::error_code         ec;
};

void test_http_client_e2e()
{
    g_case = "http client e2e";

    // --- 起一个真服务端 ---
    httpmech::ServerConfig cfg;
    cfg.local     = {"127.0.0.1", 0};
    cfg.emit_date = false;

    auto router = std::make_shared<httpmech::Router>();
    router->get("/api/status", [](const httpmech::HttpRequest&,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("application/json");
        out.write(R"({"ok":true})");
        out.end();
    });
    router->post("/api/echo", [](const httpmech::HttpRequest& req,
                                 httpmech::ResponseWriter& out) {
        out.write(req.body);
        out.end();
    });

    auto srv = httpmech::HttpServer::create(cfg, router);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // --- 客户端连过去 ---
    httpmech::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};
    auto cli    = httpmech::HttpClient::create(ccfg);
    REQUIRE(cli != nullptr);

    // 1) GET
    {
        auto w = std::make_shared<Waiter>();
        auto h = std::make_shared<SyncHandler>(w);
        httpmech::RequestSpec spec;
        spec.target = "/api/status";
        REQUIRE(cli->request(std::move(spec), h));
        CHECK(w->wait());
        CHECK(!h->ec);
        CHECK_EQ(static_cast<int>(h->status), 200);
        CHECK_EQ(h->body, std::string{R"({"ok":true})"});
    }

    // 2) POST（同一条连接复用）
    {
        auto w = std::make_shared<Waiter>();
        auto h = std::make_shared<SyncHandler>(w);
        httpmech::RequestSpec spec;
        spec.method = httpmech::Method::post;
        spec.target = "/api/echo";
        spec.body   = "hello-client";
        REQUIRE(cli->request(std::move(spec), h));
        CHECK(w->wait());
        CHECK(!h->ec);
        CHECK_EQ(h->body, std::string{"hello-client"});
    }

    // 3) 404 也要原样交给应用（不当作错误）
    {
        auto w = std::make_shared<Waiter>();
        auto h = std::make_shared<SyncHandler>(w);
        httpmech::RequestSpec spec;
        spec.target = "/nope";
        REQUIRE(cli->request(std::move(spec), h));
        CHECK(w->wait());
        CHECK(!h->ec);
        CHECK_EQ(static_cast<int>(h->status), 404);
    }

    CHECK(cli->requests_total() >= 3);
    CHECK_EQ(cli->failures_total(), std::uint64_t{0});

    cli->close();
    srv->stop();
}

// -----------------------------------------------------------------------------
//  chunked 响应：用一个"假服务端"直接往 socket 里写手写报文。
//  我们自己的服务端只会发 Content-Length，chunked 解码路径只能这样测。
// -----------------------------------------------------------------------------
void test_http_client_chunked()
{
    g_case = "http client chunked";

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(lfd, 4) == 0);

    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);

    std::thread fake([lfd] {
        const int c = ::accept(lfd, nullptr, nullptr);
        if (c < 0) {
            return;
        }
        char buf[1024];
        (void)::recv(c, buf, sizeof(buf), 0);   // 请求内容不关心
        const std::string resp =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        (void)::send(c, resp.data(), resp.size(), 0);
        ::shutdown(c, SHUT_WR);
        ::close(c);
    });

    httpmech::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", port};
    auto cli    = httpmech::HttpClient::create(ccfg);
    REQUIRE(cli != nullptr);

    auto w = std::make_shared<Waiter>();
    auto h = std::make_shared<SyncHandler>(w);
    httpmech::RequestSpec spec;
    spec.target = "/x";
    REQUIRE(cli->request(std::move(spec), h));
    CHECK(w->wait());
    CHECK(!h->ec);
    CHECK_EQ(static_cast<int>(h->status), 200);
    CHECK_EQ(h->body, std::string{"Wikipedia"});

    cli->close();
    fake.join();
    ::close(lfd);
}

void test_websocket_client_e2e()
{
    g_case = "websocket client e2e";

    // 服务端：/ws 回显
    httpmech::ServerConfig cfg;
    cfg.local     = {"127.0.0.1", 0};
    cfg.emit_date = false;

    auto router = std::make_shared<httpmech::Router>();
    httpmech::WebSocketHandlers ws;
    ws.on_open = [](httpmech::WebSocketSession& s) { s.send_text("server-hello"); };
    ws.on_message = [](httpmech::WebSocketSession& s,
                       httpmech::span<const std::uint8_t> d, bool text) {
        if (text) {
            s.send_text(std::string_view{reinterpret_cast<const char*>(d.data()), d.size()});
        } else {
            s.send_binary(d);
        }
    };
    router->websocket("/ws", std::move(ws));

    auto srv = httpmech::HttpServer::create(cfg, router);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // 客户端
    httpmech::WebSocketClientConfig wcfg;
    wcfg.remote = {"127.0.0.1", srv->local().port()};
    wcfg.path   = "/ws";
    auto cli    = httpmech::WebSocketClient::create(wcfg);
    REQUIRE(cli != nullptr);

    auto wait_open  = std::make_shared<Waiter>();
    auto wait_first = std::make_shared<Waiter>();
    auto wait_echo  = std::make_shared<Waiter>();
    std::string first_msg;
    std::string echo_msg;
    std::error_code conn_ec;

    httpmech::WebSocketClientHandlers h;
    h.on_open = [&](httpmech::WebSocketSession& s) {
        wait_open->signal();
        s.send_text("client-hi");
    };
    h.on_message = [&](httpmech::WebSocketSession&, httpmech::span<const std::uint8_t> d,
                       bool /*text*/) {
        const std::string m{reinterpret_cast<const char*>(d.data()), d.size()};
        if (!wait_first->done) {
            first_msg = m;
            wait_first->signal();
        } else {
            echo_msg = m;
            wait_echo->signal();
        }
    };
    h.on_close = [](httpmech::WebSocketSession&, std::uint16_t, std::string_view) {};
    h.on_error = [&](std::error_code ec) {
        conn_ec = ec;
        wait_open->signal();
    };

    REQUIRE(cli->connect(std::move(h)));
    CHECK(wait_open->wait());
    CHECK(!conn_ec);
    CHECK(cli->is_open());

    CHECK(wait_first->wait());
    CHECK_EQ(first_msg, std::string{"server-hello"});

    CHECK(wait_echo->wait());
    CHECK_EQ(echo_msg, std::string{"client-hi"});

    CHECK_EQ(cli->messages_received(), std::uint64_t{2});

    cli->close();
    srv->stop();
}

void test_websocket_e2e()
{
    g_case = "websocket e2e";

    httpmech::ServerConfig cfg;
    cfg.local       = {"127.0.0.1", 0};
    cfg.emit_date   = false;
    cfg.server_name = "httpmech-ut";

    auto router = std::make_shared<httpmech::Router>();

    httpmech::WebSocketHandlers ws;
    ws.on_open = [](httpmech::WebSocketSession& s) { s.send_text("welcome"); };
    ws.on_message = [](httpmech::WebSocketSession& s,
                       httpmech::span<const std::uint8_t> data, bool is_text) {
        if (is_text) {
            s.send_text(std::string_view{reinterpret_cast<const char*>(data.data()), data.size()});
        } else {
            s.send_binary(data);
        }
    };
    router->websocket("/ws", std::move(ws));

    auto srv  = httpmech::HttpServer::create(cfg, router);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());
    const std::uint16_t port = srv->local().port();
    REQUIRE(port != 0);

    const int fd = tcp_connect("127.0.0.1", port);
    REQUIRE(fd >= 0);

    // --- 握手 ---
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string req =
        "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(tcp_send_all(fd, req));

    std::string head;
    char        c = 0;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (!tcp_recv_exact(fd, &c, 1)) {
            break;
        }
        head.push_back(c);
    }
    CHECK(head.rfind("HTTP/1.1 101", 0) == 0);
    // RFC 6455 §1.3 的例子：这个 key 对应的 accept 是固定值
    CHECK(head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

    // --- on_open 主动发的欢迎消息 ---
    {
        std::uint8_t op = 0;
        std::string  payload;
        REQUIRE(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0x1);
        CHECK_EQ(payload, std::string{"welcome"});
    }

    // --- 文本回显 ---
    REQUIRE(tcp_send_all(fd, ws_build_frame(0x1, "ping-text")));
    {
        std::uint8_t op = 0;
        std::string  payload;
        REQUIRE(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0x1);
        CHECK_EQ(payload, std::string{"ping-text"});
    }

    // --- 二进制回显（含 126~65535 的长度前缀）---
    {
        std::string big(300, 'x');
        REQUIRE(tcp_send_all(fd, ws_build_frame(0x2, big)));
        std::uint8_t op = 0;
        std::string  payload;
        REQUIRE(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0x2);
        CHECK_EQ(payload.size(), big.size());
    }

    // --- ping 自动回 pong ---
    REQUIRE(tcp_send_all(fd, ws_build_frame(0x9, "hb")));
    {
        std::uint8_t op = 0;
        std::string  payload;
        REQUIRE(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0xA);
        CHECK_EQ(payload, std::string{"hb"});
    }

    // --- 分片：两段拼成一条消息 ---
    {
        REQUIRE(tcp_send_all(fd, ws_build_frame(0x1, "he", false)));
        REQUIRE(tcp_send_all(fd, ws_build_frame(0x0, "llo", true)));
        std::uint8_t op = 0;
        std::string  payload;
        REQUIRE(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0x1);
        CHECK_EQ(payload, std::string{"hello"});
    }

    // --- close 握手 ---
    {
        std::string close_payload;
        close_payload.push_back(static_cast<char>(0x03));
        close_payload.push_back(static_cast<char>(0xE8));   // 1000
        REQUIRE(tcp_send_all(fd, ws_build_frame(0x8, close_payload)));
        std::uint8_t op = 0;
        std::string  payload;
        CHECK(ws_read_frame(fd, op, payload));
        CHECK_EQ(static_cast<int>(op), 0x8);
    }

    ::close(fd);
    srv->stop();
}

void test_server_e2e()
{
    g_case = "server e2e";

    httpmech::ServerConfig cfg;
    cfg.local           = {"127.0.0.1", 0};   // 端口 0：交给系统，再问 local()
    cfg.emit_date       = false;
    cfg.server_name     = "httpmech-ut";

    auto router = std::make_shared<httpmech::Router>();
    router->get("/api/status", [](const httpmech::HttpRequest& req,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("application/json");
        out.write(std::string{R"({"ok":true,"path":")"} + std::string{req.path} + R"("})");
        out.end();
    });
    router->post("/api/echo", [](const httpmech::HttpRequest& req,
                                 httpmech::ResponseWriter& out) {
        out.write(httpmech::span<const std::uint8_t>{
            req.body.data(), req.body.size()});
        out.end();
    });

    auto srv  = httpmech::HttpServer::create(cfg, router);
    CHECK(srv != nullptr);
    const auto ec = srv->start();
    CHECK(!ec);
    CHECK(srv->running());

    // 端口 0 的场景：local() 必须给出系统分配的真实端口
    const std::uint16_t port = srv->local().port();
    CHECK(port != 0);

    // --- 1) 基本 GET ---
    int fd = tcp_connect("127.0.0.1", port);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const std::string r1 = tcp_roundtrip(
            fd, "GET /api/status HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK(r1.rfind("HTTP/1.1 200 OK", 0) == 0);
        CHECK(r1.find(R"({"ok":true,"path":"/api/status"})") != std::string::npos);
        CHECK(r1.find("Connection: keep-alive") != std::string::npos);

        // --- 2) 同一条连接上再来一个 POST（keep-alive 复用）---
        const std::string body = "hello-body";
        const std::string r2 = tcp_roundtrip(
            fd, "POST /api/echo HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body);
        CHECK(r2.rfind("HTTP/1.1 200 OK", 0) == 0);
        CHECK(r2.find(body) != std::string::npos);

        // --- 3) 未匹配路由 → 404 ---
        const std::string r3 = tcp_roundtrip(
            fd, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK(r3.rfind("HTTP/1.1 404", 0) == 0);

        // --- 4) 方法不匹配 → 405 + Allow ---
        const std::string r4 = tcp_roundtrip(
            fd, "DELETE /api/status HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK(r4.rfind("HTTP/1.1 405", 0) == 0);
        CHECK(r4.find("Allow: GET") != std::string::npos);

        ::close(fd);
    }

    CHECK(srv->requests_total() >= 4);
    CHECK(srv->protocol_errors_total() == 0);

    srv->stop();
    CHECK(!srv->running());

    // 停止后端口应当被释放：同一个端口可以再被别人监听
    auto srv2  = httpmech::HttpServer::create(cfg, router);
    // 用刚才那个端口重新起一个
    httpmech::ServerConfig cfg2 = cfg;
    cfg2.local = {"127.0.0.1", port};
    auto srv3 = httpmech::HttpServer::create(cfg2, router);
    CHECK(srv3 != nullptr);
    CHECK(!srv3->start());
    CHECK_EQ(srv3->local().port(), port);
    srv3->stop();
}

#endif // !_WIN32

} // namespace

int main()
{
#if defined(_WIN32)
    // Windows 控制台默认是 GBK 代码页，源码里的中文按 UTF-8 输出会乱码
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("---- httpmech 单元测试 ----\n");

#if !defined(_WIN32)
    if (!netmech::initialize(2)) {
        std::printf("netmech initialize 失败\n");
        return 1;
    }
#endif

    struct Case {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"simple GET",          test_simple_get},
        {"content-length body", test_content_length_body},
        {"chunked body",        test_chunked_body},
        {"split feed",          test_split_feed},
        {"pipelining",          test_pipelining},
        {"stream mode",         test_stream_mode},
        {"keep-alive",          test_keep_alive_semantics},
        {"limits",              test_limits},
        {"proxy headers",       test_proxy_headers},
        {"headers container",   test_headers_container},
        {"websocket accept",    test_websocket_accept},
        {"status",              test_status},
#if !defined(_WIN32)
        {"server e2e",          test_server_e2e},
        {"connection close",    test_connection_close_delivery},
        {"linger_zero guard",   test_linger_zero_guard},
        {"websocket e2e",       test_websocket_e2e},
        {"http client e2e",     test_http_client_e2e},
        {"http client chunked", test_http_client_chunked},
        {"websocket client e2e", test_websocket_client_e2e},
#endif
    };

    for (const auto& c : cases) {
        const int before = g_fails;
        std::printf("  [%s]\n", c.name);
        c.fn();
        std::printf("    %s\n", g_fails == before ? "ok" : "FAILED");
    }

    std::printf("\n断言: %d 通过 / %d 失败 / %d 总计\n",
                g_checks - g_fails, g_fails, g_checks);
    std::printf("结果: %s\n", g_fails == 0 ? "PASS" : "FAIL");

#if !defined(_WIN32)
    netmech::shutdown();
#endif
    return g_fails == 0 ? 0 : 1;
}
