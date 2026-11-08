// =============================================================================
//  httpmech - 客户端（HTTP 与 WebSocket）
// =============================================================================
//  文件    : httpmech/client.hpp
//  用途    : HttpClient —— 一条到目标地址的连接，串行发请求、收响应。
//  依赖    : 标准库 + 本库 parser。**公共头不引 netmech** —— 需要共享同一份
//            io 池时才用 create_with_mechanism()。
//  编码    : UTF-8
//
//  ★ 状态：v0.2 已实现
//    服务端是 v0.1 的必需项；客户端用于设备主动往平台拉/推的场景
//    （上报 JSON、订阅 WS 推送）。与 ServiceConfig 一样，客户端也不做 TLS ——
//    需要 HTTPS 的走 nginx 或由应用自己加一层。
//
//  ★ 为什么是"一条连接 + 串行队列"，不是连接池
//    通信管理机里的 HTTP 出站基本是"往平台上报一包 JSON"，QPS 个位数。
//    连接池要处理空闲回收、并发上限、DNS 变更、故障摘除 —— 一整套东西，
//    收益在这一档流量下接近于零。v0.1 只做一条连接排队发；真需要并发时，
//    应用建多个 HttpClient 即可（各自独立连接），不需要库来管。
//
//  ★ 不做的事
//    · 不自动跟随重定向（3xx 原样交给应用，`Location` 已给）
//    · 不做 POST 的 multipart 编码、不做 gzip 解码（`Accept-Encoding` 别自己填）
//    · 不做 cookie 罐
//    · 不做 HTTPS（TLS 需要 OpenSSL，见 docs/design.md「未实现项」）
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <httpmech/common.hpp>
#include <httpmech/endpoint.hpp>
#include <httpmech/headers.hpp>
#include <httpmech/message.hpp>
#include <httpmech/parser.hpp>
#include <httpmech/span.hpp>
#include <httpmech/websocket.hpp>

namespace netmech {
class Mechanism;   // 前向声明（仅 create_with_mechanism 用）
class Runtime;     // 前向声明（ClientConfig / WebSocketClientConfig::runtime）
} // namespace netmech

namespace httpmech {

class HttpClient;

// =============================================================================
//  请求描述（拥有内存）
// =============================================================================
//
//  与服务端的 HttpRequest 相反：那里是借用视图（不拥有），这里是**拥有**的。
//  因为一次请求的各个字段来自应用的不同字符串，拼出来就要活到响应回来。

struct RequestSpec {
    Method      method  = Method::get;
    Version     version = Version::http_1_1;

    /// 请求目标，原样写进请求行。通常是 `"/path?query"`。
    /// 只写路径即可 —— `Host` 头由客户端按 `ClientConfig::remote` 自动补。
    std::string target = "/";

    /// 额外头部。`Host` / `Content-Length` / `Connection` 由库管理，
    /// 这里设了也会被覆盖（避免写出自相矛盾的报文）。
    Headers headers;

    /// 消息体。空表示没有 body（不发 `Content-Length: 0`，除非方法需要）。
    std::string body;
};

// =============================================================================
//  响应回调
// =============================================================================

class HTTPMECH_API ResponseHandler {
public:
    virtual ~ResponseHandler() = default;

    /// 响应头就绪。
    ///
    /// @param[in] resp `body_streamed == false` 时 body 已经完整；
    ///                 为 true 时 body 会在随后通过 `on_body_data()` 分块给出。
    /// @warning `resp` 里的视图在**本回调返回后**失效。
    virtual void on_response(const HttpResponse& resp) { (void)resp; }

    /// 流式 body 分块（只用 `Parser::BodyMode::stream` 时才有）。
    virtual void on_body_data(span<const std::uint8_t> data) { (void)data; }

    /// 本次请求结束 —— **恰好回调一次**，无论成功失败。
    ///
    /// @param[in] ec 空表示完整收到响应；非空可能是
    ///               `httpmech::Errc::*`（协议错误）、netmech 的传输错误，
    ///               或 `Errc::request_timeout`。
    ///
    /// @note 在这里可以安全地发起下一个请求（队列里还有的话会自动发）。
    virtual void on_complete(std::error_code ec) = 0;
};

// =============================================================================
//  客户端
// =============================================================================

struct ClientConfig {
    /// 注入 netmech 的 io 运行时（`netmech/runtime.hpp`）。空 = 默认运行时。
    std::shared_ptr<netmech::Runtime> runtime;
    /// 目标地址（含端口）。`Host:` 头按它填（`IP:port`，端口 80 时省略）。
    Endpoint remote{"127.0.0.1", 80};

    Limits           limits{};
    Parser::BodyMode body_mode = Parser::BodyMode::aggregate;

    std::chrono::milliseconds connect_timeout{3'000};
    std::chrono::milliseconds request_timeout{10'000};   ///< 从发出到收完响应
    std::chrono::milliseconds idle_timeout{60'000};      ///< keep-alive 空闲多久断开

    /// 是否复用连接（HTTP/1.1 keep-alive）。false 时每个请求用完即关。
    bool keep_alive = true;

    /// 排队上限。超过时 `request()` 直接返回 false（背压，而不是无限攒）。
    std::size_t max_pending_requests = 8;

    /// 写进 `User-Agent:`。空字符串表示不写。
    std::string user_agent = "httpmech";

    /// 单次连接最多发多少个请求后主动断开（防止对端会话老化后突然 RST）。
    std::size_t max_requests_per_connection = 100;
};

/// HTTP/1.1 客户端（单连接 + 串行队列）。
///
/// @par 用法
/// @code
/// httpmech::ClientConfig cfg;
/// cfg.remote = {"192.168.3.10", 9898};
/// auto cli = httpmech::HttpClient::create(mech, cfg);
///
/// httpmech::RequestSpec spec;
/// spec.method = httpmech::Method::post;
/// spec.target = "/api/v1/report";
/// spec.headers.set("Content-Type", "application/json");
/// spec.body   = R"({"k":1})";
///
/// cli->request(std::move(spec), std::make_shared<MyHandler>());
/// @endcode
///
/// @par 线程模型
/// `request()` 可在任意线程调用；回调在 io 线程上串行执行。
/// 同一时刻**只有一个请求在途**，其余按提交顺序排队。
class HTTPMECH_API HttpClient : public std::enable_shared_from_this<HttpClient> {
public:
    struct Impl;   ///< 内部实现（不透明，定义在 src/client.cpp）

    /// 创建（不连接）。首次 `request()` 时自动连接。io 运行时由本库内部创建。
    static std::shared_ptr<HttpClient> create(ClientConfig cfg);

    /// 复用外部 io 运行时（一个进程里多个库共用一份线程池时用）。
    static std::shared_ptr<HttpClient> create_with_mechanism(
        std::shared_ptr<netmech::Mechanism> mech, ClientConfig cfg);

    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// 提交一个请求。
    ///
    /// @param[in] spec    请求内容（按值移动，库持有到响应结束）。
    /// @param[in] handler 响应回调，**不能为空**。
    /// @return true  已受理（可能排在队列里）；
    ///         false 未受理 —— 队列已满（`max_pending_requests`），
    ///               或 client 已 `close()`。此时 `handler` 不会被回调。
    bool request(RequestSpec spec, std::shared_ptr<ResponseHandler> handler);

    /// 主动关闭连接并清空队列（排队中的请求各回调一次
    /// `Errc::not_connected`）。幂等。
    void close() noexcept;

    [[nodiscard]] bool        connected() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;

    [[nodiscard]] std::uint64_t requests_total() const noexcept;
    [[nodiscard]] std::uint64_t failures_total() const noexcept;

private:
    HttpClient();

    std::shared_ptr<Impl> m_impl;
};

// =============================================================================
//  WebSocket 客户端
// =============================================================================

struct WebSocketClientConfig {
    /// 注入 netmech 的 io 运行时（`netmech/runtime.hpp`）。空 = 默认运行时。
    std::shared_ptr<netmech::Runtime> runtime;
    /// 服务端地址（含端口）。
    Endpoint remote{"127.0.0.1", 80};

    /// 握手请求的路径（含 query），例如 `"/ws/events?token=abc"`。
    std::string path{"/"};

    /// 写进 `Host:`。空 = 由 `remote` 生成（端口 80 时省略端口）。
    std::string host;

    /// 额外请求头：`Cookie` / `Authorization` / `Sec-WebSocket-Protocol` 等。
    /// `Upgrade` / `Connection` / `Sec-WebSocket-Key` / `Sec-WebSocket-Version`
    /// 由库生成，这里写了也会被覆盖。
    Headers headers;

    std::string user_agent = "httpmech";

    std::chrono::milliseconds connect_timeout{3'000};
    std::chrono::milliseconds handshake_timeout{5'000};

    WebSocketConfig ws{};
};

/// 客户端侧的回调。
///
/// 与服务端的 `WebSocketHandlers` 分开，是因为客户端多了一个"连不上/握手被拒"
/// 的阶段 —— 那时还没有 session 对象，`on_close` 没法表达。
struct WebSocketClientHandlers {
    std::function<void(WebSocketSession&)>                                       on_open;
    std::function<void(WebSocketSession&, span<const std::uint8_t>, bool)>       on_message;
    std::function<void(WebSocketSession&, std::uint16_t, std::string_view)>      on_close;

    /// 连接失败或握手被拒（此时**不会**有 `on_open` / `on_close`）。
    std::function<void(std::error_code)> on_error;
};

/// WebSocket 客户端（主动连接、发/收帧）。
///
/// @par 用法
/// @code
/// httpmech::WebSocketClientConfig cfg;
/// cfg.remote = {"192.168.3.10", 8080};
/// cfg.path   = "/ws/events";
///
/// auto cli = httpmech::WebSocketClient::create(mech, cfg);
///
/// httpmech::WebSocketClientHandlers h;
/// h.on_open    = [](httpmech::WebSocketSession& s) { s.send_text("hi"); };
/// h.on_message = [](httpmech::WebSocketSession& s, httpmech::span<const std::uint8_t> d, bool text) {
///     // ...
/// };
/// h.on_close   = [](httpmech::WebSocketSession&, std::uint16_t code, std::string_view) {};
/// h.on_error   = [](std::error_code ec) { fprintf(stderr, "%s\n", ec.message().c_str()); };
///
/// cli->connect(std::move(h));      // 异步：结果走回调
/// @endcode
///
/// @par 重连
/// **不自动重连**。掉线后 `on_close` / `on_error` 会来一次，要不要重连、隔多久
/// 重连由应用决定（设备侧通常要退避）。库只提供一次干净的状态复位。
class HTTPMECH_API WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    struct Impl;   ///< 内部实现（不透明，定义在 src/client.cpp）

    static std::shared_ptr<WebSocketClient> create(WebSocketClientConfig cfg);

    /// 复用外部 io 运行时。
    static std::shared_ptr<WebSocketClient> create_with_mechanism(
        std::shared_ptr<netmech::Mechanism> mech, WebSocketClientConfig cfg);

    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    /// 发起连接（异步）。已连接或连接中时返回 false。
    bool connect(WebSocketClientHandlers handlers);

    /// 主动关闭（发 close 帧，等对端回应或 2 秒超时）。幂等。
    void close(std::uint16_t code = static_cast<std::uint16_t>(WsCloseCode::normal),
               std::string_view reason = {});

    [[nodiscard]] bool          connecting() const noexcept;
    [[nodiscard]] bool          is_open() const noexcept;
    [[nodiscard]] std::uint64_t messages_received() const noexcept;

private:
    WebSocketClient();

    std::shared_ptr<Impl> m_impl;
};

} // namespace httpmech
