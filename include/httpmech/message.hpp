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

#include <cstdint>
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
[[nodiscard]] std::optional<std::uint64_t> parse_content_length(const HeadersView& h) noexcept;

// =============================================================================
//  请求
// =============================================================================

/// 收到的一条请求（服务端）。
struct HttpRequest {
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
};

// =============================================================================
//  响应
// =============================================================================

/// 收到的一条响应（客户端）。
struct HttpResponse {
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
