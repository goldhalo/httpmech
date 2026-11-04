// =============================================================================
//  httpmech - 内部工具：SHA-1 / Base64
// =============================================================================
//  WebSocket 握手只需要这两样：
//    Sec-WebSocket-Accept = base64( sha1( key + GUID ) )   （RFC 6455 §4.2.2）
//
//  不引 OpenSSL：SHA-1 约 120 行，Base64 约 40 行，都是纯函数、无状态。
//  **注意**：SHA-1 在这里只用于协议握手（不是安全用途），不要拿去算签名。
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace httpmech {
namespace detail {

/// 20 字节 SHA-1 摘要。
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view data) noexcept;

/// 标准 Base64 编码（带 `=` 填充）。
[[nodiscard]] std::string base64_encode(const std::uint8_t* data, std::size_t len);

} // namespace detail
} // namespace httpmech
