// =============================================================================
//  httpmech - 消息视图
// =============================================================================
//  文件    : httpmech/message.hpp
//  用途    : HttpRequest / HttpResponse —— 解析结果的**只读视图**。
//  依赖    : C++17 标准库。
//  编码    : UTF-8
//
//  ★ 为什么全是视图，不拷成 std::string
//    一条请求的处理路径上最常见的是"看一眼就回"。把 method / target / header
//    全拷一遍，等于每个请求多一次堆分配 + 一次 memcpy。这里统一借用解析器的
//    接收缓冲，应用要留就自己拷（`std::string{req.target}`）。
//
//  ★ 生命周期（唯一需要记住的一条）
//    所有 string_view / span 都指向**解析器内部缓冲**，在
//    `RequestHandler::on_request()` / `ResponseHandler::on_response()` 返回时失效。
//    跨回调保存 = 悬垂。要跨回调请拷。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <httpmech/common.hpp>
#include <httpmech/headers.hpp>
#include <httpmech/span.hpp>

namespace httpmech {

/// 读 `Content-Length`。缺失、非十进制、或出现多个不同值时返回 `std::nullopt`。
///
/// @note 「多个不同值」按 RFC 9110 §8.6 属于协议错误，解析器会在
///       更早的阶段就以 `Errc::bad_content_length` 拒掉；这里返回 nullopt
///       只是给手工构造的消息用。
[[nodiscard]] HTTPMECH_API std::optional<std::uint64_t> parse_content_length(const HeadersView& h) noexcept;

// =============================================================================
//  请求
// =============================================================================

/// 收到的一条请求（服务端）。
struct HTTPMECH_API HttpRequest {
    /// 方法。认识的方法是枚举，不认识的是 `Method::unknown`（原样看 `method_text`）。
    Method method = Method::unknown;
    std::string_view method_text;   ///< 原始方法 token

    std::string_view target;        ///< 原始请求目标，如 "/api/v1/x?a=1"
    std::string_view path;          ///< target 中 `?` 之前的部分（已切分，不含 query）
    std::string_view query;         ///< `?` 之后的部分（不含 `?`；无 query 时为空）

    Version version = Version::http_1_1;

    HeadersView headers;

    /// 消息体。**只在聚合模式下有效**（见 `body_streamed`）。
    /// 流式模式下这里是空的，body 逐块从 `RequestHandler::on_body_data()` 出来。
    span<const std::uint8_t> body;

    /// body 是否以流式方式交付（应用没申请聚合、或超过了聚合上限）。
    bool body_streamed = false;

    /// 对端是否要复用连接（看 `Connection` 头 + 版本默认值算好的结果）。
    bool keep_alive = true;

    // --- 便捷查询 ---

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept { return headers.get(name); }

    /// `Content-Length`。缺失或非法返回 `std::nullopt`。
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept
    {
        return parse_content_length(headers);
    }

    /// `Transfer-Encoding: chunked`。
    [[nodiscard]] bool is_chunked() const noexcept;

    /// `Host`（HTTP/1.1 必需）。
    [[nodiscard]] std::string_view host() const noexcept { return headers.get("Host"); }

    /// `Content-Type`（不含 `;charset=` 之后的部分）。
    [[nodiscard]] std::string_view content_type() const noexcept;

    /// 是否 `Connection: close` 之类要求关连接。
    [[nodiscard]] bool wants_close() const noexcept { return !keep_alive; }

    /// 是否 `Expect: 100-continue`。
    ///
    /// @note v0.1 **不实现** 100-continue 协商：收到这个头也直接按普通请求收 body。
    ///       HTTP 允许服务端不回 100，客户端会在 1 秒后自己开始发 —— 能互通，
    ///       只是大 body 场景多等 1 秒。见 docs/design.md「未实现项」。
    [[nodiscard]] bool expects_continue() const noexcept;

    /// 是否 WebSocket 升级请求（`Upgrade: websocket` 且 `Connection` 含 upgrade）。
    ///
    /// @note 本库**不自动**升级：要升级必须由路由/处理器显式调用
    ///       `ResponseWriter::upgrade_websocket()`。这个谓词只是让路由器能提前分流。
    [[nodiscard]] bool is_websocket_upgrade() const noexcept;

    // --- 请求级用户数据 ---

    /// 应用自用的一格。**库从不读取、也不释放它的语义**。
    ///
    /// 典型用法是把"这次请求的上下文"挂在 `before` 钩子里，供后续路由与
    /// `after` 钩子使用（比如鉴权结果、耗时统计的起点、操作日志需要的字段）：
    ///
    /// @code
    /// router->before([](httpmech::HttpRequest& req, httpmech::ResponseWriter& out) {
    ///     auto ctx = std::make_shared<MyCtx>();
    ///     ctx->token  = std::string(req.header("Authorization"));
    ///     ctx->userId = verify(ctx->token);
    ///     if (ctx->userId.empty()) { out.end_status(httpmech::Status::unauthorized); return false; }
    ///     req.user = ctx;                       // 挂上
    ///     return true;
    /// });
    ///
    /// router->get("/api/x", [](const httpmech::HttpRequest& req, httpmech::ResponseWriter& out) {
    ///     auto ctx = std::static_pointer_cast<MyCtx>(req.user);   // 取回
    ///     ...
    /// });
    ///
    /// router->after([](const httpmech::HttpRequest& req, const httpmech::ResponseSummary& res) {
    ///     auto ctx = std::static_pointer_cast<MyCtx>(req.user);   // 记日志
    ///     ...
    /// });
    /// @endcode
    ///
    /// @note 同一条请求的 `before` → 路由 → `after` 传的是**同一个**
    ///       `HttpRequest` 对象（引用透传），所以这一格是通的。
    ///       到了 `after` 返回，这个 shared_ptr 随请求一起释放。
    std::shared_ptr<void> user;

    // --- 反向代理（nginx 前置）---

    /// 本请求是否来自受信任的代理（由 `ServerConfig::trusted_proxy_cidrs` 判定，
    /// 库在派发前填好）。**为 false 时，下面三个字段一律不采信**。
    bool from_trusted_proxy = false;

    /// 客户端真实 IP。
    ///
    /// 优先级：`X-Real-IP` → `X-Forwarded-For` 的第一个地址 → 空。
    /// 只有 `from_trusted_proxy` 为 true 才有意义 —— 否则这个头是客户端随便写的。
    [[nodiscard]] std::string_view real_ip() const noexcept;

    /// 客户端真实端口（`X-Real-PORT`）。缺失返回 0。
    [[nodiscard]] std::uint16_t real_port() const noexcept;

    /// 原始连接是否为 HTTPS。
    ///
    /// 判断依据（nginx 终结 TLS 时的通行做法）：
    ///   · `X-Forwarded-Proto: https`
    ///   · `X-AGENT-SSL: 非 0`（现场已有的约定）
    /// 本端直连且不是 TLS 时恒为 false。
    [[nodiscard]] bool is_forwarded_https() const noexcept;
};

// =============================================================================
//  响应
// =============================================================================

/// 收到的一条响应（客户端）。
struct HTTPMECH_API HttpResponse {
    Version version = Version::http_1_1;

    /// 状态码的枚举形态；不在枚举表里时是 `Status::other`。
    Status status = Status::other;

    /// 原始三位状态码。**永远以这个为准**（`status` 只是便捷判断）。
    std::uint16_t status_code = 0;

    /// 对端给的原因短语（可能为空）。
    std::string_view reason;

    HeadersView headers;

    /// 消息体，语义同 `HttpRequest::body`。
    span<const std::uint8_t> body;
    bool body_streamed = false;

    bool keep_alive = true;

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept { return headers.get(name); }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept
    {
        return parse_content_length(headers);
    }
    [[nodiscard]] bool is_chunked() const noexcept;
    [[nodiscard]] bool wants_close() const noexcept { return !keep_alive; }

    /// `Location`（重定向）。本库**不自动跟随**，交应用决定。
    [[nodiscard]] std::string_view location() const noexcept { return headers.get("Location"); }
};

} // namespace httpmech
