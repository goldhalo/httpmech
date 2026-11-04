// =============================================================================
//  httpmech 示例：最小可用的 HTTP 服务端
// =============================================================================
//  编译：cmake --build build --target hello_server
//  运行：./build/bin/hello_server [port]
//  访问：
//      curl -i http://127.0.0.1:8080/api/status
//      curl -i -X POST --data 'hello' http://127.0.0.1:8080/api/echo
//      curl -i http://127.0.0.1:8080/api/README
// =============================================================================
#include <httpmech/httpmech.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    const std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1]))
                                          : 8080;

    if (!netmech::initialize(0)) {
        std::fprintf(stderr, "netmech initialize 失败\n");
        return 1;
    }

    auto router = std::make_shared<httpmech::Router>();

    router->before([](httpmech::HttpRequest&, httpmech::ResponseWriter&) { return true; });

    router->get("/api/status", [](const httpmech::HttpRequest& req,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("application/json");
        out.write(std::string{R"({"ok":true,"path":")"} + std::string{req.path} + R"("})");
        out.end();
        (void)req;
    });

    router->post("/api/echo", [](const httpmech::HttpRequest& req,
                                 httpmech::ResponseWriter& out) {
        out.set_content_type(req.content_type().empty() ? "application/octet-stream"
                                                        : req.content_type());
        out.write(req.body);
        out.end();
    });

    // WebSocket 回显：wscat -c ws://127.0.0.1:8080/ws/echo
    {
        httpmech::WebSocketHandlers ws;
        ws.on_open = [](httpmech::WebSocketSession& s) { s.send_text("{\"hello\":\"ws\"}"); };
        ws.on_message = [](httpmech::WebSocketSession& s,
                           httpmech::span<const std::uint8_t> data, bool is_text) {
            if (is_text) {
                s.send_text(std::string_view{reinterpret_cast<const char*>(data.data()),
                                             data.size()});
            } else {
                s.send_binary(data);
            }
        };
        ws.on_close = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {
            std::printf("ws closed: %u\n", code);
        };
        router->websocket("/ws/echo", std::move(ws));
    }

    router->after([](const httpmech::HttpRequest& req,
                     const httpmech::ResponseSummary& res) {
        std::printf("%-6s %-20s -> %u  %zu B  %lld ms\n",
                    std::string{req.method_text}.c_str(),
                    std::string{req.path}.c_str(), res.status, res.body_bytes,
                    static_cast<long long>(res.elapsed.count()));
    });

    httpmech::ServerConfig cfg;
    cfg.local       = {"0.0.0.0", port};
    cfg.server_name = "httpmech";

    auto mech = netmech::Mechanism::create();
    auto srv  = httpmech::HttpServer::create(mech, cfg, router);
    if (!srv) {
        std::fprintf(stderr, "create 失败（handler 为空？）\n");
        return 1;
    }
    if (auto ec = srv->start()) {
        std::fprintf(stderr, "start 失败: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("httpmech  listening on 0.0.0.0:%u\n", port);
    std::printf("  GET  /api/status\n  POST /api/echo\n  WS   /ws/echo\n  Ctrl-C 退出\n");

    while (srv->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    srv->stop();
    netmech::shutdown();
    return 0;
}
