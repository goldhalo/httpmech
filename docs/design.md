# httpmech 概要设计（v0.1 草案）

> 状态：**接口草案，未实现**。本文件与 `include/httpmech/*.hpp` 一起评审；
> 评审通过后再写 `src/`。

---

## 1. 定位

**HTTP/1.1 的协议胶水层。** 站在 netmech 的 TCP 之上，负责把字节流切成
一条条 HTTP 消息、把响应写回去；不做业务、不做框架。

与家族里其它库的分工：

```
            ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
  应用层     │ modbusmech  │  │  rtspmech   │  │   rtpmech   │  │  httpmech   │
            └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
                   │                │                │                │
            ┌──────┴────────────────┴────────────────┴────────────────┴──────┐
  传输层     │                    netmech（TCP / UDP / 定时器）              │
            └───────────────────────────────────────────────────────────────┘
```

只依赖 netmech，不依赖 rtspmech / rtpmech（三者是平级的协议层）。

### 做

| 项 | 说明 |
|---|---|
| 请求 / 响应解析 | HTTP/1.0、HTTP/1.1；增量、可喂 1 字节 |
| chunked | 请求体解码、响应体编码 |
| keep-alive | 连接复用、空闲超时、单连接请求数上限 |
| 流式 body | 上传/回灌大报文不占内存 |
| 服务端 | 监听、每连接会话、回调式处理 |
| 客户端 | 单连接串行队列、keep-alive 复用 |
| 资源上限 | 头大小/条数、body 大小、chunk 行长度 |

### 不做（v0.1）

| 项 | 为什么 | 以后 |
|---|---|---|
| 路由 / 中间件 | 十几个接口用 if-else 更直白；加框架要带一整套配置与生命周期约定 | 不做 |
| TLS / HTTPS | 要引 OpenSSL，编译与部署成本翻倍；板端 HTTP 一般在内网 | v0.3 评估 |
| HTTP/2 | 多路复用 + HPACK，工作量比 HTTP/1.1 大数倍 | 不做 |
| WebSocket 升级 | `Upgrade` 头保留可识别，但不实现握手后的帧协议 | 按需 |
| gzip 解码 | 需要 zlib；客户端不主动发 `Accept-Encoding` 即可绕开 | 按需 |
| multipart 编解码 | 属应用语义；文件上传用流式 body 自己切 | 不做 |
| 静态文件服务 | 属应用；`sendfile` 与 netmech 的 `write(ConstBuf)` 模型也不匹配 | 不做 |
| 自动重定向 / cookie 罐 | 客户端保持"一层薄封装"，策略交应用 | 不做 |

---

## 2. 模块划分

| 文件 | 职责 | 依赖 |
|---|---|---|
| `common.hpp` | Method / Version / Status / Limits / Errc | 标准库 |
| `span.hpp` | 只读 span（C++17 补丁，与 rtspmech 同一份） | 标准库 |
| `headers.hpp` | `HeadersView`（借用）+ `Headers`（拥有） | 标准库 |
| `message.hpp` | `HttpRequest` / `HttpResponse` 视图 | 上两个 |
| `parser.hpp` | `Parser`：增量解析，服务端/客户端共用 | + picohttpparser |
| `server.hpp` | `HttpServer` / `RequestHandler` / `ResponseWriter` | + netmech |
| `client.hpp` | `HttpClient` / `RequestSpec` / `ResponseHandler` | + netmech |
| `httpmech.hpp` | 汇总 | — |

`parser.hpp` 之前的部分都不碰 netmech —— 只写单元测试或做报文工具时，
只包含前四个头就够，不需要链传输层。

---

## 3. 一次请求的数据流（服务端）

```
netmech::Conn.on_read(ec, buf, n)
        │
        ▼
    Parser::feed(span{buf.data, n})
        │  攒够一条头 ──► Result::headers ──► on_headers()  [stream 模式]
        │  攒够整条   ──► Result::message ──► on_request()  [aggregate 模式]
        │  一块 body  ──► Result::body_chunk ──► on_body_data()
        │  收完       ──► Result::message_end ──► on_body_end()
        ▼
    ResponseWriter（应用填状态/头/body）
        │  end()
        ▼
    序列化成字节 → netmech::Conn::write(ConstBuf)
        │
        ▼
    决定复用还是关闭：keep_alive && 未达 max_requests_per_connection
```

要点：**读 → 解析 → 回调 → 写**全在同一条 io 线程上，没有跨线程投递，
也没有锁。

---

## 4. 关键设计决策

### D1 解析器自带拼包缓冲

**问题**：一条头部可能跨两次 `read`（这次读到 `"Content-Ty"`，下次才是
`"pe: ...\r\n\r\n"`）。若视图直接指向两次不同的接收缓冲，消息层就得拼接 ——
等于把复杂度推给每个使用者。

**选择**：`Parser` 内部持有一块可增长的缓冲，视图永远指向它。

**代价**：缓冲满时要搬一次内存。**收益**：所有使用者的代码都简单，
`string_view` 天然连续。这是"库承担复杂度"的典型取舍。

### D2 两种 body 模式**二选一**，不自动切换

`aggregate`（攒完再回调）与 `stream`（分块回调）由 `Parser::Config`
显式选择。

**为什么不做"超过 N 字节自动转流式"**：那样应用的处理路径取决于报文大小 ——
同一段代码在小请求上走 A 分支、大请求上走 B 分支，是最难复现的一类 bug。
宁可让应用自己决定"这个接口会不会收大 body"。

### D3 消息是视图，不是对象

`HttpRequest` 里全是 `string_view` / `span`，指向解析器缓冲，
**在回调返回时失效**。

**理由**：一条请求最常见的路径是"看一眼就回"。全拷一遍等于每请求多一次
堆分配 + memcpy。要跨回调保存就自己 `std::string{req.target}` —— 让"拷贝"
这件事在代码里显式出现。

**代价**：唯一的坑是"存了 view 忘了拷"，会在文档与头文件里三处标注。

### D4 `ResponseWriter` 先缓冲，超限自动转 chunked

应用永远只写 `write(...)` + `end()`，不关心是否 chunked。攒够
`max_buffered_response`（默认 64 KiB）就自动开始分块发送。

**理由**：让"小响应用 Content-Length、大响应自动流式"对应用透明。
**代价**：`Content-Length` 只在"整条都攒下"时才写得出来；超过阈值后
报文变 chunked，客户端要能解 —— HTTP/1.1 客户端必须能解，这不是问题。

### D5 客户端：单连接 + 串行队列，不做连接池

**理由**：通信管理机的 HTTP 出站是"往平台上报一包 JSON"，QPS 个位数。
连接池要处理空闲回收、并发上限、DNS 变更、故障摘除 —— 收益接近零。
需要并发时建多个 `HttpClient` 即可，各自独立连接。

**代价**：并发请求要自己建多个 client。可接受。

### D6 不做路由

`on_request` 给原始的 `path` / `query` / `method`，路由由应用决定。
一个几十行的 `if (path == "/api/v1/x")` 比引入路由表 + 参数解析 + 注册
顺序更可控。

### D7 keep-alive 由库管，pipelining 串行处理

连接复用、`Connection` 头、单连接请求数上限都在库内。
**pipelining**（一个连接上连发多个请求）不做乱序并发：解析器缓冲里若还有
下一条的字节，等当前响应写完再解析下一条。响应顺序因此永远是请求顺序 ——
HTTP/1.1 本来就要求这样，实现简单且不会错序。

### D8 v0.1 不做 TLS

TLS 需要 OpenSSL（或 schannel），会把"零外部依赖、`git clone` 即可构建"
这个家族约定打破。内网 HTTP 场景（设备配置页、平台上报走内网）暂不需要。
v0.3 若要加，做法是给 netmech 的连接加一层 `ssl::stream`，httpmech 的
解析部分完全不动。

---

## 5. 契约（使用前必读）

### 生命周期

| 对象 | 有效期 |
|---|---|
| `HttpRequest` / `HttpResponse` 里的所有视图 | 回调进入 → 回调返回 |
| `ResponseWriter` | 同一次回调内，不能存 |
| `Headers::view()` 的结果 | 到下次修改该 `Headers` 为止 |
| `HttpServer` / `HttpClient` | 由 `shared_ptr` 持有，`stop()`/`close()` 后仍可读统计 |

### 线程

- 同一连接的回调**串行**（继承 netmech "1 io_context = 1 线程"）。
- 不同连接可能并发；共享状态自己加锁。
- 回调里不要阻塞；耗时活儿把数据拷出来丢业务线程。

### 超时

| 名字 | 默认 | 作用 |
|---|---|---|
| `idle_timeout` | 60 s | keep-alive 空闲多久关连接 |
| `header_timeout` | 10 s | 建连到"请求头收完"，防 slowloris |
| `body_idle_timeout` | 30 s | 流式 body 两次收数据的最大间隔 |
| `request_timeout`（client） | 10 s | 发出到收完响应 |
| `connect_timeout`（client） | 3 s | TCP 建连 |

### 资源上限

`Limits` 里四项全部有非零默认值：头字节 32 KiB、头条数 100、
body 8 MiB（聚合模式）、chunk 行 1 KiB。

### 错误处理策略

| 情况 | 库的行为 |
|---|---|
| 请求行/头部语法错 | 回 `400`，`Connection: close`，断开 |
| 头超限 | 回 `431`，断开 |
| 聚合模式 body 超限 | 回 `413`，断开 |
| 不支持的 `Transfer-Encoding` | 回 `501`，断开 |
| `Content-Length` 与 `Transfer-Encoding` 并存 | 回 `400`（防请求走私），断开 |
| 应用没调 `end()` | 库补 `500`，记 `Errc::response_incomplete`，断开 |
| 传输层错误 | 直接断，不回响应（回不了） |

---

## 6. 未实现项（v0.1 明确不做，代码里留了位置）

| 项 | 现状 |
|---|---|
| `Expect: 100-continue` | 忽略该头，直接收 body。客户端会在 1 s 后自己开始发，能互通，大 body 多等 1 秒 |
| `Trailer` 头 | 解析后丢弃 |
| 响应压缩 | 不做；`Content-Encoding` 由应用自己设（库不编不编） |
| Upgrade / WebSocket | `Upgrade` 头可读，握手后协议不实现 |

---

## 7. 与 rtspmech 的关系

**不依赖，但骨架同构**。RTSP 与 HTTP 都是"文本头 + 可选 body"，
`RtspSession` 的"读 → 解析 → 弱回调 → 写"结构可以直接照搬。
不能复用的原因：

| | RTSP | HTTP |
|---|---|---|
| body | 基本没有，有也很小 | 可能很大，必须流式 |
| 连接复用 | 一条连接一个会话，长驻 | 一次连接多个请求（keep-alive） |
| 特殊帧 | `$` 开头的 interleaved 二进制帧 | 无 |
| chunked | 无 | 必需 |

所以是"同一套骨架，不同的消息状态机"，各自实现更清楚。

---

## 8. 待确认

1. **`ResponseWriter` 是否要提供 `send_file(path)`**？会牵出 `sendfile`/
   mmap 与 netmech `write()` 的适配，v0.1 先不做。
2. **`HttpClient` 是否需要自动重连**（对端 RST 后下次请求自动重连）？
   当前设计是"下一个请求发现连接没了就重连一次"，不额外做保活探测。
3. **`Date:` 头用什么时钟**？用系统时间（`std::chrono::system_clock`），
   设备上时钟不准时 `Date` 会错 —— 是否要允许应用注入一个时间源？
4. 是否需要 **`Server: httpmech/x.y` 之外的自定义横幅**（安全考虑常要求隐藏）。
