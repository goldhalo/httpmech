// =============================================================================
//  httpmech 单元测试（自带的极简断言框架，不引第三方）
// =============================================================================
#include <httpmech/httpmech.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
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

void test_server_e2e()
{
    g_case = "server e2e";

    httpmech::ServerConfig cfg;
    cfg.local           = {"127.0.0.1", 18080};
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

    auto mech = netmech::Mechanism::create();
    auto srv  = httpmech::HttpServer::create(mech, cfg, router);
    CHECK(srv != nullptr);
    const auto ec = srv->start();
    CHECK(!ec);
    CHECK(srv->running());

    // --- 1) 基本 GET ---
    int fd = tcp_connect("127.0.0.1", 18080);
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
}

#endif // !_WIN32

} // namespace

int main()
{
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
        {"status",              test_status},
#if !defined(_WIN32)
        {"server e2e",          test_server_e2e},
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
