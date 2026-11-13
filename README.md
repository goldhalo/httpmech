# httpmech

[![CI](https://github.com/goldhalo/httpmech/actions/workflows/ci.yml/badge.svg)](https://github.com/goldhalo/httpmech/actions/workflows/ci.yml)

**HTTP/1.1 与 WebSocket 的服务端协议层**，基于 [netmech](../netmech) 的 TCP。
TLS、HTTP/2 交给前面的 nginx，本库只处理它转过来的明文请求。
静态文件由可选的 `StaticFileHandler` 组件提供（分块投递、背压、任意大小文件，
见「静态文件服务」一节）；超高并发静态服务仍建议交给 nginx。

```
浏览器 ──HTTPS/WSS──► nginx ──HTTP/1.1 明文──► httpmech ──► netmech ──► TCP
                       (TLS 终结)                (本库)
```

- **公共头不依赖 netmech**：只用 httpmech 时不需要把 netmech 的头挂到 include 路径上
  （链接仍需要 `libnetmech.a`，见下方"依赖"）
- 自带路由、before/after 钩子、WebSocket（握手 + 帧层），**零外部依赖**
- C++17，不引 Boost / OpenSSL；WebSocket 握手用的 SHA-1 + Base64 也是自带的
- 单元 + 端到端测试，CI 覆盖 Linux（gcc/clang × Debug/Release）与 Windows MSVC

---

## 范围

| 做 | 不做 |
|---|---|
| HTTP/1.1 解析与序列化、chunked 请求体 | TLS / HTTPS（nginx 终结） |
| 路由（精确匹配）+ before/after 钩子 | HTTP/2、gRPC |
| WebSocket（握手、帧层、分片、ping/pong、close） | 业务纤程 / 线程池 |
| keep-alive、流式 body、资源上限 | 连接池 / 自动重连 / 重定向跟随 |
| `X-Real-IP` / `X-AGENT-SSL` 代理头识别（白名单） | gzip、multipart |
| 静态文件 + 通用大响应通道（分块投递、背压、单段/多段 Range） | 目录列表、gzip、sendfile |
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

### 静态文件服务（可选组件）

`StaticFileHandler` 是独立的 `RequestHandler`，既能接管全部请求，也能挂成路由表的
404 兜底。文件通过 `ResponseWriter::set_body_provider()` 分块投递：由写完成回调
驱动下一块，每连接最多 1 块（256 KiB）在途，**内存占用与文件大小无关**，
慢客户端靠 `ServerConfig::max_send_queue_bytes` 背压。

```cpp
httpmech::StaticFileConfig sf;
sf.root             = "./www";
sf.mount_point      = "/static";        // 段边界匹配，不会吞掉 /statics/x
sf.mime_types["md"] = "text/markdown; charset=utf-8";
sf.extra_headers.set("Cache-Control", "public, max-age=60");
// sf.max_file_bytes 默认 0 = 不限制；设成非 0 只是业务策略（超过回 403）

httpmech::ServerConfig cfg;
cfg.max_send_queue_bytes = 2 * 1024 * 1024;   // 可选：单连接发送队列水位
// 不需要调 max_buffered_response：它只管老的 write() 自动切 chunked 路径

auto router = std::make_shared<httpmech::Router>();
router->before(auth_hook);                    // 鉴权先于静态文件
auto files = httpmech::mount_static(router, std::move(sf));
if (!files->valid()) { /* files->error_message() */ }

auto srv = httpmech::HttpServer::create(cfg, router);
```

`mount_static()` 挂的是 `set_not_found()`，所以 `before` 钩子（鉴权）依然生效，
不会像 cpp-httplib 的 mount point 那样跳过路由前的处理。

支持：GET/HEAD、任意大小文件、`index.html`、目录 301、扩展名→MIME、
`ETag`/`Last-Modified`、`If-None-Match`/`If-Modified-Since`/304、
`If-Range`、单段与多段 `Range`（`multipart/byteranges`）/206/416（含大偏移、
段数上限 DoS 防护）、`Accept-Ranges`、405 `Allow`、慢客户端背压、keep-alive 复用。

不做：gzip / 静态压缩（零外部依赖约定，nginx 可代劳）、目录列表、真·sendfile /
mmap 零拷贝。分块投递已把内存占用降到与文件大小无关，再引入平台相关的映射
代码收益有限。provider 目前在该连接的 io 线程上同步 `read()` 一块；根目录若在
慢速网络盘上，接入方应自行把文件 IO 挪到业务线程池再经 `Conn::post()` 回投。

**通用大响应能力**：`set_body_provider()` 不只服务静态文件，大 JSON、日志流也能用
（`include/httpmech/response_body.hpp`）。`after` 钩子由库在响应真正结束时触发，
异步响应下 `ResponseSummary` 的 `body_bytes` / `elapsed` 仍是准确值。

路径安全按 cpp-httplib 的口径：先 percent-decode，再拒绝 `..`/空段/
尾点尾空格/NUL/反斜杠（Windows 还拒 `:`，防 NTFS 流 `a.txt::$DATA`），
最后 `weakly_canonical()` 与规范化根目录做前缀比较，挡住 symlink/junction 逃逸。

跑示例：

```bash
cmake --build build --target static_server
./build/bin/static_server 8080 ./www

curl -i  http://127.0.0.1:8080/
curl -I  http://127.0.0.1:8080/index.html
curl -i -H 'Range: bytes=0-9' http://127.0.0.1:8080/index.html
curl -i -H 'If-None-Match: <ETag>' http://127.0.0.1:8080/index.html
```

### JSON 配置驱动的静态服务器

`static_json_server` 只接受一个参数：JSON 配置文件路径。监听端口和全部静态
服务配置都从 JSON 读（vendored `thirdparty/nlohmann`，MIT 3.11.3，无额外依赖）。

```bash
cmake --build build --target static_json_server
./build/bin/static_json_server example/static_server.json
```

完整字段见 `example/static_server.json`，结构如下：

```json
{
  "server": { "host": "0.0.0.0", "port": 8080, "max_send_queue_bytes": 2097152 },
  "static": {
    "root": "./www",
    "mount_point": "/",
    "index_file": "index.html",
    "enable_range": true,
    "enable_etag": true,
    "mime_types": { "md": "text/markdown; charset=utf-8" },
    "headers": { "Cache-Control": "public, max-age=60" }
  }
}
```

| JSON 字段 | 对应 API | 默认 |
|---|---|---|
| `server.host` / `port` | `ServerConfig::local` | `0.0.0.0:8080`（port 0 = 系统分配，启动后打印实际端口） |
| `server.server_name` / `emit_date` | 同名 | `httpmech-static` / `true` |
| `server.max_send_queue_bytes` | 同名 | 2 MiB |
| `static.root` / `mount_point` / `index_file` | `StaticFileConfig` 同名 | `./www` / `/` / `index.html` |
| `static.default_content_type` / `max_file_bytes` | 同名 | `application/octet-stream` / 0（不限制） |
| `static.enable_range` / `enable_etag` / `redirect_directory` | 同名 | `true` |
| `static.mime_types` / `headers` | 同名映射，逐条覆盖 | 空 |

字段缺失用默认值；类型写错或文件打不开会打印 `config error: ...` 并以非零码退出。
`mime_types` 的 key 大小写不敏感、可带前导 `.`。

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
│   ├── response_body.hpp  DataSink / ContentProvider（异步大响应）
│   ├── router.hpp         Router：精确路由 + before/after
│   ├── static_file.hpp    StaticFileHandler（可选静态文件组件）
│   └── client.hpp         HttpClient / WebSocketClient
├── src/                   实现（parser / server / router / static_file / websocket / sha1+base64）
├── test/                  单元测试 + 端到端测试
├── example/               hello_server / static_server / static_json_server + JSON 配置示例
├── docs/design.md         概要设计
└── thirdparty/            picohttpparser、nlohmann/json（均 MIT，vendored）
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
- 示例的 JSON 配置：[nlohmann/json](https://github.com/nlohmann/json) 3.11.3
  （MIT，已 vendored 到 `thirdparty/nlohmann/`；只被 `example/static_json_server.cpp`
  使用，**不随库安装**、不影响 `libhttpmech` 的依赖面）
- 本库：MIT，见 [LICENSE](LICENSE)

## 动态库与 Runtime 注入

```bash
cmake -S . -B build-shared -DHTTPMECH_LIBRARY_TYPE=SHARED
```

- 公共 API 用 `HTTPMECH_API` 导出（Windows `dllexport/dllimport`，Linux hidden 可见性
  + `-Wl,--exclude-libs,ALL`）；
- SHARED 构建会把 netmech 也编成共享库（`libnetmech.so` / `netmech.dll`）：
  两端必须在同一份 io 运行时上，否则 Runtime 注入会跨副本使用。发布时两个
  共享库一起发，并保留 netmech 的 MIT 许可证声明。

```cpp
#include <netmech/runtime.hpp>

netmech::Runtime::Options o;
o.io_threads = 2;
auto rt = netmech::Runtime::create(o);

httpmech::ServerConfig cfg;
cfg.local   = {"0.0.0.0", 8080};
cfg.runtime = rt;                    // ServerConfig / ClientConfig / WebSocketClientConfig

auto srv = httpmech::HttpServer::create(cfg, handler);
```

不传 `runtime` 时行为与以前一致（进程级默认运行时）。
