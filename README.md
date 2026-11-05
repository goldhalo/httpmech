# httpmech

[![CI](https://github.com/goldhalo/httpmech/actions/workflows/ci.yml/badge.svg)](https://github.com/goldhalo/httpmech/actions/workflows/ci.yml)

**HTTP/1.1 与 WebSocket 的服务端协议层**，基于 [netmech](../netmech) 的 TCP。
TLS、HTTP/2、静态文件都交给前面的 nginx，本库只处理它转过来的明文请求。

```
浏览器 ──HTTPS/WSS──► nginx ──HTTP/1.1 明文──► httpmech ──► netmech ──► TCP
                       (TLS 终结)                (本库)
```

- **公共头不依赖 netmech**：只用 httpmech 时不需要把 netmech 的头挂到 include 路径上
  （链接仍需要 `libnetmech.a`，见下方"依赖"）
- 自带路由、before/after 钩子、WebSocket（握手 + 帧层），**零外部依赖**
- C++17，不引 Boost / OpenSSL；WebSocket 握手用的 SHA-1 + Base64 也是自带的
- 212 个断言（单元 + 端到端），CI 覆盖 Linux（gcc/clang × Debug/Release）与 Windows MSVC

---

## 范围

| 做 | 不做 |
|---|---|
| HTTP/1.1 解析与序列化、chunked 请求体 | TLS / HTTPS（nginx 终结） |
| 路由（精确匹配）+ before/after 钩子 | HTTP/2、gRPC |
| WebSocket（握手、帧层、分片、ping/pong、close） | 业务纤程 / 线程池 |
| keep-alive、流式 body、资源上限 | 连接池 / 自动重连 / 重定向跟随 |
| `X-Real-IP` / `X-AGENT-SSL` 代理头识别（白名单） | 静态文件、gzip、multipart |
| **客户端**：HttpClient（串行队列）/ WebSocketClient | 连接池、自动重连、cookie 罐 |
| **客户端**：HttpClient（串行队列）/ WebSocketClient | 连接池、自动重连、cookie 罐 |

设计取舍见 **[docs/design.md](docs/design.md)**（含与 wtoe::HttpProtocol 的对照）。

---

## 完整示例

下面这段就是 `example/hello_server.cpp` 的实质内容，可直接编译运行：

```cpp
#include <httpmech/httpmech.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    auto router = std::make_shared<httpmech::Router>();

    // ---- 鉴权：before 返回 false 表示已回响应，后续不再走 ----
    router->before([](httpmech::HttpRequest& req, httpmech::ResponseWriter& out) {
        const auto token = req.header("Authorization");
        if (token.empty()) {
            out.end_status(httpmech::Status::unauthorized, "missing token");
            return false;
        }
        req.user = std::make_shared<std::string>(token);   // 挂给后面用
        return true;
    });

    // ---- 一个 GET ----
    router->get("/api/status", [](const httpmech::HttpRequest& req,
                                  httpmech::ResponseWriter& out) {
        out.set_content_type("application/json");
        out.write(std::string{R"({"ok":true,"path":")"} + std::string{req.path} + R"("})");
        out.end();
    });

    // ---- 一个 POST：读 body 再回写 ----
    router->post("/api/echo", [](const httpmech::HttpRequest& req,
                                 httpmech::ResponseWriter& out) {
        out.set_content_type("application/octet-stream");
        out.write(req.body);
        out.end();
    });

    // ---- WebSocket ----
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
    ws.on_close = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {
        std::printf("ws closed: %u\n", code);
    };
    router->websocket("/ws/echo", std::move(ws));

    // ---- 访问日志：after 拿到响应摘要 ----
    router->after([](const httpmech::HttpRequest& req, const httpmech::ResponseSummary& res) {
        std::printf("%-6s %-16s -> %u  %zu B  %lld ms\n",
                    std::string{req.method_text}.c_str(), std::string{req.path}.c_str(),
                    res.status, res.body_bytes,
                    static_cast<long long>(res.elapsed.count()));
    });

    httpmech::ServerConfig cfg;
    cfg.local           = {"127.0.0.1", 8080};   // 0 也行，start() 后用 local() 取真实端口
    cfg.trusted_proxies = {"127.0.0.1"};         // 采信 nginx 的 X-Real-IP

    // 不需要 netmech：server 自己建 io 运行时
    auto srv = httpmech::HttpServer::create(cfg, router);
    if (auto ec = srv->start()) {
        std::fprintf(stderr, "start 失败: %s\n", ec.message().c_str());
        return 1;
    }

    while (srv->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    srv->stop();
}
```

跑起来：

```bash
cmake --build build --target hello_server
./build/bin/hello_server 8080

curl -i http://127.0.0.1:8080/api/status -H 'Authorization: x'
curl -i -X POST --data 'hello' http://127.0.0.1:8080/api/echo -H 'Authorization: x'
wscat -c ws://127.0.0.1:8080/ws/echo        # WebSocket 回显
```

### 只想要 HTTP、不要路由表

`Router` 只是一个现成的 `RequestHandler` 实现。要自己接管所有请求，
直接实现接口并交给 `HttpServer::create()`：

```cpp
class MyHandler : public httpmech::RequestHandler {
public:
    void on_request(httpmech::HttpRequest& req, httpmech::ResponseWriter& out) override
    {
        out.set_content_type("text/plain");
        out.write("hello");
        out.end();
    }
};

auto srv = httpmech::HttpServer::create(cfg, std::make_shared<MyHandler>());
```

### 大 body：流式收

默认是聚合模式（`max_body_bytes` 以内一次给完整 body）。要收上传/大报文，
切到流式模式，body 会被分块喂过来，不占内存：

```cpp
httpmech::ServerConfig cfg;
cfg.body_mode = httpmech::Parser::BodyMode::stream;

class UploadHandler : public httpmech::RequestHandler {
public:
    void on_request(httpmech::HttpRequest&, httpmech::ResponseWriter&) override {}   // 流式下不调用

    bool on_headers(httpmech::HttpRequest& req, httpmech::ResponseWriter& out) override
    {
        if (req.content_length().value_or(0) > 512ull * 1024 * 1024) {
            out.end_status(httpmech::Status::payload_too_large, "too big");
            return false;                       // 拒收，省带宽
        }
        return true;
    }

    void on_body_data(httpmech::HttpRequest&, httpmech::span<const std::uint8_t> data) override
    {
        // 落盘 / 算摘要 / 转发。data 只在本次回调内有效。
    }

    void on_body_end(httpmech::HttpRequest&, httpmech::ResponseWriter& out) override
    {
        out.end_status(httpmech::Status::ok, "stored");
    }
};
```

### 客户端

```cpp
// ---- HTTP ----
httpmech::ClientConfig ccfg;
ccfg.remote = {"192.168.3.10", 9898};

auto cli = httpmech::HttpClient::create(ccfg);

class MyHandler : public httpmech::ResponseHandler {
public:
    void on_response(const httpmech::HttpResponse& resp) override
    {
        // resp.body 在聚合模式下是完整的；视图只在回调内有效
        std::printf("status=%u body=%zu\n", resp.status_code, resp.body.size());
    }
    void on_complete(std::error_code ec) override
    {
        if (ec) { std::fprintf(stderr, "%s\n", ec.message().c_str()); }
    }
};

httpmech::RequestSpec spec;
spec.method = httpmech::Method::post;
spec.target = "/api/v1/report";
spec.headers.set("Content-Type", "application/json");
spec.body   = R"({"dev":"m1","p":123.4})";
cli->request(std::move(spec), std::make_shared<MyHandler>());   // 串行队列
```

```cpp
// ---- WebSocket ----
httpmech::WebSocketClientConfig wcfg;
wcfg.remote = {"192.168.3.10", 8080};
wcfg.path   = "/ws/events";

auto ws = httpmech::WebSocketClient::create(wcfg);

httpmech::WebSocketClientHandlers h;
h.on_open    = [](httpmech::WebSocketSession& s) { s.send_text("hi"); };
h.on_message = [](httpmech::WebSocketSession&, httpmech::span<const std::uint8_t> d, bool) {
    // 收到一条完整消息（已解掩码、已重组分片）
};
h.on_close   = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {};
h.on_error   = [](std::error_code ec) { std::fprintf(stderr, "%s\n", ec.message().c_str()); };

ws->connect(std::move(h));      // 异步：结果走回调
```

客户端的两条约定：

- **一条连接 + 串行队列**，不是连接池。同一时刻只有一笔请求在途，其余排队；
  需要并发就建多个 `HttpClient`。
- **不自动重连**。掉线后 `on_close` / `on_error` 各来一次，退避策略由应用决定。
- HTTP 的 `4xx/5xx` 交给应用（`on_response` 里的 `status_code`），**不**当成
  `ec`；`ec` 只表示"没拿到响应"（连不上、超时、协议错误）。

### 客户端

```cpp
// ---- HTTP ----
httpmech::ClientConfig ccfg;
ccfg.remote = {"192.168.3.10", 9898};

auto cli = httpmech::HttpClient::create(ccfg);

class MyHandler : public httpmech::ResponseHandler {
public:
    void on_response(const httpmech::HttpResponse& resp) override
    {
        // resp.body 在聚合模式下是完整的；视图只在回调内有效
        std::printf("status=%u body=%zu
", resp.status_code, resp.body.size());
    }
    void on_complete(std::error_code ec) override
    {
        if (ec) { std::fprintf(stderr, "%s
", ec.message().c_str()); }
    }
};

httpmech::RequestSpec spec;
spec.method = httpmech::Method::post;
spec.target = "/api/v1/report";
spec.headers.set("Content-Type", "application/json");
spec.body   = R"({"dev":"m1","p":123.4})";
cli->request(std::move(spec), std::make_shared<MyHandler>());   // 串行队列
```

```cpp
// ---- WebSocket ----
httpmech::WebSocketClientConfig wcfg;
wcfg.remote = {"192.168.3.10", 8080};
wcfg.path   = "/ws/events";

auto ws = httpmech::WebSocketClient::create(wcfg);

httpmech::WebSocketClientHandlers h;
h.on_open    = [](httpmech::WebSocketSession& s) { s.send_text("hi"); };
h.on_message = [](httpmech::WebSocketSession&, httpmech::span<const std::uint8_t> d, bool) {
    // 收到一条完整消息（已解掩码、已重组分片）
};
h.on_close   = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {};
h.on_error   = [](std::error_code ec) { std::fprintf(stderr, "%s
", ec.message().c_str()); };

ws->connect(std::move(h));      // 异步：结果走回调
```

客户端的两条约定：

- **一条连接 + 串行队列**，不是连接池。同一时刻只有一笔请求在途，其余排队；
  需要并发就建多个 `HttpClient`。
- **不自动重连**。掉线后 `on_close` / `on_error` 各来一次，退避策略由应用决定。
- HTTP 的 `4xx/5xx` 交给应用（`on_response` 里的 `status_code`），**不**当成
  `ec`；`ec` 只表示"没拿到响应"（连不上、超时、协议错误）。

---

## 契约（四条必须记住的）

1. **视图借用**：`HttpRequest` / `HttpResponse` 里的 `string_view` / `span`
   指向解析器缓冲，**回调返回即失效**。要跨回调保存请自己拷。
2. **回调串行**：同一连接的回调在该连接的 io 线程上串行，不会并发；
   不同连接可能并发，共享状态自己加锁。回调里不要阻塞 —— 耗时活儿丢自己的线程池
   （本库**不提供**纤程或业务线程调度）。
3. **必须收尾**：进了处理器就必须以 `out.end()` 或 `out.upgrade_websocket()` 结束，
   否则库补 `500` 并断开。
4. **`req.user` 是应用自留地**：库从不读取、不解释；`before` → 路由 → `after`
   传的是同一个 `HttpRequest`，所以这一格是通的。

---

## ⚠️ 底层 `Mechanism` 必须关闭 `linger_zero`

`netmech::Mechanism::Config::linger_zero = true` 会把 socket 的 `SO_LINGER`
设成 `{1, 0}`：**`close()` 立刻发 RST，丢掉还没进内核的数据**。

HTTP 的 `Connection: close` 恰恰是"写完响应立刻关"，于是最后一包被丢 ——
客户端拿不到响应，服务端也没有任何错误信号，只能抓包才能发现。

- netmech 的默认值是 `false`（正常四次挥手，数据保证送达），**保持默认即可**。
- httpmech 在 `start()` 里会检查：发现 `linger_zero == true` 直接返回
  `Errc::linger_zero_enabled`，错误信息里写了怎么改。
- 若某个进程里 Modbus 确实需要 `linger_zero = true`，请**给它单独的
  `Mechanism`**；实在要让 HTTP 共用，把 `ServerConfig::allow_linger_zero`
  置 true 并自行承担丢应答的风险。

端到端测试 `connection close` 专门验证这条：8 KiB 应答完整送达并以 FIN
（`recv() == 0`）收尾，而不是 RST。

---

## 构建

```bash
# netmech 放在同级目录（../netmech），或用 -DHTTPMECH_NETMECH_DIR=<路径>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # 或直接 ./build/bin/httpmech_ut
```

交叉编译 aarch64：

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=../netmech/cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
```

## 安装与在别的项目里使用

```bash
cmake --install ../netmech/build      # 先装依赖（本仓按家族约定只装自己）
cmake --install build                 # 默认前缀：Linux /usr/local、Windows C:/Program Files/httpmech
```

```cmake
find_package(httpmech REQUIRED)
target_link_libraries(app PRIVATE httpmech::httpmech)
```

装出来的结构：

```
<prefix>/include/httpmech/*.hpp                      # 只有本库的头
<prefix>/lib/libhttpmech.a                           # picohttpparser 已编进来
<prefix>/lib/cmake/httpmech/httpmech{Config,ConfigVersion,Targets}.cmake
<prefix>/share/doc/httpmech/LICENSE
<prefix>/share/doc/httpmech/LICENSE.picohttpparser   # MIT 要求保留的声明
```

**picohttpparser 不再单独出库**：它被编进 `libhttpmech.a`（符号加了 `httpmech_`
前缀，所以和 rtspmech 各内嵌一份也不会在链接期撞车）。使用方只需要链一个 `.a`。

---

## 目录结构

```
httpmech/
├── include/httpmech/      对外头文件
│   ├── httpmech.hpp       汇总
│   ├── common.hpp         Method / Status / Limits / Errc
│   ├── endpoint.hpp       端点（IP + 端口，不引 netmech）
│   ├── span.hpp           只读 span
│   ├── headers.hpp        HeadersView（借用）/ Headers（拥有）
│   ├── message.hpp        HttpRequest / HttpResponse + 代理头识别
│   ├── parser.hpp         增量 HTTP 解析器
│   ├── websocket.hpp      WebSocket 服务端（会话 / 配置 / 握手）
│   ├── server.hpp         HttpServer / ResponseWriter / RequestHandler
│   ├── router.hpp         Router：精确路由 + before/after
│   └── client.hpp         HttpClient / WebSocketClient
├── src/                   实现（parser / server / router / websocket / sha1+base64）
├── test/                  单元测试 + 端到端测试
├── example/hello_server.cpp
├── docs/design.md         概要设计
└── thirdparty/            picohttpparser（MIT，vendored）
```

## 依赖与许可

- 传输：[netmech](https://github.com/goldhalo/netmech)（自带 Asio，无需安装 Boost）
  - **头层面不依赖**：`include/httpmech/*.hpp` 里没有 `#include <netmech/...>`，
    只有 `create_with_mechanism()` 对 `netmech::Mechanism` 做前向声明。
    所以"只想用 httpmech"的项目不必准备 netmech 的头。
  - **链接层面需要**：`libhttpmech.a` 内部调用 netmech，最终要链上 `libnetmech.a`
    （`find_package(httpmech)` 会自动带出来）。
  - 一个进程里跑多个库（比如 httpmech + modbusmech）时，想让它们共用一份 io 线程池：
    先调一次 `netmech::initialize(n)`，或各自 `create_with_mechanism()` 传同一个
    `Mechanism`。不传的话每个库自己建一个（线程池仍是进程级共享的，只是 `Mechanism`
    各管各的监听器）。
- 解析：[picohttpparser](https://github.com/h2o/picohttpparser)（MIT，已 vendored）
- 本库：MIT，见 [LICENSE](LICENSE)
