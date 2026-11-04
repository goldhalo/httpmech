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

### D9 请求上留一格 `shared_ptr<void>`，响应给一个摘要

**问题**：`after` 钩子要记操作日志，需要"这次请求是谁、花了多久、回了多少"，
而这些东西分布在 before（鉴权结果）和响应阶段（状态码、字节数）。

**选择**：
- `HttpRequest::user`（`std::shared_ptr<void>`）—— 库**从不读取**，也不解释其语义。
  `before` 里 `req.user = ctx;` 挂上自己的上下文类，路由与 `after` 里
  `static_pointer_cast` 取回。三个回调传的是同一个 `HttpRequest` 对象。
- `after` 的第二个参数是 `ResponseSummary`（状态码 / body 字节数 / 耗时 /
  是否升级 / 是否复用），而不是完整的 `HttpResponse` —— 响应已经写出，
  视图可能失效，能可靠给的只有这几个标量。

**为什么不直接给一个 `SHttpResponse` 那样的响应对象**：本库的响应是
**写入器**（`ResponseWriter`），因为要支持流式 body、超限自动 chunked、
以及中途升级成 WebSocket —— 这些都不是"先攒一个值再发"能表达的。
值语义的响应体留给 v0.2 的客户端（`HttpResponse` 已经存在，是**解析结果**）。

---

## 5. 契约

### 生命周期

| 对象 | 有效期 |
|---|---|
| `HttpRequest` / `HttpResponse` 里的视图 | 回调进入 → 回调返回 |
| `ResponseWriter` | 同一次回调内，不能存 |
| `HttpRequest::user` | `before` 写入 → `after` 返回后随请求一起释放 |
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

### SO_LINGER

底层 `Mechanism` **不能**用 `Config::linger_zero = true`（会把 SO_LINGER 设成
`{1,0}`，`close()` 直接 RST 丢掉未发数据）。HTTP 的 `Connection: close`
正是"写完响应立刻关"，最后一包会被静默丢弃。

`HttpServer::start()` 会检查并返回 `Errc::linger_zero_enabled`；
确实需要 RST 的场景请给 HTTP 单独的 `Mechanism`，或显式置
`ServerConfig::allow_linger_zero = true` 承担风险。

### SO_LINGER

底层 `Mechanism` **不能**用 `Config::linger_zero = true`（会把 SO_LINGER 设成
`{1,0}`，`close()` 直接 RST 丢掉未发数据）。HTTP 的 `Connection: close`
正是"写完响应立刻关"，最后一包会被静默丢弃。

`HttpServer::start()` 会检查并返回 `Errc::linger_zero_enabled`；
确实需要 RST 的场景请给 HTTP 单独的 `Mechanism`，或显式置
`ServerConfig::allow_linger_zero = true` 承担风险。

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

## 9. 已确认的决定（2026-11-04）

| 问题 | 决定 |
|---|---|
| WebSocket 子协议 | v0.1 **原样回显**第一个 `Sec-WebSocket-Protocol`，不做子协议路由 |
| `after` 钩子入参 | 给 `ResponseSummary`（状态码 / body 字节数 / 耗时 / 是否升级 / 是否复用）；请求级上下文走 `HttpRequest::user`（`shared_ptr<void>`，库不解释） |
| 路由大小写 | **按规范**：路径大小写敏感、精确匹配 |
| `trusted_proxies` 默认值 | `{"127.0.0.1"}` —— 贴合"nginx 与进程同机"的实际部署，`real_ip()` 开箱即用 |

### 与参考实现的落地差异汇总

| 项 | 参考实现 | httpmech | 影响 |
|---|---|---|---|
| 请求上下文 | `SHttpRequest::userPtr` | `HttpRequest::user` | 同构 |
| 响应入参 | `SHttpResponse&`（可写值） | `ResponseWriter&` + `ResponseSummary` | 支持流式/chunked/升级 |
| 路由大小写 | 不敏感（`ci_map`） | 敏感（RFC 9110） | 迁移时统一大小写 |
| 代理头 | 无条件采信 | 白名单（默认仅本地回环） | 防客户端伪造身份 |

---

## 10. 实现进度

| 模块 | 状态 | 说明 |
|---|---|---|
| `common` / `headers` / `message` | ✅ | 方法/状态/头部容器/代理头解析 |
| `parser` | ✅ | 增量解析、chunked、pipelining、两种 body 模式；单元测试 100+ 断言 |
| `server` | ✅ | netmech 接线、keep-alive、超时清理、协议错误响应、`real_ip()` 白名单 |
| `router` | ✅ | 精确路由 + before/after + 405/Allow |
| `websocket` | ✅ | 握手（SHA-1 + Base64 自带）、帧层、分片、ping/pong、close 握手、空闲与 ping 定时 |
| `client` | ⏳ | v0.2 |

### 与设计稿的偏差（实现时发现，已在代码注释里标出）

| 设计稿写的 | 实际实现 | 原因 |
|---|---|---|
| 响应超 64 KiB 自动 chunked | 响应整块缓冲，按 `Content-Length` 发 | 自动 chunked 要求"第一次 write 就定死头部"，属于流式响应；v0.1 的接口（应用只在 `end()` 时收尾）表达不了。JSON 接口够用，流式下一阶段做 |

> 另两条曾经的偏差（`local()` 拿不到真实端口、`stop()` 只能 `close_all()`）
> **已经通过补 netmech 接口解决**：`Mechanism::unlisten()` /
> `listening_address()` / `listening_addresses()`。见下节。

### 验证

`test/test_main.cpp` 里除了解析器的单元测试，还有一条**端到端**用例：
起一个真服务端（127.0.0.1:18080），用阻塞 socket 发真请求，覆盖
GET / POST 带 body / 404 / 405+Allow / 同连接 keep-alive 复用。

另用 `curl` 手工验证过：
```
$ curl -s -i http://127.0.0.1:18081/api/status
HTTP/1.1 200 OK
Server: httpmech
Date: Wed, 04 Nov 2026 05:37:00 GMT
Content-Length: 32
Connection: keep-alive
Content-Type: application/json

{"ok":true,"path":"/api/status"}
```

---

## 11. 为此补的 netmech 接口

设计时发现两个能力缺口，netmech 是自己的项目，直接补上而不是在 httpmech 里绕：

| 新接口 | 解决的问题 |
|---|---|
| `Mechanism::unlisten(const Address&)` | 之前要停服务只能 `close_all()`，按下一个按钮，把同一 Mechanism 上的其它服务一起关掉。现在只停指定监听器，**不动已建立的连接**（关连接是业务层的事） |
| `Mechanism::listening_address(const Address&)` | `listen(..:0)` 后拿不到系统分配的端口。现在能查回来（`HttpServer::local()` 因此能返回真实端口） |
| `Mechanism::listening_addresses()` | 列出全部实际监听地址（诊断 / 测试） |

三条语义约定：

1. **端口 0 的入参要求"唯一匹配"**。同一 IP 上并存两个 `:0` 监听时，
   "问 :0 是哪个"没有唯一答案 → `listening_address()` 返回 `nullopt`，
   `unlisten()` 不动作。要精确操作就传实际地址。
2. **`unlisten()` 幂等**：没在监听也算成功。
3. **`unlisten()` 只停 accept，不关连接**。断开既有连接由调用方负责
   （httpmech 的 `stop()` 会逐个关自己的会话）。

netmech 侧新增 4 个用例（端口取回、只停目标监听、端口可复用、不误伤其它监听），
62 个用例 / 791 个断言全过。

---

## 12. WebSocket 实现记录（v0.1 完成）

自带 SHA-1（RFC 3174）+ Base64（RFC 4648），不引 OpenSSL。

| 项 | 处理 |
|---|---|
| 握手 | 校验 `Upgrade` / `Connection` / `Sec-WebSocket-Version: 13` / `Sec-WebSocket-Key`；回 `101` |
| 子协议 | 原样回显第一个 `Sec-WebSocket-Protocol` |
| 掩码 | 客户端→服务端**必须**带掩码，否则 `1002` 断开；服务端发出的帧不打掩码 |
| 长度 | 7 / 16 / 64 位三档都支持 |
| 分片 | `continuation` 拼接到 `max_message_bytes`，超了 `1009` |
| 控制帧 | `ping` 自动回 `pong`；控制帧不可分片、载荷 ≤125 |
| 关闭 | 收 `close` 回声同码再断；主动 `close` 等对端 2 秒超时兜底 |
| 保活 | `ping_interval` 主动 ping；`idle_timeout` 无帧即断 |

### 两个非得靠端到端测试才现形的 bug

1. **升级后 HTTP 解析器还在跑**。
   `pump()` 是"解析-派发"的循环，升级成 WS 之后 `finish_writer` 提前返回、
   没有 `reset()`，解析器停在 `done` 状态，`next()` 一直返回 `message`
   → **同一个请求被派发了 9 次**，每次都在升级。现象是客户端一个字节都收不到
   （服务端在死循环里）。修法：派发后 `if (m_is_ws) return;`。

2. **"写完就关"被实现成了"立刻关"**。
   `on_read` 里看到 `m_close_after_write` 就 `close_now()`，于是 close 帧
   还没写出去连接就被掐了 —— 对端看到的是 abnormal closure。
   修法：`on_read` 不关，交给 `on_write` 在写完成后再关。

这两条都是"看代码觉得没问题、跑起来立刻现形"的类型，也再次说明
端到端测试不能省。

### 测试

`websocket accept`：RFC 6455 §1.3 的官方例子（key → 固定 accept 值）。

`websocket e2e`：起真服务端 + 原始 socket，覆盖
握手 → on_open 主动推送 → 文本回显 → 二进制回显（16 位长度）→
ping/pong → 两帧分片拼成一条消息 → close 握手回声。

---

## 12. WebSocket 实现记录（v0.1 完成）

自带 SHA-1（RFC 3174）+ Base64（RFC 4648），不引 OpenSSL。

| 项 | 处理 |
|---|---|
| 握手 | 校验 `Upgrade` / `Connection` / `Sec-WebSocket-Version: 13` / `Sec-WebSocket-Key`；回 `101` |
| 子协议 | 原样回显第一个 `Sec-WebSocket-Protocol` |
| 掩码 | 客户端→服务端**必须**带掩码，否则 `1002` 断开；服务端发出的帧不打掩码 |
| 长度 | 7 / 16 / 64 位三档都支持 |
| 分片 | `continuation` 拼接到 `max_message_bytes`，超了 `1009` |
| 控制帧 | `ping` 自动回 `pong`；控制帧不可分片、载荷 ≤125 |
| 关闭 | 收 `close` 回声同码再断；主动 `close` 等对端 2 秒超时兜底 |
| 保活 | `ping_interval` 主动 ping；`idle_timeout` 无帧即断 |

### 两个非得靠端到端测试才现形的 bug

1. **升级后 HTTP 解析器还在跑**。
   `pump()` 是"解析-派发"的循环，升级成 WS 之后 `finish_writer` 提前返回、
   没有 `reset()`，解析器停在 `done` 状态，`next()` 一直返回 `message`
   → **同一个请求被派发了 9 次**，每次都在升级。现象是客户端一个字节都收不到
   （服务端在死循环里）。修法：派发后 `if (m_is_ws) return;`。

2. **"写完就关"被实现成了"立刻关"**。
   `on_read` 里看到 `m_close_after_write` 就 `close_now()`，于是 close 帧
   还没写出去连接就被掐了 —— 对端看到的是 abnormal closure。
   修法：`on_read` 不关，交给 `on_write` 在写完成后再关。

这两条都是"看代码觉得没问题、跑起来立刻现形"的类型，也再次说明
端到端测试不能省。

### 测试

`websocket accept`：RFC 6455 §1.3 的官方例子（key → 固定 accept 值）。

`websocket e2e`：起真服务端 + 原始 socket，覆盖
握手 → on_open 主动推送 → 文本回显 → 二进制回显（16 位长度）→
ping/pong → 两帧分片拼成一条消息 → close 握手回声。
