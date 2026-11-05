// =============================================================================
//  httpmech - 消息视图的实现
// =============================================================================
#include <httpmech/message.hpp>

#include <cstdlib>
#include <optional>
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace httpmech {

namespace {

/// 十进制解析，全程做溢出检查。任何非法字符都判失败。
bool parse_u64(std::string_view s, std::uint64_t& out) noexcept
{
    if (s.empty() || s.size() > 20) {
        return false;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;   // 溢出
        }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

/// Transfer-Encoding 的**最后一个** coding 是否为 chunked（RFC 9112 §6.1）。
bool te_is_chunked(const HeadersView& h) noexcept
{
    const auto te = h.get("Transfer-Encoding");
    if (te.empty()) {
        return false;
    }
    // "gzip, chunked" / "chunked" 都算；只看最后一段。
    std::size_t pos = te.rfind(',');
    std::string_view last = (pos == std::string_view::npos) ? te : te.substr(pos + 1);
    return iequals(trim_ows(last), "chunked");
}

/// 逗号分隔的 token 列表里是否含某项（大小写不敏感），用于 Connection 头。
bool token_list_has(std::string_view list, std::string_view token) noexcept
{
    std::size_t i = 0;
    while (i <= list.size()) {
        const std::size_t comma = list.find(',', i);
        const std::size_t end   = (comma == std::string_view::npos) ? list.size() : comma;
        if (iequals(trim_ows(list.substr(i, end - i)), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        i = comma + 1;
    }
    return false;
}

} // namespace

// =============================================================================
//  Content-Length
// =============================================================================

std::optional<std::uint64_t> parse_content_length(const HeadersView& h) noexcept
{
    std::optional<std::uint64_t> result;
    for (const auto& f : h.all()) {
        if (!iequals(f.name, "Content-Length")) {
            continue;
        }
        std::uint64_t v = 0;
        if (!parse_u64(trim_ows(f.value), v)) {
            return std::nullopt;
        }
        if (result && *result != v) {
            return std::nullopt;   // 多个不同值 = 协议错误
        }
        result = v;
    }
    return result;
}

// =============================================================================
//  HttpRequest
// =============================================================================

bool HttpRequest::is_chunked() const noexcept
{
    return te_is_chunked(headers);
}

std::string_view HttpRequest::content_type() const noexcept
{
    const auto ct = headers.get("Content-Type");
    const auto semi = ct.find(';');
    return (semi == std::string_view::npos) ? ct : trim_ows(ct.substr(0, semi));
}

bool HttpRequest::expects_continue() const noexcept
{
    return iequals(trim_ows(headers.get("Expect")), "100-continue");
}

bool HttpRequest::is_websocket_upgrade() const noexcept
{
    if (!token_list_has(headers.get("Connection"), "upgrade")) {
        return false;
    }
    return iequals(trim_ows(headers.get("Upgrade")), "websocket");
}

std::string_view HttpRequest::real_ip() const noexcept
{
    if (!from_trusted_proxy) {
        return {};
    }
    const auto real = trim_ows(headers.get("X-Real-IP"));
    if (!real.empty()) {
        return real;
    }
    // X-Forwarded-For: client, proxy1, proxy2 —— 取第一段
    const auto xff = headers.get("X-Forwarded-For");
    if (!xff.empty()) {
        const auto comma = xff.find(',');
        return trim_ows(comma == std::string_view::npos ? xff : xff.substr(0, comma));
    }
    return {};
}

std::uint16_t HttpRequest::real_port() const noexcept
{
    if (!from_trusted_proxy) {
        return 0;
    }
    std::uint64_t v = 0;
    if (!parse_u64(trim_ows(headers.get("X-Real-PORT")), v) || v == 0 || v > 65535) {
        return 0;
    }
    return static_cast<std::uint16_t>(v);
}

bool HttpRequest::is_forwarded_https() const noexcept
{
    if (!from_trusted_proxy) {
        return false;
    }
    if (iequals(trim_ows(headers.get("X-Forwarded-Proto")), "https")) {
        return true;
    }
    const auto agent_ssl = trim_ows(headers.get("X-AGENT-SSL"));
    return !agent_ssl.empty() && agent_ssl != "0";
}

// =============================================================================
//  HttpResponse
// =============================================================================

bool HttpResponse::is_chunked() const noexcept
{
    return te_is_chunked(headers);
}

} // namespace httpmech
