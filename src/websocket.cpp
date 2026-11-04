// =============================================================================
//  httpmech - WebSocket 实现（握手算法 + 帧层 + 会话）
// =============================================================================
//  RFC 6455：
//    §4.2.2  握手：Sec-WebSocket-Accept = base64(sha1(key + GUID))
//    §5      帧格式：2 字节头 + 可选扩展长度 + 可选掩码 + 载荷
//    §5.5    控制帧：ping / pong / close，不可分片、载荷 ≤125
//    §7.4    关闭码
//
//  两条硬规则（客户端与服务端不对称，写错就是对端直接断）：
//    · 客户端 → 服务端：**必须**带掩码
//    · 服务端 → 客户端：**必须不带**掩码
// =============================================================================
#include "ws_internal.hpp"

#include "sha1_base64.hpp"

#include <httpmech/headers.hpp>


#include <algorithm>
#include <cstring>

namespace httpmech {

std::string make_websocket_accept(std::string_view client_key)
{
    // RFC 6455 §1.3 的魔法字符串
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string buf(client_key);
    buf += kGuid;

    const auto digest = detail::sha1(buf);
    return detail::base64_encode(digest.data(), digest.size());
}

namespace detail {

namespace {

constexpr std::uint8_t kOpCont   = 0x0;
constexpr std::uint8_t kOpText   = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose  = 0x8;
constexpr std::uint8_t kOpPing   = 0x9;
constexpr std::uint8_t kOpPong   = 0xA;

/// 追加一帧（服务端发出的帧**不打掩码**）。
void append_frame(std::string& out, std::uint8_t opcode, bool fin,
                  const std::uint8_t* payload, std::size_t len)
{
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    if (len != 0) {
        out.append(reinterpret_cast<const char*>(payload), len);
    }
}

} // namespace

// =============================================================================
//  会话
// =============================================================================

WsSessionImpl::WsSessionImpl(WsIo io, WebSocketConfig cfg, WebSocketHandlers handlers,
                             std::string path, std::string real_ip, netmech::Address peer,
                             std::string handshake_headers_raw)
    : m_io(std::move(io))
    , m_cfg(std::move(cfg))
    , m_handlers(std::move(handlers))
    , m_path(std::move(path))
    , m_real_ip(std::move(real_ip))
    , m_peer(peer)
    , m_headers_raw(std::move(handshake_headers_raw))
{
    m_last_rx   = std::chrono::steady_clock::now();
    m_last_ping = m_last_rx;
}

void WsSessionImpl::start()
{
    if (m_handlers.on_open) {
        m_handlers.on_open(*this);
    }
}

bool WsSessionImpl::is_open() const noexcept
{
    return m_state == State::open;
}

std::string_view WsSessionImpl::handshake_header(std::string_view name) const noexcept
{
    // 头块是原始文本（"Name: value\r\n..."）。这里做一次线性查找，
    // 只用于 on_open 里读一下 Cookie / Origin 之类，不值得建索引。
    std::size_t pos = 0;
    while (pos < m_headers_raw.size()) {
        const std::size_t eol = m_headers_raw.find("\r\n", pos);
        const std::size_t end = (eol == std::string::npos) ? m_headers_raw.size() : eol;
        const std::string_view line{m_headers_raw.data() + pos, end - pos};
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && iequals(trim_ows(line.substr(0, colon)), name)) {
            return trim_ows(line.substr(colon + 1));
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 2;
    }
    return {};
}

bool WsSessionImpl::send_frame(std::uint8_t opcode, bool fin, span<const std::uint8_t> payload,
                               bool close_after)
{
    if (m_state == State::closed) {
        return false;
    }
    std::string wire;
    wire.reserve(payload.size() + 10);
    append_frame(wire, opcode, fin, payload.data(), payload.size());

    m_pending_out += wire.size();
    const bool ok = m_io.send ? m_io.send(std::move(wire), close_after) : false;
    if (!ok) {
        m_state = State::closed;
    }
    return ok;
}

bool WsSessionImpl::send_text(std::string_view text)
{
    if (m_cfg.max_send_queue_bytes != 0 && m_pending_out > m_cfg.max_send_queue_bytes) {
        return false;   // 背压：队列已经堆满，交给应用自己决定丢弃还是等
    }
    return send_frame(kOpText, true,
                      span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                               text.size()});
}

bool WsSessionImpl::send_binary(span<const std::uint8_t> data)
{
    if (m_cfg.max_send_queue_bytes != 0 && m_pending_out > m_cfg.max_send_queue_bytes) {
        return false;
    }
    return send_frame(kOpBinary, true, data);
}

bool WsSessionImpl::send_ping(span<const std::uint8_t> payload)
{
    if (payload.size() > 125) {
        return false;   // 控制帧载荷硬上限
    }
    return send_frame(kOpPing, true, payload);
}

void WsSessionImpl::close(std::uint16_t code, std::string_view reason)
{
    if (m_state != State::open || m_close_sent) {
        return;
    }
    m_close_sent = true;
    m_state      = State::closing;

    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    const std::size_t take = std::min<std::size_t>(reason.size(), 123);
    payload.append(reason.data(), take);

    // 先发 close 帧，等对端回帧（tick 里超时兜底）
    send_frame(kOpClose, true,
               span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                        payload.size()});
    m_close_deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    m_close_deadline_set = true;
}

void WsSessionImpl::protocol_error(std::uint16_t code, std::string_view why)
{
    close(code, why);
    m_state = State::closed;
    if (m_io.close) {
        m_io.close();
    }
}

void WsSessionImpl::on_bytes(span<const std::uint8_t> data)
{
    if (m_state == State::closed) {
        return;
    }
    m_in.append(reinterpret_cast<const char*>(data.data()), data.size());
    m_last_rx = std::chrono::steady_clock::now();

    for (;;) {
        const std::size_t before = m_in.size();
        std::error_code   ec;
        feed_one_frame(ec);
        if (ec) {
            protocol_error(static_cast<std::uint16_t>(WsCloseCode::protocol_error), ec.message());
            return;
        }
        if (m_in.size() == before) {
            break;   // 没有进展：数据不够一帧
        }
        if (m_state == State::closed) {
            return;
        }
    }
}

void WsSessionImpl::feed_one_frame(std::error_code& ec)
{
    const auto* p   = reinterpret_cast<const std::uint8_t*>(m_in.data());
    const std::size_t n = m_in.size();
    if (n < 2) {
        return;
    }

    const bool         fin    = (p[0] & 0x80) != 0;
    const std::uint8_t rsv    = static_cast<std::uint8_t>(p[0] & 0x70);
    const std::uint8_t opcode = static_cast<std::uint8_t>(p[0] & 0x0F);
    const bool         masked = (p[1] & 0x80) != 0;
    std::uint64_t      len    = static_cast<std::uint64_t>(p[1] & 0x7F);
    std::size_t        off    = 2;

    if (rsv != 0) {
        ec = make_error_code(Errc::bad_chunk);   // 没有协商任何扩展，RSV 必须为 0
        return;
    }
    if (!masked) {
        // 客户端→服务端必须打掩码
        ec = make_error_code(Errc::bad_chunk);
        return;
    }

    if (len == 126) {
        if (n < off + 2) {
            return;
        }
        len = (static_cast<std::uint64_t>(p[off]) << 8) | p[off + 1];
        off += 2;
    } else if (len == 127) {
        if (n < off + 8) {
            return;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | p[off + i];
        }
        off += 8;
    }

    const bool is_control = (opcode & 0x08) != 0;
    if (is_control) {
        if (!fin || len > 125) {
            ec = make_error_code(Errc::bad_chunk);
            return;
        }
    }
    if (len > m_cfg.max_frame_bytes) {
        protocol_error(static_cast<std::uint16_t>(WsCloseCode::message_too_big), "frame too big");
        m_in.clear();
        return;
    }

    if (n < off + 4 + static_cast<std::size_t>(len)) {
        return;   // 帧还没到齐
    }

    const std::uint8_t* mask = p + off;
    off += 4;
    const std::uint8_t* payload = p + off;

    std::string data(reinterpret_cast<const char*>(payload), static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<std::uint8_t>(data[i]) ^ mask[i % 4]);
    }

    m_in.erase(0, off + static_cast<std::size_t>(len));

    switch (opcode) {
    case kOpPing:
        send_frame(kOpPong, true,
                   span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(data.data()),
                                            data.size()});
        return;

    case kOpPong:
        return;   // 只用于刷新 m_last_rx（上面已刷）

    case kOpClose: {
        std::uint16_t code = static_cast<std::uint16_t>(WsCloseCode::no_status);
        std::string   why;
        if (data.size() >= 2) {
            code = static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[0]) << 8) |
                                              static_cast<std::uint8_t>(data[1]));
            why.assign(data.data() + 2, data.size() - 2);
        }
        // 回一帧 close 再断。（若我们之前已经主动发过 close，就只剩等对端这一帧。）
        bool echoed = false;
        if (!m_close_sent) {
            m_close_sent = true;
            std::string echo;
            echo.push_back(data.size() >= 2 ? data[0] : static_cast<char>(0x03));
            echo.push_back(data.size() >= 2 ? data[1] : static_cast<char>(0xE8));   // 1000
            send_frame(kOpClose, true,
                       span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(echo.data()),
                                                echo.size()},
                       true /* close_after */);
            echoed = true;
        }
        const std::uint16_t report = (data.size() >= 2) ? code
                                                        : static_cast<std::uint16_t>(WsCloseCode::no_status);
        if (m_handlers.on_close) {
            m_handlers.on_close(*this, report, why);
        }
        m_state = State::closed;
        // 刚回过 close 的情况：那一帧是 close_after=true 排进队列的，
        // **不能**再调 io.close() —— 会把还没写出去的 close 帧一起丢掉，
        // 对端只能看到 TCP 被掐断（RFC 6455 里这叫 abnormal closure）。
        // 反之（我们早就发过 close，这次只是收到对端的回应）才需要收尾。
        if (!echoed && m_io.close) {
            m_io.close();
        }
        return;
    }

    case kOpText:
    case kOpBinary: {
        if (m_in_fragment) {
            ec = make_error_code(Errc::bad_chunk);   // 上一个分片还没收完就来了新消息头
            return;
        }
        if (fin) {
            deliver_message(std::move(data), opcode == kOpText);
        } else {
            m_in_fragment = true;
            m_msg_is_text = (opcode == kOpText);
            m_msg         = std::move(data);
            if (m_msg.size() > m_cfg.max_message_bytes) {
                protocol_error(static_cast<std::uint16_t>(WsCloseCode::message_too_big),
                               "message too big");
            }
        }
        return;
    }

    case kOpCont: {
        if (!m_in_fragment) {
            ec = make_error_code(Errc::bad_chunk);   // 没有起始帧
            return;
        }
        if (m_msg.size() + data.size() > m_cfg.max_message_bytes) {
            protocol_error(static_cast<std::uint16_t>(WsCloseCode::message_too_big),
                           "message too big");
            return;
        }
        m_msg += data;
        if (fin) {
            m_in_fragment = false;
            std::string whole = std::move(m_msg);
            m_msg.clear();
            deliver_message(std::move(whole), m_msg_is_text);
        }
        return;
    }

    default:
        ec = make_error_code(Errc::bad_chunk);   // 未知 opcode
        return;
    }
}

void WsSessionImpl::deliver_message(std::string&& payload, bool is_text)
{
    ++m_recv_msgs;
    if (!m_handlers.on_message) {
        return;
    }
    m_handlers.on_message(*this,
                          span<const std::uint8_t>{
                              reinterpret_cast<const std::uint8_t*>(payload.data()),
                              payload.size()},
                          is_text);
}

void WsSessionImpl::tick(std::chrono::steady_clock::time_point now)
{
    if (m_state == State::closed) {
        return;
    }

    // 关闭握手的兜底：发了 close 但对方不回，2 秒后强关
    if (m_close_deadline_set && now >= m_close_deadline) {
        m_state = State::closed;
        if (m_io.close) {
            m_io.close();
        }
        return;
    }

    // 空闲超时
    if (m_cfg.idle_timeout.count() > 0 && now - m_last_rx > m_cfg.idle_timeout) {
        close(static_cast<std::uint16_t>(WsCloseCode::going_away), "idle timeout");
        m_state = State::closed;
        if (m_io.close) {
            m_io.close();
        }
        return;
    }

    // 主动 ping 保活
    if (m_cfg.ping_interval.count() > 0 && now - m_last_ping >= m_cfg.ping_interval) {
        m_last_ping = now;
        send_frame(kOpPing, true, {});
    }
}

void WsSessionImpl::on_transport_closed()
{
    if (m_state == State::closed) {
        return;
    }
    m_state = State::closed;
    if (m_handlers.on_close) {
        m_handlers.on_close(*this, static_cast<std::uint16_t>(WsCloseCode::abnormal), "transport closed");
    }
}

} // namespace detail
} // namespace httpmech
