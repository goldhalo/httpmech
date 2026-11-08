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
#include <random>
#include <limits>
#include <string_view>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace httpmech {

namespace {

/// steady_clock 的纳秒计数（时间戳要放进原子量，所以统一用 int64）。
std::int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

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

/// close code：协议错误（RFC 6455 §7.4.1 定义的 1002）。
constexpr std::uint16_t kCloseProtocolError = 1002;

/// 追加一帧（服务端发出的帧**不打掩码**）。
void append_frame(std::string& out, std::uint8_t opcode, bool fin,
                  const std::uint8_t* payload, std::size_t len,
                  const std::uint8_t* mask /* nullptr = 不打掩码 */)
{
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

    const std::uint8_t mask_bit = (mask != nullptr) ? 0x80 : 0x00;
    if (len < 126) {
        out.push_back(static_cast<char>(mask_bit | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    if (mask != nullptr) {
        out.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        return;
    }
    if (len != 0) {
        out.append(reinterpret_cast<const char*>(payload), len);
    }
}

} // namespace

// =============================================================================
//  会话
// =============================================================================

WsSessionImpl::WsSessionImpl(Role role, WsIo io, WebSocketConfig cfg, WebSocketHandlers handlers,
                             std::string path, std::string real_ip, Endpoint peer,
                             std::string handshake_headers_raw)
    : m_role(role)
    , m_io(std::move(io))
    , m_cfg(std::move(cfg))
    , m_handlers(std::move(handlers))
    , m_path(std::move(path))
    , m_real_ip(std::move(real_ip))
    , m_peer(peer)
    , m_headers_raw(std::move(handshake_headers_raw))
{
    m_last_rx_ns.store(now_ns(), std::memory_order_relaxed);
    m_last_ping_ns.store(m_last_rx_ns.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);

    // 掩码用的种子：同一进程里的多个连接不该共用同一串掩码。
    // 这里只求"不重复"，不追求密码学强度（RFC 只要求不可预测到防止
    // 中间层做缓存投毒，对设备内网场景足够）。
    std::random_device rd;
    m_rng.store((static_cast<std::uint32_t>(rd()) ^ 0x9E3779B9u) | 1u,
                std::memory_order_relaxed);
}

std::size_t WsSessionImpl::pending_send_bytes() const noexcept
{
    return m_io.pending ? m_io.pending() : 0;
}

void WsSessionImpl::start()
{
    if (m_handlers.on_open) {
        m_handlers.on_open(*this);
    }
}

bool WsSessionImpl::is_open() const noexcept
{
    return m_state.load(std::memory_order_acquire) == State::open;
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
    if (m_state.load(std::memory_order_acquire) == State::closed) {
        return false;
    }
    std::string wire;
    wire.reserve(payload.size() + 14);

    std::uint8_t mask[4] = {0, 0, 0, 0};
    if (m_role == Role::client) {
        // xorshift：够用且无依赖
        // 原子递增取数：掩码只要求"每帧不同"，不要求密码学强度。
        const std::uint32_t k = m_rng.fetch_add(0x9E3779B9u, std::memory_order_relaxed);
        mask[0] = static_cast<std::uint8_t>((k >> 24) & 0xFF);
        mask[1] = static_cast<std::uint8_t>((k >> 16) & 0xFF);
        mask[2] = static_cast<std::uint8_t>((k >> 8) & 0xFF);
        mask[3] = static_cast<std::uint8_t>(k & 0xFF);
    }
    append_frame(wire, opcode, fin, payload.data(), payload.size(),
                 (m_role == Role::client) ? mask : nullptr);

    const bool ok = m_io.send ? m_io.send(std::move(wire), close_after) : false;
    if (!ok) {
        // 两种可能：底层反压（正常，重试即可），或者传输已经关死了。
        // 后者必须走完整的关闭流程 —— 否则应用永远等不到 on_close，
        // 只能看着 is_open() 变 false 却不知道发生了什么。
        if (m_io.is_closed && m_io.is_closed()) {
            fail_transport();
        }
        return false;
    }
    return true;
}

bool WsSessionImpl::send_text(std::string_view text)
{
    if (m_cfg.max_send_queue_bytes != 0 && pending_send_bytes() > m_cfg.max_send_queue_bytes) {
        return false;   // 背压：队列已经堆满，交给应用自己决定丢弃还是等
    }
    if (m_cfg.max_frame_bytes != 0 && text.size() > m_cfg.max_frame_bytes) {
        return false;   // 单帧超限：我们不做发送侧分片，超了就是发不了
    }
    return send_frame(kOpText, true,
                      span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                               text.size()});
}

bool WsSessionImpl::send_binary(span<const std::uint8_t> data)
{
    if (m_cfg.max_send_queue_bytes != 0 && pending_send_bytes() > m_cfg.max_send_queue_bytes) {
        return false;
    }
    if (m_cfg.max_frame_bytes != 0 && data.size() > m_cfg.max_frame_bytes) {
        return false;   // 同上：超单帧上限
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
    if (m_state.load(std::memory_order_acquire) != State::open) {
        return;
    }
    bool expected = false;
    if (!m_close_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;   // 别人已经在关了
    }
    m_state.store(State::closing, std::memory_order_release);

    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    const std::size_t take = std::min<std::size_t>(reason.size(), 123);
    payload.append(reason.data(), take);

    // 先发 close 帧，等对端回帧（tick 里超时兜底）
    const bool sent = send_frame(
        kOpClose, true,
        span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 payload.size()});
    if (!sent) {
        // close 帧根本没发出去：握手已经不可能完成，直接断，别让应用干等 2 秒。
        fail_transport();
        return;
    }
    m_close_deadline_ns.store(now_ns() + 2'000'000'000LL, std::memory_order_release);
}

void WsSessionImpl::protocol_error(std::uint16_t code, std::string_view why)
{
    close_with_frame(code, why);
}

void WsSessionImpl::fail_transport()
{
    // 与 on_transport_closed() 共用同一道闸：谁先到谁负责通知，另一个直接返回。
    if (m_state.exchange(State::closed, std::memory_order_acq_rel) == State::closed) {
        return;
    }
    if (m_handlers.on_close) {
        m_handlers.on_close(*this, static_cast<std::uint16_t>(WsCloseCode::abnormal),
                            "transport lost");
    }
    if (m_io.close) {
        m_io.close();
    }
}

void WsSessionImpl::close_with_frame(std::uint16_t code, std::string_view why)
{
    // 「发 close 帧（写完关传输）→ 带真实 code 通知应用 → 再置 closed」。
    // 协议错误与空闲超时共用这条收尾路径。
    //
    // on_transport_closed() 是正常关闭的收口（它只能报 abnormal 1006），
    // 所以这里自己发「close 帧 + 写完后关传输」，然后直接回调并置 closed：
    //   · close 帧用 close_after=true 交给底层，保证先写出去再关 TCP；
    //   · 回调后再置 closed，随后的 on_transport_closed() 看到 closed 会返回，
    //     不会第二次通知。
    //
    // 以前这里先置 closed 再关传输：close 帧可能没写出去，且 on_transport_closed()
    // 以为自己来晚了 —— 所有协议错误（RSV 非 0、掩码错、未知 opcode、close code
    // 非法、分片错乱…）一次 on_close 都不回调，应用靠它清理的状态全部残留。
    if (m_state.load(std::memory_order_acquire) == State::closed) {
        return;
    }
    m_state.store(State::closing, std::memory_order_release);
    m_close_sent.store(true, std::memory_order_release);

    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    const std::size_t take = std::min<std::size_t>(why.size(), 123);
    payload.append(why.data(), take);

    const bool sent = send_frame(
        kOpClose, true,
        span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 payload.size()},
        true /* close_after：写完 close 帧就关传输 */);
    if (!sent) {
        // 连 close 帧都发不出去：按传输丢失收口（fail_transport 报 abnormal）。
        fail_transport();
        return;
    }

    m_state.store(State::closed, std::memory_order_release);
    if (m_handlers.on_close) {
        m_handlers.on_close(*this, code, why);
    }
}

void WsSessionImpl::on_bytes(span<const std::uint8_t> data)
{
    if (m_state.load(std::memory_order_acquire) == State::closed) {
        return;
    }
    m_in.append(reinterpret_cast<const char*>(data.data()), data.size());
    m_last_rx_ns.store(now_ns(), std::memory_order_relaxed);

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
        if (m_state.load(std::memory_order_acquire) == State::closed) {
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
    // 掩码规则两端相反：服务端收到的必须带，客户端收到的必须不带。
    if (m_role == Role::server ? !masked : masked) {
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
        // RFC 6455 §5.2：64 位长度最高位必须为 0。不校验的话后续
        // `off + key_len + len` 会回绕，按超大长度构造 std::string 直接崩。
        if ((len & 0x8000000000000000ULL) != 0) {
            ec = make_error_code(Errc::bad_chunk);
            return;
        }
        off += 8;
    }
    // 即便上限配成 0（不限），也要防 size_t 加法回绕。
    if (len > (std::numeric_limits<std::size_t>::max)() - 16) {
        ec = make_error_code(Errc::bad_chunk);
        return;
    }

    const bool is_control = (opcode & 0x08) != 0;
    if (is_control) {
        if (!fin || len > 125) {
            ec = make_error_code(Errc::bad_chunk);
            return;
        }
    }
    // 0 表示"不限"（与 send_text/send_binary 的语义一致）。
    // 以前这里只写 `len > m_cfg.max_frame_bytes`，用户把上限设成 0 就变成
    // "任何带载荷的帧都被拒"——而发送侧把 0 当不限，两头打架。
    if (m_cfg.max_frame_bytes != 0 && len > m_cfg.max_frame_bytes) {
        protocol_error(static_cast<std::uint16_t>(WsCloseCode::message_too_big), "frame too big");
        m_in.clear();
        return;
    }

    // 只有带掩码的帧才有那 4 字节 key。客户端收到的帧按 RFC 是**不带**的，
    // 早期版本无条件跳 4 字节，结果把 payload 的前 4 字节当掩码吃掉。
    const std::size_t key_len = masked ? 4u : 0u;
    if (n < off + key_len + static_cast<std::size_t>(len)) {
        return;   // 帧还没到齐
    }

    const std::uint8_t* mask    = masked ? (p + off) : nullptr;
    off += key_len;
    const std::uint8_t* payload = p + off;

    std::string data(reinterpret_cast<const char*>(payload), static_cast<std::size_t>(len));
    if (mask != nullptr) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>(static_cast<std::uint8_t>(data[i]) ^ mask[i % 4]);
        }
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
        // RFC 6455 §5.5.1：close 帧载荷要么空，要么 ≥2 字节（2 字节 code + 可选 reason）。
        if (data.size() == 1) {
            protocol_error(kCloseProtocolError, "close 帧载荷只有 1 字节");
            return;
        }
        std::uint16_t code = static_cast<std::uint16_t>(WsCloseCode::no_status);
        std::string   why;
        if (data.size() >= 2) {
            code = static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[0]) << 8) |
                                              static_cast<std::uint8_t>(data[1]));
            // 1005/1006/1015 是"本地状态"用值，**不允许出现在线路上**；
            // 1004 保留、1012-1014 未定义。收到这些只能按协议错误处理。
            const bool valid = (code >= 1000 && code <= 1003) ||
                               (code >= 1007 && code <= 1011) ||
                               (code >= 3000 && code <= 4999);
            if (!valid) {
                protocol_error(kCloseProtocolError, "close code 非法");
                return;
            }
            why.assign(data.data() + 2, data.size() - 2);
        }
        // 回一帧 close 再断。（若我们之前已经主动发过 close，就只剩等对端这一帧。）
        bool echoed = false;
        bool expected_unused = false;
        if (m_close_sent.compare_exchange_strong(expected_unused, true,
                                                std::memory_order_acq_rel)) {
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
        m_state.store(State::closed, std::memory_order_release);
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
            // 0 = 不限（与 max_frame_bytes / send_* 的语义一致）
            if (m_cfg.max_message_bytes != 0 && m_msg.size() > m_cfg.max_message_bytes) {
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
        if (m_cfg.max_message_bytes != 0 && m_msg.size() + data.size() > m_cfg.max_message_bytes) {
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
    m_recv_msgs.fetch_add(1, std::memory_order_relaxed);
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
    // 注意：tick 跑在**服务器的 sweeper 线程**上，而 on_bytes/on_write 在 io 线程。
    // 所以这里读写的每个字段都是原子量；真正发帧那一步走的是线程安全的 send 路径。
    if (m_state.load(std::memory_order_acquire) == State::closed) {
        return;
    }
    const std::int64_t now_ns_v =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    // 关闭握手的兜底：发了 close 但对方不回，2 秒后强关
    const std::int64_t dl = m_close_deadline_ns.load(std::memory_order_acquire);
    if (dl != 0 && now_ns_v >= dl) {
        m_close_deadline_ns.store(0, std::memory_order_release);   // 只触发一次

        // ★ 这里**不能**先 `m_state.store(closed)` 再关。
        //
        //   `on_close` 的契约是"恰好一次"（websocket.hpp），而收口点只有一个：
        //   io 线程上的 `ServerSession::on_close()` → `on_transport_closed()`，
        //   它靠 `exchange(closed)` 判断"我是不是第一个到的"。
        //   以前这里先把状态置成 closed，之后 on_transport_closed 一看已经是
        //   closed 就直接 return —— 于是**这条路径一次都不回调**，
        //   应用靠 on_close 清理的每连接状态（订阅表、会话表）全部残留。
        //
        //   现在只关传输，通知交给 on_transport_closed：
        //   它跑在 io 线程上，顺带满足"回调在该连接的 io 线程"的契约。
        if (m_io.close) {
            m_io.close();
        }
        return;
    }

    // 空闲超时
    const auto idle_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             m_cfg.idle_timeout).count();
    if (idle_ns > 0 &&
        now_ns_v - m_last_rx_ns.load(std::memory_order_relaxed) > idle_ns) {
        // 同上：先发 close 帧（close() 内部若发不出去，自己会走 fail_transport
        // 并回调），再把传输关掉；**不要**在这里置 closed —— 否则随后的
        // on_transport_closed 会以为自己来晚了，on_close 就一次都不会到。
        close_with_frame(static_cast<std::uint16_t>(WsCloseCode::going_away),
                         "idle timeout");
        return;
    }

    // 主动 ping 保活
    const auto ping_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             m_cfg.ping_interval).count();
    if (ping_ns > 0 &&
        now_ns_v - m_last_ping_ns.load(std::memory_order_relaxed) >= ping_ns) {
        m_last_ping_ns.store(now_ns_v, std::memory_order_relaxed);
        send_frame(kOpPing, true, {});
    }
}

void WsSessionImpl::on_transport_closed()
{
    if (m_state.exchange(State::closed, std::memory_order_acq_rel) == State::closed) {
        return;
    }
    if (m_handlers.on_close) {
        m_handlers.on_close(*this, static_cast<std::uint16_t>(WsCloseCode::abnormal), "transport closed");
    }
}

} // namespace detail
} // namespace httpmech
