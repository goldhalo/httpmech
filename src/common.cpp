// =============================================================================
//  httpmech - 公共基础类型的实现
// =============================================================================
#include <httpmech/common.hpp>

#include <string>

namespace httpmech {

// =============================================================================
//  方法
// =============================================================================

const char* to_string(Method m) noexcept
{
    switch (m) {
    case Method::get:     return "GET";
    case Method::head:    return "HEAD";
    case Method::post:    return "POST";
    case Method::put:     return "PUT";
    case Method::delete_: return "DELETE";
    case Method::connect: return "CONNECT";
    case Method::options: return "OPTIONS";
    case Method::trace:   return "TRACE";
    case Method::patch:   return "PATCH";
    case Method::unknown: break;
    }
    return "UNKNOWN";
}

Method method_from(std::string_view text) noexcept
{
    // 方法都是大写，且长度 3~7；这里按长度先分派，避免逐字符比较一串。
    switch (text.size()) {
    case 3:
        if (text == "GET") return Method::get;
        if (text == "PUT") return Method::put;
        break;
    case 4:
        if (text == "HEAD") return Method::head;
        if (text == "POST") return Method::post;
        break;
    case 5:
        if (text == "PATCH") return Method::patch;
        if (text == "TRACE") return Method::trace;
        break;
    case 6:
        if (text == "DELETE") return Method::delete_;
        break;
    case 7:
        if (text == "OPTIONS") return Method::options;
        if (text == "CONNECT") return Method::connect;
        break;
    default:
        break;
    }
    return Method::unknown;
}

bool method_allows_body(Method m) noexcept
{
    switch (m) {
    case Method::post:
    case Method::put:
    case Method::patch:
        return true;
    default:
        return false;
    }
}

// =============================================================================
//  版本
// =============================================================================

const char* to_string(Version v) noexcept
{
    return v == Version::http_1_0 ? "HTTP/1.0" : "HTTP/1.1";
}

// =============================================================================
//  状态码
// =============================================================================

Status status_from(std::uint16_t code) noexcept
{
    switch (code) {
    case 100: return Status::continue_100;
    case 101: return Status::switching_protocols;

    case 200: return Status::ok;
    case 201: return Status::created;
    case 202: return Status::accepted;
    case 204: return Status::no_content;
    case 206: return Status::partial_content;

    case 301: return Status::moved_permanently;
    case 302: return Status::found;
    case 304: return Status::not_modified;
    case 307: return Status::temporary_redirect;
    case 308: return Status::permanent_redirect;

    case 400: return Status::bad_request;
    case 401: return Status::unauthorized;
    case 403: return Status::forbidden;
    case 404: return Status::not_found;
    case 405: return Status::method_not_allowed;
    case 408: return Status::request_timeout;
    case 411: return Status::length_required;
    case 413: return Status::payload_too_large;
    case 414: return Status::uri_too_long;
    case 415: return Status::unsupported_media;
    case 417: return Status::expectation_failed;
    case 418: return Status::teapot;
    case 429: return Status::too_many_requests;
    case 431: return Status::header_fields_too_large;

    case 500: return Status::internal_error;
    case 501: return Status::not_implemented;
    case 502: return Status::bad_gateway;
    case 503: return Status::service_unavailable;
    case 504: return Status::gateway_timeout;
    case 505: return Status::version_not_supported;
    default:  return Status::other;
    }
}

const char* reason_phrase(Status s) noexcept
{
    switch (s) {
    case Status::continue_100:          return "Continue";
    case Status::switching_protocols:   return "Switching Protocols";
    case Status::ok:                    return "OK";
    case Status::created:               return "Created";
    case Status::accepted:              return "Accepted";
    case Status::no_content:            return "No Content";
    case Status::partial_content:       return "Partial Content";
    case Status::moved_permanently:     return "Moved Permanently";
    case Status::found:                 return "Found";
    case Status::not_modified:          return "Not Modified";
    case Status::temporary_redirect:    return "Temporary Redirect";
    case Status::permanent_redirect:    return "Permanent Redirect";
    case Status::bad_request:           return "Bad Request";
    case Status::unauthorized:          return "Unauthorized";
    case Status::forbidden:             return "Forbidden";
    case Status::not_found:             return "Not Found";
    case Status::method_not_allowed:    return "Method Not Allowed";
    case Status::request_timeout:       return "Request Timeout";
    case Status::length_required:       return "Length Required";
    case Status::payload_too_large:     return "Payload Too Large";
    case Status::uri_too_long:          return "URI Too Long";
    case Status::unsupported_media:     return "Unsupported Media Type";
    case Status::expectation_failed:    return "Expectation Failed";
    case Status::teapot:                return "I'm a teapot";
    case Status::too_many_requests:     return "Too Many Requests";
    case Status::header_fields_too_large: return "Request Header Fields Too Large";
    case Status::internal_error:        return "Internal Server Error";
    case Status::not_implemented:       return "Not Implemented";
    case Status::bad_gateway:           return "Bad Gateway";
    case Status::service_unavailable:   return "Service Unavailable";
    case Status::gateway_timeout:       return "Gateway Timeout";
    case Status::version_not_supported: return "HTTP Version Not Supported";
    case Status::other:                 break;
    }
    return "";
}

// =============================================================================
//  错误码
// =============================================================================

namespace {

class ErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "httpmech"; }

    std::string message(int ev) const override
    {
        switch (static_cast<Errc>(ev)) {
        case Errc::ok:                        return "ok";
        case Errc::bad_request_line:          return "malformed request line";
        case Errc::bad_status_line:           return "malformed status line";
        case Errc::bad_header_field:          return "malformed header field";
        case Errc::header_too_large:          return "header section too large";
        case Errc::too_many_headers:          return "too many header fields";
        case Errc::unsupported_version:       return "unsupported HTTP version";
        case Errc::bad_chunk:                 return "malformed chunked encoding";
        case Errc::body_too_large:            return "body exceeds limit";
        case Errc::conflicting_length:        return "both Content-Length and Transfer-Encoding";
        case Errc::bad_content_length:        return "malformed Content-Length";
        case Errc::unsupported_transfer_coding: return "unsupported transfer coding";
        case Errc::not_started:               return "not started";
        case Errc::already_started:           return "already started";
        case Errc::not_connected:             return "not connected";
        case Errc::headers_sent:              return "headers already sent";
        case Errc::response_incomplete:       return "handler returned without ending the response";
        case Errc::client_timeout:            return "response timeout";
        case Errc::bad_websocket_handshake:   return "websocket handshake rejected";
        case Errc::linger_zero_enabled:
            return "Mechanism was created with linger_zero=true: SO_LINGER{1,0} sends RST on close "
                   "and would drop the last response. Create the Mechanism with "
                   "Config::linger_zero=false, or set ServerConfig::allow_linger_zero=true";
        }
        return "unknown httpmech error";
    }
};

} // namespace

const std::error_category& error_category() noexcept
{
    static const ErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept
{
    return {static_cast<int>(e), error_category()};
}

} // namespace httpmech
