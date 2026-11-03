# httpmech

**HTTP/1.1 与 WebSocket 的服务端协议层**：站在 [netmech](../netmech) 的 TCP 之上，
处理 nginx 转过来的明文请求。TLS、HTTP/2、静态文件都交给 nginx。

> **状态：v0.1 接口草案，尚未实现。**
> 本仓目前只有 `include/httpmech/*.hpp` 与设计文档，没有 `src/`。
> 目的是先把接口和契约定下来评审，实现随后补。
> 设计细节与"与 wtoe::HttpProtocol 的对照"见 **[docs/design.md](docs/design.md)**。

```
浏览器 ──HTTPS/WSS──► nginx ──HTTP/1.1 明文──► httpmech ──► netmech ──► TCP
                       (TLS 终结)                (本库)
```

## 范围

| v0.1 做 | v0.1 不做 |
|---|---|
| HTTP/1.1 解析与序列化 | TLS / HTTPS（nginx 终结） |
| 路由（精确匹配）+ before/after 钩子 | HTTP/2、gRPC |
| WebSocket（握手 + 帧层 + 分片 + close） | 业务纤程 / 线程池 |
| keep-alive、流式 body、资源上限 | HTTP / WebSocket 客户端（v0.2） |
| `X-Real-IP` / `X-AGENT-SSL` 代理头识别 | 静态文件、gzip、multipart |

---

## 接口速览

### HTTP 服务端 + 路由

```cpp
#include <httpmech/httpmech.hpp>

auto router = std::make_shared<httpmech::Router>();

// 鉴权：返回 false 表示已回响应，短路
router->before([](const httpmech::HttpRequest& req, httpmech::ResponseWriter& out) {
    if (req.header("Authorization").empty()) {
        out.end_status(httpmech::Status::unauthorized, "no token");
        return false;
    }
    return true;
});

router->get("/api/status", [](const httpmech::HttpRequest&, httpmech::ResponseWriter& out) {
    out.set_content_type("application/json");
    out.write(R"({"ok":true})");
    out.end();
});

// 访问日志
router->after([](const httpmech::HttpRequest& req, std::uint16_t status) {
    log("%s %s -> %u", std::string(req.method_text).c_str(),
        std::string(req.path).c_str(), status);
});

httpmech::ServerConfig cfg;
cfg.local = {"127.0.0.1", 8080};            // 只听 nginx 转过来的
cfg.trusted_proxies = {"127.0.0.1"};        // 采信本机 nginx 的 X-Real-IP

auto srv = httpmech::HttpServer::create(netmech::Mechanism::create(), cfg, router);
if (auto ec = srv->start()) { /* 处理错误 */ }
```

### WebSocket

```cpp
httpmech::WebSocketHandlers ws;

ws.on_open = [](httpmech::WebSocketSession& s) {
    s.send_text(R"({"hello":"world"})");
};

ws.on_message = [](httpmech::WebSocketSession& s,
                   httpmech::span<const std::uint8_t> data, bool is_text) {
    if (is_text) {
        s.send_text(std::string_view{reinterpret_cast<const char*>(data.data()), data.size()});
    }
};

ws.on_close = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {
    log("ws closed: %u", code);
};

router->websocket("/ws/events", std::move(ws));   // 握手由库完成
```

### 解析器（单独用）

```cpp
httpmech::Parser p{httpmech::Parser::Role::server};
std::error_code ec;
auto r = p.feed(httpmech::span{buf, n}, ec);      // 可任意切块喂，包括 1 字节
if (r == httpmech::Parser::Result::message) {
    use(p.request());                             // 视图，回调返回即失效
    p.reset();                                    // 准备下一条（keep-alive）
}
```

---

## 契约（三条必须记住的）

1. **视图借用**：`HttpRequest` / `HttpResponse` 里的 `string_view` / `span`
   指向解析器缓冲，**回调返回即失效**。要跨回调保存请自己拷。
2. **回调串行**：同一连接的回调在该连接的 io 线程上串行，不会并发；
   不同连接可能并发，共享状态自己加锁。回调里不要阻塞 —— 耗时活儿丢自己的线程池
   （本库**不提供**纤程或业务线程调度）。
3. **必须收尾**：进了处理器就必须以 `out.end()` 或 `out.upgrade_websocket()` 结束，
   否则库补 `500` 并断开。

---

## 构建

```bash
# netmech 放在同级目录（../netmech），或用 -DHTTPMECH_NETMECH_DIR=<路径>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

交叉编译 aarch64：

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=../netmech/cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
```

## 安装与在别的项目里使用

```bash
cmake --install ../netmech/build     # 先装依赖（本仓按家族约定只装自己）
cmake --install build                # 默认前缀：Linux /usr/local、Windows C:/Program Files/httpmech
```

```cmake
find_package(httpmech REQUIRED)
target_link_libraries(app PRIVATE httpmech::httpmech)
```

---

## 目录结构

```
httpmech/
├── include/httpmech/      对外头文件（当前唯一有内容的部分）
│   ├── httpmech.hpp       汇总
│   ├── common.hpp         Method / Status / Limits / Errc
│   ├── span.hpp           只读 span
│   ├── headers.hpp        HeadersView（借用）/ Headers（拥有）
│   ├── message.hpp        HttpRequest / HttpResponse + 代理头识别
│   ├── parser.hpp         增量 HTTP 解析器
│   ├── websocket.hpp      WebSocket 服务端（会话 / 配置 / 握手）
│   ├── server.hpp         HttpServer / ResponseWriter / RequestHandler
│   ├── router.hpp         Router：精确路由 + before/after
│   └── client.hpp         【v0.2 预留，不实现】
├── docs/design.md         概要设计（含与 wtoe::HttpProtocol 的对照）
├── cmake/                 包配置模板
├── thirdparty/            picohttpparser（MIT，vendored）
├── src/                   （待实现：parser / server / ws 帧层 / sha1-base64）
├── test/                  （待实现）
└── example/               （待实现）
```

## 依赖与许可

- 传输：[netmech](../netmech)（自带 Asio，无需安装 Boost）
- 解析：[picohttpparser](https://github.com/h2o/picohttpparser)（MIT，已 vendored）
- 本库：MIT，见 [LICENSE](LICENSE)
