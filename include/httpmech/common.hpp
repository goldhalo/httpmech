// =============================================================================
//  httpmech - 公共基础类型
// =============================================================================
//  文件    : httpmech/common.hpp
//  用途    : 方法 / 版本 / 状态码 / 解析上限 / 错误码。
//            业务代码一般只 #include <httpmech/httpmech.hpp>。
//  依赖    : 仅 C++17 标准库（不拖 asio，不拖 netmech）。
//  编码    : UTF-8
//
//  ★ 为什么单独拆出来
//    解析器（parser.hpp）服务端与客户端共用；类型放这里，两边都能只包含
//    一个小头，不必为了拿个 Method 把 server.hpp 整个拖进来。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace httpmech {

// =============================================================================
//  方法
// =============================================================================

/// HTTP 方法。
///
/// @note 用枚举而不是 `std::string_view`：解析时只做一次比较，
///       之后走分支快得多；也避免应用里到处写字符串比较。
enum class Method : std::uint8_t {
    unknown = 0,   ///< 不认识的方法（保留原始 token 在 HttpRequest::method_text）
    get,
    head,
    post,
    put,
    delete_,
    connect,
    options,
    trace,
    patch,
};

/// 枚举 → 规范大写文本（"GET" / "DELETE" ...）。`unknown` 返回 "UNKNOWN"。
[[nodiscard]] const char* to_string(Method m) noexcept;

/// 文本 → 枚举，大小写敏感（HTTP 方法就是大写）。不认识返回 `Method::unknown`。
[[nodiscard]] Method method_from(std::string_view text) noexcept;

/// 该方法的请求能否带消息体（GET/HEAD/DELETE/OPTIONS/TRACE/CONNECT 一般为否）。
///
/// @note 只是**提示**：解析器不据此拒绝带体的 GET —— 现实里真有这种请求，
///       严格拒绝会互通性变差。要不要处理由应用决定。
[[nodiscard]] bool method_allows_body(Method m) noexcept;

// =============================================================================
//  版本
// =============================================================================

enum class Version : std::uint8_t {
    unknown = 0,
    http_1_0,
    http_1_1,
};

/// "HTTP/1.1" 这种文本。`unknown` 返回 "HTTP/1.1"（保守假设）。
[[nodiscard]] const char* to_string(Version v) noexcept;

// =============================================================================
//  状态码
// =============================================================================

/// 响应状态码。只列常见值；**非枚举值用 `other`**，真实数字看
/// `HttpResponse::status_code`。
///
/// @note 除 `other` 外，取值与线上码一致，可 `static_cast<std::uint16_t>(s)`
///       直接得到三位数字。
enum class Status : std::uint16_t {
    other               = 0,     ///< 不在下表里的码，数字见 HttpResponse::status_code

    continue_100        = 100,

    ok                  = 200,
    created             = 201,
    accepted            = 202,
    no_content          = 204,
    partial_content     = 206,

    moved_permanently   = 301,
    found               = 302,
    not_modified        = 304,
    temporary_redirect  = 307,
    permanent_redirect  = 308,

    bad_request         = 400,
    unauthorized        = 401,
    forbidden           = 403,
    not_found           = 404,
    method_not_allowed  = 405,
    request_timeout     = 408,
    length_required     = 411,
    payload_too_large   = 413,
    uri_too_long        = 414,
    unsupported_media   = 415,
    expectation_failed  = 417,
    teapot              = 418,   ///< 保留，测试用
    too_many_requests   = 429,
    header_fields_too_large = 431,

    internal_error      = 500,
    not_implemented     = 501,
    bad_gateway         = 502,
    service_unavailable = 503,
    gateway_timeout     = 504,
    version_not_supported = 505,
};

/// 数字 → 枚举。认识的返回对应值，不认识的返回 `Status::other`
/// （此时请从 `HttpResponse::status_code` 取原始数字）。
[[nodiscard]] Status status_from(std::uint16_t code) noexcept;

/// 标准原因短语（"Not Found"）。`Status::other` 返回 ""，
/// 自定义短语请用 `HttpResponse::reason`。
[[nodiscard]] const char* reason_phrase(Status s) noexcept;

[[nodiscard]] constexpr bool is_informational(Status s) noexcept { return static_cast<std::uint16_t>(s) < 200; }
[[nodiscard]] constexpr bool is_success(Status s) noexcept       { return static_cast<std::uint16_t>(s) >= 200 && static_cast<std::uint16_t>(s) < 300; }
[[nodiscard]] constexpr bool is_redirect(Status s) noexcept      { return static_cast<std::uint16_t>(s) >= 300 && static_cast<std::uint16_t>(s) < 400; }
[[nodiscard]] constexpr bool is_client_error(Status s) noexcept  { return static_cast<std::uint16_t>(s) >= 400 && static_cast<std::uint16_t>(s) < 500; }
[[nodiscard]] constexpr bool is_server_error(Status s) noexcept  { return static_cast<std::uint16_t>(s) >= 500; }

// =============================================================================
//  解析上限
// =============================================================================
//
//  一起写死在一起，是因为这组数字决定了"面对恶意/畸形请求，进程会不会被
//  单个连接吃掉内存"。每一项都能被应用单独调，但**必须有一个非零默认值**。

/// 解析器与连接的资源上限。
struct Limits {
    /// 请求行 + 全部头部字段的总字节数上限（默认 32 KiB）。
    /// 超了回 `431 Request Header Fields Too Large` 并断开。
    std::size_t max_header_bytes = 32 * 1024;

    /// 头部字段条数上限（默认 100）。超了同上。
    std::size_t max_header_fields = 100;

    /// 单个请求/响应体（Content-Length 或 chunked 解出来的总量）上限（默认 8 MiB）。
    ///
    /// @note 超过这个值**不是**直接报错：走了"流式模式"的连接会持续把数据
    ///       交给 `on_body_data()`，不在这里拦。这个值只用于"聚合模式"——
    ///       即应用声明要一次性拿到完整 body 时，超限回 `413`。
    std::size_t max_body_bytes = 8 * 1024 * 1024;

    /// chunk 头（`1a2b\r\n`）与 trailer 的单行长度上限（默认 1 KiB）。
    /// 超了回 `400`。防止对端发一个不换行的超长 chunk 头撑爆缓冲。
    std::size_t max_chunk_line = 1024;
};

// =============================================================================
//  错误码
// =============================================================================

/// 协议层错误。传输层错误（断连、超时）直接用 netmech 的 `std::error_code`，
/// 本枚举只表达"对端发来的东西不合协议"或"本端配置不满足"。
enum class Errc {
    ok = 0,

    // --- 解析阶段 ---
    bad_request_line   = 1,   ///< 请求行无法解析
    bad_status_line    = 2,   ///< 状态行无法解析
    bad_header_field   = 3,   ///< 头部字段格式非法
    header_too_large   = 4,   ///< 超过 Limits::max_header_bytes
    too_many_headers   = 5,   ///< 超过 Limits::max_header_fields
    unsupported_version= 6,   ///< 不是 HTTP/1.0 或 1.1
    bad_chunk          = 7,   ///< chunk 大小行或 CRLF 非法
    body_too_large     = 8,   ///< 聚合模式下超过 Limits::max_body_bytes

    // --- 语义阶段 ---
    conflicting_length = 10,  ///< Content-Length 与 Transfer-Encoding 同时出现
    bad_content_length = 11,  ///< Content-Length 不是十进制数字，或有多个不同值
    unsupported_transfer_coding = 12,  ///< Transfer-Encoding 不是 chunked

    // --- 本端状态 ---
    not_started        = 20,  ///< 服务端/客户端尚未 start()
    already_started    = 21,
    not_connected      = 22,  ///< 客户端当前无可用连接
    headers_sent       = 23,  ///< 已开始发送响应，不能再改状态/头部
    response_incomplete= 24,  ///< 应用没调用 end() 就返回了
};

/// 错误码类别。`std::error_code{e, httpmech::error_category()}`。
[[nodiscard]] const std::error_category& error_category() noexcept;

[[nodiscard]] std::error_code make_error_code(Errc e) noexcept;

} // namespace httpmech

// 让 `std::error_code ec = httpmech::Errc::bad_request_line;` 成立
namespace std {
template <>
struct is_error_code_enum<httpmech::Errc> : true_type {};
} // namespace std
