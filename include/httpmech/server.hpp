// =============================================================================
//  httpmech - 服务端
// =============================================================================
//  文件    : httpmech/server.hpp
//  用途    : HttpServer —— 在 netmech 的 listen 之上，把每个连接包装成
//            "解析请求 → 回调应用 → 写响应 → 复用或关闭"。
//  依赖    : netmech（Mechanism / Linkable / Conn）+ 本库 parser。
//  编码    : UTF-8
//
//  ★ 定位：只做协议，不做框架
//    没有路由表、没有中间件、没有静态文件服务、没有模板。`on_request` 拿到的
//    是原始 path/query/headers/body，应用自己 if-else 或接自己的路由表。
//    理由：这些"框架功能"每加一个都要跟着一套配置与生命周期约定，
//    而通信管理机的 HTTP 面通常就十几个接口 —— 加框架的收益远小于复杂度。
//
//  ★ 线程模型
//    回调在**该连接所属的 io 线程**上串行执行（继承 netmech 的
//    "1 io_context = 1 线程"）。同一连接绝不会并发进 on_request；
//    不同连接可能并发，共享状态请自行加锁。
//    回调里**不要阻塞**：耗时活儿请把数据拷出来丢到业务线程。
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <netmech/common.hpp>
#include <netmech/mechanism.hpp>

#include <httpmech/common.hpp>
#include <httpmech/headers.hpp>
#include <httpmech/message.hpp>
#include <httpmech/parser.hpp>
#include <httpmech/span.hpp>
#include <httpmech/websocket.hpp>

namespace httpmech {

class HttpServer;
class Router;
namespace detail {
class ServerSession;   ///< 内部：一条连接，见 src/server.cpp
} // namespace detail

// =============================================================================
//  响应写入器
// =============================================================================

/// 应用填响应用的句柄。**只有 `on_request` / `on_body_end` 的回调期间有效**，
/// 别存下来跨回调用。
///
/// @par 必须 `end()`
/// 每次进入 `on_request` 都必须以 `end()` 结束。没调用 `end()` 就返回，
/// 库会补一个 `500`（并在日志里记一条 `Errc::response_incomplete`）——
/// 这是兜底，不是允许。
///
/// @par 大响应
/// `write()` 的字节先攒在内存里，攒够 `ServerConfig::max_buffered_response`
/// 就自动切成 `Transfer-Encoding: chunked` 流式发出。**应用代码不变**：
/// 一直 `write()` 然后 `end()` 即可。
class ResponseWriter {
public:
    ResponseWriter(const ResponseWriter&)            = delete;
    ResponseWriter& operator=(const ResponseWriter&) = delete;

    // --- 状态行 ---

    /// 设定状态码。重复调用以最后一次为准（已发头则失败，见 `headers_sent()`）。
    void set_status(Status s, std::string_view reason = {});

    /// 用原始数字设状态码（`Status` 枚举没覆盖的码走这个）。
    void set_status(std::uint16_t code, std::string_view reason = {});

    // --- 头部 ---

    /// 覆盖同名头部。
    void set_header(std::string_view name, std::string_view value);

    /// 追加同名头部（`Set-Cookie` 就得多条）。
    void add_header(std::string_view name, std::string_view value);

    /// 便捷：`set_header("Content-Type", v)`。
    void set_content_type(std::string_view v) { set_header("Content-Type", v); }

    // --- 消息体 ---

    /// 追加 body。返回 false 表示连接已不可写（对端断开 / 已 abort）。
    ///
    /// @note 0 字节也是合法的（`204` / `304` 就是空体）。
    bool write(span<const std::uint8_t> data);

    /// 便捷重载：文本 body。
    bool write(std::string_view text);

    /// 结束本次响应并发送。
    void end();

    /// 便捷：写 body + 结束。
    void end(span<const std::uint8_t> body) { if (!body.empty()) write(body); end(); }

    /// 便捷：`404 Not Found` + 一行文本。
    void end_status(Status s, std::string_view text = {});

    /// 头部是否已经发出（`write()` 超过缓冲上限、进入 chunked 之后为 true）。
    [[nodiscard]] bool headers_sent() const noexcept;

    /// 升级为 WebSocket。
    ///
    /// 由路由自己判断"这是不是升级请求"，然后调本函数。库会：
    ///   1. 校验 `Upgrade` / `Sec-WebSocket-Key` / `Sec-WebSocket-Version`
    ///   2. 回 `101 Switching Protocols`
    ///   3. 之后该连接不再走 HTTP 解析，改由 `handlers` 驱动
    ///
    /// @param[in] handlers 三条回调（open / message / close）。
    /// @param[in] cfg      WS 参数（默认取 `ServerConfig::websocket`）。
    /// @return true  已接受，连接进入 WS 模式；
    ///         false 不是合法的升级请求（或已发过响应），**调用方应自己写 400**。
    ///
    /// @warning 调用本函数后不要再写任何 HTTP 响应 —— `end()` 会被忽略。
    bool upgrade_websocket(WebSocketHandlers handlers,
                           const WebSocketConfig& cfg = WebSocketConfig{});

    /// 是否已经升级为 WebSocket。
    [[nodiscard]] bool upgraded() const noexcept;

    /// 当前设定的状态码（after 钩子里记访问日志用）。
    [[nodiscard]] std::uint16_t status_code() const noexcept;

    /// 已写入的 body 字节数。
    [[nodiscard]] std::size_t body_bytes() const noexcept;

    /// 直接断连，不发响应。用于"这个连接我不想再理了"。
    void abort() noexcept;

private:
    friend class HttpServer;
    friend class detail::ServerSession;
    struct Impl;
    explicit ResponseWriter(Impl* impl) noexcept : m_impl(impl) {}
    Impl* m_impl = nullptr;
};

// =============================================================================
//  响应摘要
// =============================================================================

/// 一次响应的结果摘要，交给 `after` 钩子（记访问/操作日志用）。
///
/// @note 这里刻意**不是**完整的 `HttpResponse`：`after` 在响应已经写出、
///       视图可能已失效之后才调用，能可靠给出的是这几个标量。
///       需要更多信息（URL、token、用户）请走 `HttpRequest::user`。
struct ResponseSummary {
    std::uint16_t status      = 0;      ///< 实际发出的状态码（兜底的 500 也算）
    std::size_t   body_bytes  = 0;      ///< 写入的 body 字节数
    bool          upgraded    = false;  ///< 是否升级成了 WebSocket（此时 status = 101）
    bool          keep_alive  = false;  ///< 这个连接是否被复用
    std::chrono::milliseconds elapsed{0};   ///< 从收到请求头到响应写完
};

// =============================================================================
//  应用回调
// =============================================================================

/// 业务侧实现本接口，交给 `HttpServer::create()`。
///
/// @par 生命周期
/// server 用 `std::shared_ptr` 持有，直到 `stop()` 之后连接全部回收。
///
/// @par 两种模式实现哪个
/// - `BodyMode::aggregate`（默认）：只实现 `on_request`，body 已完整。
/// - `BodyMode::stream`：实现 `on_headers` / `on_body_data` / `on_body_end`，
///   **不要**再在 `on_headers` 里写响应 —— 响应应在 `on_body_end` 里写。
class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    /// 一条请求处理完毕（聚合模式）。
    ///
    /// @param[in,out] req 请求视图，回调返回后失效。
    ///                    传**非 const 引用**是为了让处理器能挂
    ///                    `req.user`（见 `HttpRequest::user`），其它字段
    ///                    视同只读。`before` 钩子与处理器拿到的是同一个对象。
    /// @param[in]     out 响应写入器，**必须** 以 `out.end()` 结束。
    virtual void on_request(HttpRequest& req, ResponseWriter& out) = 0;

    // --- 以下是流式模式的可选实现（聚合模式下不会被调用）---

    /// 头部已就绪，body 还没来。
    ///
    /// @return true  继续接收 body（会依次来 `on_body_data`）；
    ///         false 立刻回 `413 Payload Too Large` 并断开（省带宽的拒收）。
    virtual bool on_headers(HttpRequest& req, ResponseWriter& out)
    {
        (void)req; (void)out;
        return true;
    }

    /// 一块 body。`data` 指向解析器缓冲，返回即失效，要留请拷。
    virtual void on_body_data(HttpRequest& req, span<const std::uint8_t> data)
    {
        (void)req; (void)data;
    }

    /// body 收完，在这里写响应（必须有 `out.end()`）。
    virtual void on_body_end(HttpRequest& req, ResponseWriter& out)
    {
        (void)req; (void)out;
    }

    /// 协议错误。库已决定**关闭该连接**，这里只用于记录/计数。
    ///
    /// @param[in] req 出错时的请求（可能只解析出一半，字段可能为空）。
    virtual void on_protocol_error(const HttpRequest& req, std::error_code ec)
    {
        (void)req; (void)ec;
    }
};

// =============================================================================
//  服务端
// =============================================================================

struct ServerConfig {
    /// 监听地址。端口 0 表示由系统分配，起来后用 `local()` 取实际端口。
    netmech::Address local{"0.0.0.0", 0};

    Limits            limits{};
    Parser::BodyMode  body_mode = Parser::BodyMode::aggregate;

    /// keep-alive 空闲超时：连接上没有请求在途、且这么久没新数据就关。
    std::chrono::milliseconds idle_timeout{60'000};

    /// 从连接建立到"请求头收完"的超时。防 slowloris（一点点发头耗住连接）。
    std::chrono::milliseconds header_timeout{10'000};

    /// 流式 body 接收期间，两次收到数据的最大间隔。
    std::chrono::milliseconds body_idle_timeout{30'000};

    /// 单连接最多处理多少个请求（默认 1000）。达到后优雅关闭（回完最后一个
    /// 响应、发 `Connection: close`）。防止长连接永久占用资源。
    std::size_t max_requests_per_connection = 1000;

    /// 响应在内存里攒多少字节之后转 chunked 流式（默认 64 KiB）。
    std::size_t max_buffered_response = 64 * 1024;

    /// listen 的 backlog。
    std::uint32_t backlog = 128;

    /// 写进 `Server:` 响应头的值。空字符串表示不写这个头。
    std::string server_name = "httpmech";

    /// 是否在每个响应里写 `Date:`。需要精确时间时必须开（HTTP/1.1 建议）。
    bool emit_date = true;

    /// WebSocket 参数（`upgrade_websocket()` 未显式给 cfg 时用这份）。
    WebSocketConfig websocket{};

    /// 受信任的反向代理地址。**只有来自这些地址的连接**，`X-Real-IP` /
    /// `X-Forwarded-*` / `X-AGENT-SSL` 才会被采信，否则视为普通客户端伪造的头。
    ///
    /// 支持两种写法：`"127.0.0.1"`（精确）与 IPv4 前缀 `"10.0.0.0/8"`。
    ///
    /// @note 默认 `{"127.0.0.1"}` —— 贴合"nginx 与进程同机"这个实际部署形态，
    ///       使 `real_ip()` 开箱即用。若要更严格（例如 nginx 在另一台机器上、
    ///       或压根没有代理），显式改成对应的代理地址，或置空表示**完全不采信**。
    std::vector<std::string> trusted_proxies{"127.0.0.1"};
};

/// HTTP/1.1 服务端。
///
/// @par 用法
/// @code
/// auto mech = netmech::Mechanism::create();
/// auto srv  = httpmech::HttpServer::create(mech, cfg, std::make_shared<MyHandler>());
/// if (auto ec = srv->start()) { ... }
/// // ... 进程运行中 ...
/// srv->stop();
/// @endcode
///
/// @par 生命周期
/// 通过 `create()` 得到 shared_ptr。`stop()` 之后 server 仍存活，
/// 但不再接受新连接，既有连接在回调返回后被关闭。
class HttpServer : public std::enable_shared_from_this<HttpServer> {
public:
    struct Impl;   ///< 内部实现（不透明，定义在 src/server.cpp）

    /// 创建（不监听）。同一个 `Mechanism` 可以挂多个 HttpServer。
    ///
    /// @param[in] mech    netmech 运行时（必须有存活线程池）。
    /// @param[in] cfg     服务端配置。
    /// @param[in] handler 业务回调；server 持有 shared_ptr。
    ///                    **不能为空**，为空时返回 nullptr。
    static std::shared_ptr<HttpServer> create(std::shared_ptr<netmech::Mechanism> mech,
                                              ServerConfig                       cfg,
                                              std::shared_ptr<RequestHandler>    handler);

    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// 开始监听。重复调用返回 `Errc::already_started`。
    [[nodiscard]] std::error_code start();

    /// 停止监听并关闭所有连接。幂等，可在任意线程调用。
    ///
    /// @warning 不要在本 server 的回调里调用（会把当前连接也关掉）。
    /// @warning v0.1 的实现走的是 `Mechanism::close_all()`，会关掉该 Mechanism
    ///          上**所有**监听器与连接。如果一个 Mechanism 上挂了多个 server，
    ///          请各自用独立的 Mechanism。
    void stop() noexcept;

    /// 实际监听地址。
    ///
    /// @note v0.1 返回的是 `cfg.local` 原值：netmech 目前没有暴露"bind 之后
    ///       实际端口"，所以 **`cfg.local.port` 请显式给一个非零端口**。
    [[nodiscard]] netmech::Address local() const;

    [[nodiscard]] bool        running() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;

    /// 累计处理过的请求数 / 协议错误数（监控用）。
    [[nodiscard]] std::uint64_t requests_total() const noexcept;
    [[nodiscard]] std::uint64_t protocol_errors_total() const noexcept;

private:
    HttpServer();

    std::shared_ptr<Impl> m_impl;

    friend struct Impl;
};

} // namespace httpmech
