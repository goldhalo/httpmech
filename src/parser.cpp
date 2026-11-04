// =============================================================================
//  httpmech - HTTP/1.1 增量解析器
// =============================================================================
//  实现要点见头文件顶部注释。这里补充三处实现层的说明：
//
//  1. 头部字节会**拷一份**到 header_owned。
//     原因：视图必须稳定。接收缓冲在 body 阶段还会继续增长/压缩，
//     直接让 string_view 指进去，一次 realloc 就全成悬垂。
//     头块很小（上限 32 KiB），一次 memcpy 换掉一整类生命周期 bug，划算。
//     body 仍然是零拷贝的（流式模式直接切接收缓冲）。
//
//  2. pico 的 last_len 传"上次扫描到的缓冲长度"，避免慢速喂数据时每字节都从头扫。
//     缓冲一旦被消费/压缩（begin 变化），必须把它清零 —— 偏移作废了。
//
//  3. 一次 next() 只产出一个事件。feed() = 追加数据 + next()；
//     要把一批数据里的事件取完，就循环调 next() 直到 need_more。
// =============================================================================
#include <httpmech/parser.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// 必须先包含 rename：pico 的符号在本库里带 httpmech_ 前缀，
// 这样和 rtspmech 各自内嵌一份时不会在链接期撞车。
#include "pico_rename.hpp"

#include <picohttpparser.h>

namespace httpmech {

namespace {

constexpr std::size_t kMaxHeaderFieldsHardCap = 4096;

inline std::string_view sv(const char* p, std::size_t n) noexcept
{
    return (p == nullptr) ? std::string_view{} : std::string_view{p, n};
}

/// 十六进制解析（chunk 大小行）。允许后面跟 ";ext" 之类的扩展。
bool parse_chunk_size(std::string_view line, std::uint64_t& out) noexcept
{
    line = trim_ows(line);
    std::size_t   i   = 0;
    std::uint64_t v   = 0;
    bool          any = false;
    for (; i < line.size(); ++i) {
        const char    c = line[i];
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            break;
        }
        if (v > (UINT64_MAX >> 4)) {
            return false;
        }
        v   = (v << 4) | d;
        any = true;
    }
    if (!any) {
        return false;
    }
    for (; i < line.size(); ++i) {
        if (line[i] == ';') {
            break;
        }
        if (line[i] != ' ' && line[i] != '\t') {
            return false;
        }
    }
    out = v;
    return true;
}

/// Connection 这类逗号列表里是否含某 token（大小写不敏感）。
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

bool te_is_chunked(const HeadersView& h) noexcept
{
    const auto te = h.get("Transfer-Encoding");
    if (te.empty()) {
        return false;
    }
    const std::size_t pos  = te.rfind(',');
    const std::string_view last = (pos == std::string_view::npos) ? te : te.substr(pos + 1);
    return iequals(trim_ows(last), "chunked");
}

} // namespace

// =============================================================================
//  实现
// =============================================================================

struct Parser::Impl {
    Role   role;
    Config cfg;

    std::vector<char> buf;        // 未消费字节在 [begin, buf.size())
    std::size_t       begin = 0;
    std::size_t       last_scan = 0;

    std::string              header_owned;   // 头块副本，视图指向它
    std::vector<HeaderField> fields;
    std::string              body_owned;     // 聚合模式的 body

    HttpRequest  req;
    HttpResponse resp;

    std::vector<phr_header> hdr_scratch;

    enum class St : std::uint8_t {
        headers,
        body_fixed,
        chunk_size,
        chunk_data,
        chunk_crlf,
        trailers,
        done,
    };
    St            st = St::headers;
    std::uint64_t remaining = 0;
    bool          headers_emitted = false;

    span<const std::uint8_t> chunk_view{};
    std::uint64_t            messages = 0;
    bool                     failed   = false;

    Impl(Role r, Config c) : role(r), cfg(std::move(c))
    {
        const std::size_t cap = std::min<std::size_t>(
            std::max<std::size_t>(cfg.limits.max_header_fields, 8), kMaxHeaderFieldsHardCap);
        hdr_scratch.resize(cap);
        buf.reserve(std::max<std::size_t>(cfg.initial_buffer_size, 1024));
        header_owned.reserve(2048);
        fields.reserve(32);
        body_owned.reserve(4096);
    }

    std::size_t avail() const noexcept { return buf.size() - begin; }
    const char* data() const noexcept { return buf.data() + begin; }

    void compact_if_needed()
    {
        if (begin == buf.size()) {
            buf.clear();
            begin     = 0;
            last_scan = 0;
            return;
        }
        if (begin >= 65536 && begin * 2 >= buf.size()) {
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(begin));
            begin     = 0;
            last_scan = 0;
        }
    }

    void fail(std::error_code& ec, Errc e)
    {
        ec     = make_error_code(e);
        failed = true;
    }

    bool parse_head(std::error_code& ec);
    void decide_framing(std::error_code& ec);
    void finish_message();
};

// ---------------------------------------------------------------------------
//  头部：pico 解析 + 拷块 + 建视图
// ---------------------------------------------------------------------------

bool Parser::Impl::parse_head(std::error_code& ec)
{
    const std::size_t n = avail();
    if (n == 0) {
        return false;
    }
    if (n > cfg.limits.max_header_bytes) {
        fail(ec, Errc::header_too_large);
        return false;
    }

    const char* b  = data();
    std::size_t nh = hdr_scratch.size();
    int         rc = 0;

    // pico 给回来的指针（相对 b 的偏移，拷块之后要用偏移重绑）
    struct Raw {
        const char* method     = nullptr;
        std::size_t method_len = 0;
        const char* path       = nullptr;
        std::size_t path_len   = 0;
        int         minor      = 0;
        int         status     = 0;
        const char* reason     = nullptr;
        std::size_t reason_len = 0;
    } raw;

    if (role == Role::server) {
        rc = phr_parse_request(b, n, &raw.method, &raw.method_len, &raw.path, &raw.path_len,
                               &raw.minor, hdr_scratch.data(), &nh, last_scan);
    } else {
        rc = phr_parse_response(b, n, &raw.minor, &raw.status, &raw.reason, &raw.reason_len,
                                hdr_scratch.data(), &nh, last_scan);
    }

    if (rc == -2) {
        // 不完整：记住已扫描长度，下次接着扫
        last_scan = n;
        return false;
    }
    if (rc < 0) {
        if (nh >= hdr_scratch.size()) {
            fail(ec, Errc::too_many_headers);
        } else {
            fail(ec, (role == Role::server) ? Errc::bad_request_line : Errc::bad_status_line);
        }
        return false;
    }

    // ---- 头块拷一份，指针全部改指副本 ----
    const std::size_t block = static_cast<std::size_t>(rc);
    header_owned.assign(b, block);

    char*       nbase = header_owned.data();
    auto rebase = [&](const char* p) -> const char* {
        return (p == nullptr) ? nullptr : nbase + (p - b);
    };

    // ---- 消息行 ----
    const Version v = (raw.minor == 0) ? Version::http_1_0 : Version::http_1_1;

    if (role == Role::server) {
        req            = HttpRequest{};
        req.method     = method_from(sv(raw.method, raw.method_len));
        req.method_text= sv(rebase(raw.method), raw.method_len);
        req.target     = sv(rebase(raw.path), raw.path_len);
        req.version    = v;

        const std::size_t q = req.target.find('?');
        if (q == std::string_view::npos) {
            req.path  = req.target;
            req.query = {};
        } else {
            req.path  = req.target.substr(0, q);
            req.query = req.target.substr(q + 1);
        }
    } else {
        resp             = HttpResponse{};
        resp.version     = v;
        resp.status_code = static_cast<std::uint16_t>(raw.status);
        resp.status      = status_from(resp.status_code);
        resp.reason      = sv(rebase(raw.reason), raw.reason_len);
    }

    // ---- 头部字段 ----
    fields.clear();
    for (std::size_t i = 0; i < nh; ++i) {
        if (hdr_scratch[i].name == nullptr) {
            // obs-fold 续行：RFC 9112 §5.2 允许直接拒绝，比悄悄丢掉更安全
            fail(ec, Errc::bad_header_field);
            return false;
        }
        fields.push_back(HeaderField{
            sv(rebase(hdr_scratch[i].name), hdr_scratch[i].name_len),
            trim_ows(sv(rebase(hdr_scratch[i].value), hdr_scratch[i].value_len))});
    }

    const HeadersView hv{span<const HeaderField>{fields.data(), fields.size()}};
    if (role == Role::server) {
        req.headers = hv;
    } else {
        resp.headers = hv;
    }

    // ---- 消费头块 ----
    begin += block;
    last_scan = 0;

    decide_framing(ec);
    return true;
}

// ---------------------------------------------------------------------------
//  keep-alive 与 body 框架
// ---------------------------------------------------------------------------

void Parser::Impl::decide_framing(std::error_code& ec)
{
    const HeadersView hv = (role == Role::server) ? req.headers : resp.headers;
    const Version     v  = (role == Role::server) ? req.version : resp.version;

    bool ka = (v == Version::http_1_1);
    const std::string_view conn = hv.get("Connection");
    // Connection 可以出现多次，pico 会给多个字段；这里逐个列表项匹配
    for (const auto& f : hv.all()) {
        if (!iequals(f.name, "Connection")) {
            continue;
        }
        if (token_list_has(f.value, "close")) {
            ka = false;
        } else if (token_list_has(f.value, "keep-alive")) {
            ka = true;
        }
    }
    (void)conn;

    if (role == Role::server) {
        req.keep_alive = ka;
    } else {
        resp.keep_alive = ka;
    }

    const bool chunked     = te_is_chunked(hv);
    const auto content_len = parse_content_length(hv);

    // Content-Length 与 Transfer-Encoding 同时出现 = 请求走私的经典构造
    if (chunked && content_len) {
        fail(ec, Errc::conflicting_length);
        return;
    }

    // 响应里 1xx / 204 / 304 不带 body
    if (role == Role::client) {
        const auto sc = resp.status_code;
        if ((sc >= 100 && sc < 200) || sc == 204 || sc == 304) {
            finish_message();
            return;
        }
    }

    if (chunked) {
        st = St::chunk_size;
        return;
    }
    if (content_len) {
        if (*content_len == 0) {
            finish_message();
        } else {
            remaining = *content_len;
            st        = St::body_fixed;
        }
        return;
    }

    // 请求没有 body；响应既无长度也无 chunked（按无 body 处理，不读到底断开）
    finish_message();
}

void Parser::Impl::finish_message()
{
    st = St::done;
    ++messages;
    if (cfg.body_mode == BodyMode::aggregate) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(body_owned.data());
        const span<const std::uint8_t> body{p, body_owned.size()};
        if (role == Role::server) {
            req.body = body;
        } else {
            resp.body = body;
        }
    }
}

// =============================================================================
//  Parser 对外接口
// =============================================================================

Parser::Parser(Role role) : Parser(role, Config{}) {}

Parser::Parser(Role role, Config cfg) : m_impl(new Impl(role, std::move(cfg))) {}

Parser::~Parser() = default;

Parser::Result Parser::feed(span<const std::uint8_t> data, std::error_code& ec)
{
    Impl& d = *m_impl;

    d.compact_if_needed();
    if (!data.empty()) {
        const auto* p = reinterpret_cast<const char*>(data.data());
        d.buf.insert(d.buf.end(), p, p + data.size());
    }
    return next(ec);
}

Parser::Result Parser::next(std::error_code& ec)
{
    Impl& d = *m_impl;
    ec.clear();

    for (;;) {
        if (d.failed) {
            if (!ec) {
                ec = make_error_code(Errc::bad_request_line);
            }
            return Result::need_more;
        }

        switch (d.st) {
        case Impl::St::headers: {
            if (!d.parse_head(ec)) {
                return Result::need_more;   // ec 可能已被置位（错误）或为空（还要数据）
            }
            if (d.failed) {
                continue;                   // decide_framing 里失败
            }
            if (d.cfg.body_mode == BodyMode::stream) {
                d.headers_emitted = true;
                return Result::headers;
            }
            continue;
        }

        case Impl::St::body_fixed: {
            if (d.avail() == 0) {
                return Result::need_more;
            }
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(d.remaining, d.avail()));

            if (d.cfg.body_mode == BodyMode::stream) {
                d.chunk_view = span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(d.data()), take};
                d.begin += take;
                d.remaining -= take;
                if (d.remaining == 0) {
                    d.finish_message();
                }
                return Result::body_chunk;
            }

            if (d.body_owned.size() + take > d.cfg.limits.max_body_bytes) {
                d.fail(ec, Errc::body_too_large);
                return Result::need_more;
            }
            d.body_owned.append(d.data(), take);
            d.begin += take;
            d.remaining -= take;
            if (d.remaining == 0) {
                d.finish_message();
            }
            continue;
        }

        case Impl::St::chunk_size: {
            const char*       p    = d.data();
            const std::size_t n    = d.avail();
            const char*       crlf = nullptr;
            for (std::size_t i = 0; i + 1 < n; ++i) {
                if (p[i] == '\r' && p[i + 1] == '\n') {
                    crlf = p + i;
                    break;
                }
            }
            if (crlf == nullptr) {
                if (n > d.cfg.limits.max_chunk_line) {
                    d.fail(ec, Errc::bad_chunk);
                }
                return Result::need_more;
            }
            const std::size_t line_len = static_cast<std::size_t>(crlf - p);
            if (line_len > d.cfg.limits.max_chunk_line) {
                d.fail(ec, Errc::bad_chunk);
                return Result::need_more;
            }

            std::uint64_t size = 0;
            if (!parse_chunk_size(std::string_view{p, line_len}, size)) {
                d.fail(ec, Errc::bad_chunk);
                return Result::need_more;
            }
            d.begin += line_len + 2;

            if (size == 0) {
                d.st = Impl::St::trailers;
                continue;
            }
            d.remaining = size;
            d.st        = Impl::St::chunk_data;
            continue;
        }

        case Impl::St::chunk_data: {
            if (d.avail() == 0) {
                return Result::need_more;
            }
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(d.remaining, d.avail()));

            if (d.cfg.body_mode == BodyMode::stream) {
                d.chunk_view = span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(d.data()), take};
                d.begin += take;
                d.remaining -= take;
                if (d.remaining == 0) {
                    d.st = Impl::St::chunk_crlf;
                }
                return Result::body_chunk;
            }

            if (d.body_owned.size() + take > d.cfg.limits.max_body_bytes) {
                d.fail(ec, Errc::body_too_large);
                return Result::need_more;
            }
            d.body_owned.append(d.data(), take);
            d.begin += take;
            d.remaining -= take;
            if (d.remaining == 0) {
                d.st = Impl::St::chunk_crlf;
            }
            continue;
        }

        case Impl::St::chunk_crlf: {
            if (d.avail() < 2) {
                return Result::need_more;
            }
            if (d.data()[0] != '\r' || d.data()[1] != '\n') {
                d.fail(ec, Errc::bad_chunk);
                return Result::need_more;
            }
            d.begin += 2;
            d.st = Impl::St::chunk_size;
            continue;
        }

        case Impl::St::trailers: {
            const char*       p    = d.data();
            const std::size_t n    = d.avail();
            const char*       crlf = nullptr;
            for (std::size_t i = 0; i + 1 < n; ++i) {
                if (p[i] == '\r' && p[i + 1] == '\n') {
                    crlf = p + i;
                    break;
                }
            }
            if (crlf == nullptr) {
                if (n > d.cfg.limits.max_chunk_line) {
                    d.fail(ec, Errc::bad_chunk);
                }
                return Result::need_more;
            }
            const std::size_t line_len = static_cast<std::size_t>(crlf - p);
            if (line_len > d.cfg.limits.max_chunk_line) {
                d.fail(ec, Errc::bad_chunk);
                return Result::need_more;
            }
            d.begin += line_len + 2;
            if (line_len == 0) {
                d.finish_message();
            }
            continue;   // 非空行是 trailer，丢弃
        }

        case Impl::St::done: {
            if (d.cfg.body_mode == BodyMode::stream) {
                if (!d.headers_emitted) {
                    d.headers_emitted = true;
                    return Result::headers;
                }
                return Result::message_end;
            }
            return Result::message;
        }
        }
        return Result::need_more;
    }
}

const HttpRequest& Parser::request() const noexcept
{
    return m_impl->req;
}

const HttpResponse& Parser::response() const noexcept
{
    return m_impl->resp;
}

HttpRequest& Parser::request_mut() noexcept
{
    return m_impl->req;
}

span<const std::uint8_t> Parser::body_chunk() const noexcept
{
    return m_impl->chunk_view;
}

void Parser::reset() noexcept
{
    Impl& d = *m_impl;

    d.st              = Impl::St::headers;
    d.remaining       = 0;
    d.headers_emitted = false;
    d.chunk_view      = {};
    d.fields.clear();
    d.body_owned.clear();
    d.header_owned.clear();
    d.last_scan = 0;
    d.failed    = false;
}

span<const std::uint8_t> Parser::pending_bytes() const noexcept
{
    const Impl& d = *m_impl;
    return span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(d.buf.data() + d.begin), d.avail()};
}

std::size_t Parser::pending() const noexcept
{
    return m_impl->avail();
}

std::uint64_t Parser::messages() const noexcept
{
    return m_impl->messages;
}

} // namespace httpmech
