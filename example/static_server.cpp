// =============================================================================
//  httpmech 示例：静态文件服务（异步响应体，支持大文件）
// =============================================================================
//  编译：cmake --build build --target static_server
//  运行：./build/bin/static_server [port] [root-dir]
//  访问：
//      curl -i http://127.0.0.1:8080/
//      curl -i http://127.0.0.1:8080/api/status
//      curl -I  http://127.0.0.1:8080/index.html
//      curl -i -H 'Range: bytes=0-9' http://127.0.0.1:8080/index.html
//      curl -i -H 'If-None-Match: <上一步的 ETag>' http://127.0.0.1:8080/index.html
//      curl -o /dev/null -w '%{size_download}\n' http://127.0.0.1:8080/big.bin
// =============================================================================
#include <httpmech/httpmech.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    const std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1]))
                                          : 8080;
    const std::string   root = (argc > 2) ? argv[2] : "www";

    httpmech::StaticFileConfig sf;
    sf.mount_point      = "/";
    sf.root             = root;
    sf.index_file       = "index.html";
    sf.mime_types["md"] = "text/markdown; charset=utf-8";
    sf.extra_headers.set("Cache-Control", "public, max-age=60");
    // max_file_bytes 默认 0 = 不限制：文件按 256 KiB 分块投递，
    // 内存占用与文件大小无关。要限制单文件大小可自行设成非 0。

    httpmech::ServerConfig cfg;
    cfg.local     = {"0.0.0.0", port};
    cfg.emit_date = true;
    // 单连接发送队列水位：慢客户端靠它背压，不会把整文件堆进内存。
    cfg.max_send_queue_bytes = 2 * 1024 * 1024;
    // 不再需要调 max_buffered_response：它只管老的 write() 自动切
    // chunked 路径，静态文件走的是 set_body_provider() 独立通道。

    auto router = std::make_shared<httpmech::Router>();

    // before 先于静态兜底执行：鉴权在这里做，静态文件不会绕过它。
    router->before([](httpmech::HttpRequest&, httpmech::ResponseWriter&) { return true; });

    router->get("/api/status", [](const httpmech::HttpRequest&,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("application/json");
        out.write(R"({"ok":true})");
        out.end();
    });

    auto files = httpmech::mount_static(router, std::move(sf));
    if (!files->valid()) {
        std::fprintf(stderr, "静态目录配置无效: %s\n", files->error_message().c_str());
        return 1;
    }

    auto srv = httpmech::HttpServer::create(cfg, router);
    if (!srv) {
        std::fprintf(stderr, "create 失败\n");
        return 1;
    }
    if (auto ec = srv->start()) {
        std::fprintf(stderr, "start 失败: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("httpmech static server  :%u\n", port);
    std::printf("  root = %s\n", files->resolved_root().c_str());
    std::printf("  GET /api/status  /  GET /*  /  Range  /  Ctrl-C 退出\n");

    while (srv->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    srv->stop();
    return 0;
}
