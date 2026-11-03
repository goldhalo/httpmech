# httpmech 概要设计（v0.1 草案）

> 状态：**接口草案，未实现**。本文件与 `include/httpmech/*.hpp` 一起评审；
> 评审通过后写 `src/`。

---

## 1. 定位与范围

**HTTP/1.1 与 WebSocket 的服务端协议层**，站在 netmech 的 TCP 之上。
部署形态固定为 **nginx 在前**：TLS、HTTP/2、限流、静态文件都由 nginx 负责，
本库只处理 nginx 转过来的明文 HTTP/1.1 与 WebSocket。

```
浏览器 ──HTTPS/WSS──► nginx ──HTTP/1.1 明文──► httpmech ──► netmech ──► TCP
                       (TLS 终结)                (本库)
```

### v0.1 做

| 项 | 说明 |
|---|---|
| HTTP/1.1 解析与序列化 | 增量、可喂 1 字节；chunked 请求体解码 |
| 路由 | (方法, 路径) 精确匹配 + before/after 钩子 |
| WebSocket | Upgrade 握手 + RFC 6455 帧层 + 分片/控制帧/关闭握手 |
| keep-alive | 连接复用、空闲超时、单连接请求数上限 |
| 流式 body | 上传/回灌大报文不占内存 |
| 反向代理识别 | `X-Real-IP` / `X-Real-PORT` / `X-AGENT-SSL`，且只采信白名单来源 |
| 资源上限 | 头大小/条数、body 大小、帧大小、单消息大小 |

### v0.1 不做

| 项 | 为什么 |
|---|---|
| **TLS / HTTPS** | nginx 终结；引 OpenSSL 会打破"零外部依赖"的家族约定 |
| **HTTP/2 / h2c** | nginx 可以把 H2 转成 H1 再转过来；自己实现 HPACK + 多路复用代价太大 |
| **gRPC** | 不做 |
| **业务纤程 / 线程池** | 参考实现用 boost fiber 跑业务；本库只给回调，耗时活儿由接入方自己丢线程池 |
| **HTTP / WebSocket 客户端** | 第一阶段不需要；接口形状先放着（`client.hpp`，标 v0.2） |
| 路径参数 `:id`、通配符、正则路由 | 十几条接口用精确匹配就够，参数解析是复杂度源头 |
| 静态文件服务 | nginx 的 `root` / `try_files` 比我们做得好 |
| gzip、multipart、cookie 罐 | 属应用语义或由 nginx 处理 |

---

## 2. 与现有实现（`wtoe::HttpProtocol`）的对照

参考实现约 13.8k 行，用 Boost.Beast + Boost.Asio + boost fiber，覆盖 H1/H2/gRPC/
WebSocket/SSL 双端。本节列出"留什么、砍什么"，作为本次设计的依据。

| 参考实现的组成 | httpmech v0.1 | 理由 |
|---|---|---|
| Boost.Beast 的 H1 解析/序列化 | 自研 `Parser`（picohttpparser 打底） | 家族约定：不引 Boost |
| Boost.Beast 的 WebSocket | 自研帧层（约 300 行） | 同上；帧层本身不复杂 |
| 多 `io_context` + `TaskFiberPool`（纤程业务线程） | 只用 netmech 的 io 线程，回调派发 | **明确不要纤程**；业务耗时活儿交给调用方的线程池 |
| SSL context / ALPN / `SslWebsocketSession` | 不做 | nginx 终结 TLS |
| HTTP/2（`Http2Session`、`H2Transport`、h2c 嗅探） | 不做 | nginx 可代劳 |
| gRPC（`GrpcFacade` / `GrpcFramer`） | 不做 | 无需求 |
| `IHttpServer::Attr` 建造者 + `setRoute` / `setWebsocketRoute` | `Router` 的 `route()` / `websocket()` | **保留这个形态**，接入方的用法不变 |
| `beforeCb` / `afterCb`（`std::list`） | `before()` / `after()` 钩子 | 语义一致：鉴权 + 操作日志 |
| `SHttpRequest` 值语义（`std::string` 全拷） | 视图（`string_view`） | 零拷贝；代价是"要留自己拷" |
| `X-Real-IP` / `X-Real-PORT` / `X-AGENT-SSL` | `real_ip()` / `real_port()` / `is_forwarded_https()` | **加了一层白名单**：只采信 `trusted_proxies` 里的来源，否则这些头是客户端随便写的 |
| 超时：连接 3s / 读 10s / 写 10s / keep-alive 60s | 见 §5 默认值表 | 沿用同一组经验值 |
| `IHttpClient` / `IWebsocketClient` | v0.2 预留 | 第一阶段不做 |

**一句话**：把参考实现里"协议 + 路由 + WebSocket"这三块留下，把"传输安全 +
H2 + gRPC + 纤程调度"四块交给 nginx 和调用方。

---

## 3. 模块划分

| 文件 | 职责 | 依赖 |
|---|---|---|
| `common.hpp` | Method / Version / Status / Limits / Errc | 标准库 |
| `span.hpp` | 只读 span（C++17 补丁） | 标准库 |
| `headers.hpp` | `HeadersView`（借用）/ `Headers`（拥有） | 标准库 |
| `message.hpp` | `HttpRequest` / `HttpResponse` 视图 + 代理头解析 | 上两个 |
| `parser.hpp` | `Parser`：增量 HTTP 解析 | + picohttpparser |
| `websocket.hpp` | `WebSocketSession` / `WebSocketConfig` / 握手工具 | 标准库（SHA-1/Base64 在 src/） |
| `server.hpp` | `HttpServer` / `ResponseWriter` / `RequestHandler` | + netmech |
| `router.hpp` | `Router`：精确路由 + before/after | 上两个 |
| `client.hpp` | **v0.2 预留**，不实现 | — |

---

## 4. 关键设计决策

### D1 解析器自带拼包缓冲
一条头必然跨 `read`。若视图指向两块内存，消息层就得拼接 —— 复杂度推给每个使用者。
改为解析器持有一块可增长缓冲，视图永远连续。代价是一次内存搬移，收益是所有调用点都简单。

### D2 body 两种模式**二选一**，不自动切换
`aggregate`（攒完再回调）/ `stream`（分块回调）由配置显式选。
不做"超过 N 字节自动转流式"——那样应用路径取决于报文大小，是最难复现的 bug。

### D3 消息是视图，不是对象
`HttpRequest` 里全是 `string_view` / `span`，指向解析器缓冲，**回调返回即失效**。
参考实现是全拷（`SHttpRequest` 里 `std::string url/body`）：省事但每请求多几次分配。
本库选零拷贝，把"拷贝"这件事留给需要的人显式做。

### D4 自带路由，但只做精确匹配
参考实现的 `setRoute(method, url, cb)` 是接入方已经习惯的形态，且这层不到 200 行。
**不做** `:id` 参数、通配符、正则 —— 那是复杂度的真正来源。

> 与参考实现的一处**行为差异**：它用 `ci_map`（大小写不敏感）存路由；
> HTTP 的 path 按 RFC 9110 是大小写敏感的，本库按规范精确匹配。
> 迁移时如果现有路由写成 `/API/xxx`，需要统一大小写。

### D5 WebSocket 与 HTTP 共用同一端口
nginx 转发的 WS 也是 HTTP 请求（带 `Upgrade`），所以**不需要协议嗅探**：
照常走 HTTP 解析，路由里判断是升级请求就调 `upgrade_websocket()`。
参考实现里的 `ProtocolSniffer`（h2c preface 嗅探）在这里用不上 —— 我们不做 H2。

### D6 服务端自己实现 WebSocket 帧层
不引 beast。帧格式（RFC 6455 §5）只有 7 种 opcode + 3 种长度前缀 + mask 位；
握手需要 SHA-1 + Base64，各约 100 行，一并写在 `src/`。

### D7 代理头默认不采信
`X-Real-IP` 这类头任何人都能伪造。只有连接来自 `ServerConfig::trusted_proxies`
（默认空 = 全不采信）时，`real_ip()` / `is_forwarded_https()` 才有值。
nginx 与进程同机时配 `{"127.0.0.1"}`。

### D8 不做纤程/业务线程池
回调在 io 线程上跑。耗时操作（查库、写文件、等下游）由接入方丢到自己的线程池，
库不提供调度器 —— 参考实现的 `TaskFiberPool` 是它自己业务模型的一部分，
不属于协议层。

---

## 5. 契约

### 生命周期

| 对象 | 有效期 |
|---|---|
| `HttpRequest` / `HttpResponse` 里的视图 | 回调进入 → 回调返回 |
| `ResponseWriter` | 同一次回调内，不能存 |
| `WebSocketSession` | 库持有 shared_ptr；`is_open()` 为 false 后 `send_*()` 全部返回 false |
| `Router` 的路由表 | `HttpServer::start()` 之前注册完，运行期只读、不加锁 |

### 线程

- 同一连接的回调**串行**（继承 netmech "1 io_context = 1 线程"）。
- 不同连接可能并发；共享状态自己加锁。
- `WebSocketSession::send_*()` 可在任意线程调（内部投递到 io 线程）。

### 超时（默认值取自参考实现的经验值）

| 名字 | 默认 | 作用 |
|---|---|---|
| `idle_timeout` | 60 s | keep-alive 空闲多久关连接（= 参考实现的 keepaliveTimeOut） |
| `header_timeout` | 10 s | 建连到"请求头收完"（= readTimeOut） |
| `body_idle_timeout` | 30 s | 流式 body 两次收数据的最大间隔 |
| `websocket.ping_interval` | 25 s | 主动 ping 间隔 |
| `websocket.idle_timeout` | 120 s | 这么久没收到任何帧就断 |

### 资源上限

| 项 | 默认 |
|---|---|
| 头字节数 | 32 KiB |
| 头条数 | 100 |
| 聚合 body | 8 MiB |
| WS 单帧 | 1 MiB |
| WS 单消息（跨分片） | 4 MiB |
| WS 发送队列 | 8 MiB |

### 错误处理

| 情况 | 行为 |
|---|---|
| 请求行/头语法错 | `400` + 断开 |
| 头超限 | `431` + 断开 |
| 聚合 body 超限 | `413` + 断开 |
| 路由未匹配 | `404`（可替换） |
| 路径匹配但方法不符 | `405` + `Allow` 头 |
| 处理器没调 `end()` | 库补 `500`，记 `response_incomplete`，断开 |
| `upgrade_websocket()` 失败（非升级请求） | 返回 false，由调用方写 `400` |
| WS 帧协议错 / 消息超限 | 回 close `1002` / `1009` 后断开 |

---

## 6. WebSocket 实现要点

| 项 | 处理 |
|---|---|
| 握手 | 校验 `Upgrade: websocket`、`Sec-WebSocket-Version: 13`、`Sec-WebSocket-Key` 存在；回 `101` + `Sec-WebSocket-Accept` |
| 子协议 | `Sec-WebSocket-Protocol` 原样回显第一个（由应用在 handlers 前决定，v0.1 直接回显） |
| 掩码 | 客户端→服务端**必须**带掩码，服务端→客户端**必须不带**；不合规即 `1002` 断开 |
| 分片 | `continuation` 帧拼接，累计到 `max_message_bytes` |
| 控制帧 | `ping` 自动回 `pong`；`close` 走关闭握手；控制帧不可分片、载荷 ≤125 字节 |
| 关闭 | 收 `close` 回 `close` 再断；主动关闭发 `close` 等回帧或超时 |
| UTF-8 | 文本帧可选严格校验（默认关，设备侧常发非标准字节） |
| 扩展 | 不协商任何扩展（`permessage-deflate` 不做） |

---

## 7. 未实现项（v0.1 明确不做）

| 项 | 现状 |
|---|---|
| `Expect: 100-continue` | 忽略该头，直接收 body（客户端 1 s 后自己发，能互通） |
| `Trailer` 头 | 解析后丢弃 |
| 响应压缩 | 不做（nginx 可代劳） |
| HTTP/2、gRPC、TLS | 不做（见 §1） |

---

## 8. 版本规划

| 版本 | 内容 |
|---|---|
| **v0.1** | HTTP/1.1 服务端 + 路由 + WebSocket 服务端 + 代理头识别 |
| v0.2 | `HttpClient` / `WebSocketClient`（设备主动出站时） |
| v0.3 | 视需要：连接数/请求数指标导出、`send_file()`（sendfile 或 mmap） |

---

## 9. 待确认

1. **`Sec-WebSocket-Protocol`** 子协议：v0.1 打算"原样回显第一个"，够不够？
   如果要按子协议选 handler，得让 `websocket(path, handler)` 能拿到请求头（现在能）。
2. **`after` 钩子的入参**：现在给 `(请求, 状态码)`。做操作日志是否需要 body 大小、
   耗时？加字段很便宜，但会改签名。
3. **路由表大小写**：确认要按规范大小写敏感（与参考实现的 `ci_map` 不同）。
4. **`trusted_proxies` 是否默认 `127.0.0.1`**？默认空最安全但会让现有接入方
   `real_ip()` 永远为空；默认本地回环更贴合"nginx 同机"的部署，但与安全直觉相反。
