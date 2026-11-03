# httpmech

HTTP/1.1 的**协议胶水层**：站在 [netmech](../netmech) 的 TCP 之上，把字节流切成
一条条 HTTP 消息，再把响应写回去。服务端与客户端共用同一套消息类型与解析器。

> **状态：v0.1 接口草案，尚未实现。**
> 本仓目前只有 `include/httpmech/*.hpp` 与设计文档，没有 `src/`。
> 目的是先把接口和契约定下来评审，实现随后补。
> 设计细节见 **[docs/design.md](docs/design.md)**。

---

## 定位

```
应用 ──► httpmech ──► netmech ──► TCP
         （协议）      （传输）
```

| 做 | 不做（v0.1） |
|---|---|
| HTTP/1.0、1.1 解析与序列化 | 路由表 / 中间件 |
| chunked 编解码 | TLS / HTTPS |
| keep-alive、空闲超时 | HTTP/2 |
| 流式 body（上传不占内存） | gzip 解压、multipart 语义 |
| 资源上限（头部/body/chunk） | 静态文件服务、cookie 罐 |

理由与取舍逐条写在 [docs/design.md](docs/design.md) 的「关键设计决策」一节。

---

## 接口速览

### 服务端

```cpp
#include <httpmech/httpmech.hpp>

class EchoHandler : public httpmech::RequestHandler {
public:
    void on_request(const httpmech::HttpRequest& req,
                    httpmech::ResponseWriter&    out) override
    {
        out.set_content_type("application/json");
        out.write(R"({"ok":true})");
        out.end();                      // 必须调用
    }
};

auto mech = netmech::Mechanism::create();

httpmech::ServerConfig cfg;
cfg.local = {"0.0.0.0", 8080};

auto srv = httpmech::HttpServer::create(mech, cfg, std::make_shared<EchoHandler>());
if (auto ec = srv->start()) { /* 处理错误 */ }
```

### 客户端

```cpp
httpmech::ClientConfig ccfg;
ccfg.remote = {"192.168.3.10", 9898};
auto cli = httpmech::HttpClient::create(mech, ccfg);

httpmech::RequestSpec spec;
spec.method = httpmech::Method::post;
spec.target = "/api/v1/report";
spec.headers.set("Content-Type", "application/json");
spec.body   = R"({"dev":"m1","p":123.4})";

cli->request(std::move(spec), std::make_shared<MyHandler>());   // 串行队列
```

### 解析器（单独用）

```cpp
httpmech::Parser p{httpmech::Parser::Role::server};
std::error_code ec;

// 可以任意切块喂，包括 1 字节
auto r = p.feed(httpmech::span{buf, n}, ec);
if (r == httpmech::Parser::Result::message) {
    use(p.request());     // 视图，回调返回即失效
    p.reset();            // 准备下一条（keep-alive）
}
```

---

## 契约（三条必须记住的）

1. **视图借用**：`HttpRequest` / `HttpResponse` 里的 `string_view` / `span`
   指向解析器缓冲，**回调返回即失效**。要跨回调保存请自己拷。
2. **回调串行**：同一连接的回调在该连接的 io 线程上串行，不会并发；
   不同连接可能并发，共享状态自己加锁。回调里不要阻塞。
3. **必须 `end()`**：进了 `on_request` / `on_body_end` 就必须以
   `out.end()` 收尾，否则库补 `500` 并断开。

---

## 构建

```bash
# netmech 放在同级目录（../netmech），或用 -DHTTPMECH_NETMECH_DIR=<路径>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

交叉编译 aarch64 时把 netmech 的 toolchain 文件一起用：

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=../netmech/cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
```

## 安装与在别的项目里使用

```bash
cmake --install build          # 默认前缀：Linux /usr/local、Windows C:/Program Files/httpmech
```

先装 netmech（本仓按家族约定**只装自己**）：

```bash
cmake -S ../netmech -B ../netmech/build && cmake --build ../netmech/build -j
cmake --install ../netmech/build
cmake --install build
```

然后：

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
│   ├── message.hpp        HttpRequest / HttpResponse
│   ├── parser.hpp         增量解析器
│   ├── server.hpp         HttpServer / RequestHandler / ResponseWriter
│   └── client.hpp         HttpClient / RequestSpec / ResponseHandler
├── docs/design.md         概要设计
├── cmake/                 包配置模板
├── thirdparty/            picohttpparser（MIT，vendored）
├── src/                   （待实现）
├── test/                  （待实现）
└── example/               （待实现）
```

## 依赖与许可

- 传输：[netmech](../netmech)（自带 Asio，无需安装 Boost）
- 解析：[picohttpparser](https://github.com/h2o/picohttpparser)（MIT，已 vendored）
- 本库：MIT，见 [LICENSE](LICENSE)
