// =============================================================================
//  httpmech - 客户端
// =============================================================================
//  文件    : httpmech/client.hpp
//  用途    : HttpClient —— 一条到目标地址的连接，串行发请求、收响应。
//  依赖    : netmech（Mechanism / Linkable / Conn）+ 本库 parser。
//  编码    : UTF-8
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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <netmech/common.hpp>
#include <netmech/mechanism.hpp>

#include <httpmech/common.hpp>
#include <httpmech/headers.hpp>
#include <httpmech/message.hpp>
#include <httpmech/parser.hpp>
#include <httpmech/span.hpp>

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

class ResponseHandler {
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
    /// 目标地址（含端口）。`Host:` 头按它填（`IP:port`，端口 80 时省略）。
    netmech::Address remote{"127.0.0.1", 80};

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
class HttpClient : public std::enable_shared_from_this<HttpClient> {
public:
    /// 创建（不连接）。首次 `request()` 时自动连接。
    static std::shared_ptr<HttpClient> create(std::shared_ptr<netmech::Mechanism> mech,
                                              ClientConfig                       cfg);

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
};

} // namespace httpmech
