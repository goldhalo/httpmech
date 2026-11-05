// =============================================================================
//  httpmech - 服务端实现
// =============================================================================
//  结构：
//    ResponseWriter::Impl   一次响应的状态（状态码 / 头 / body / 是否已发）
//    HttpServer::Impl       配置、监听、会话表、统计、空闲清理线程
//    detail::ServerSession  一条连接的完整生命周期（读 → 解析 → 派发 → 写）
//
//  读模型（来自 netmech 的约定）：
//    · 一笔 read() 必须显式再次发起，库不会自动续读；
//    · Buf 由我们提供，回调里原样传回，所以每连接一块读缓冲就够用；
//    · 解析器会把收到的字节**拷进自己的缓冲**，所以读缓冲可以立刻复用。
//
//  v0.1 的取舍：响应整块缓冲后按 Content-Length 发。
//    头文件里提到的"超 max_buffered_response 自动 chunked"尚未实现 ——
//    它要求 header 在第一次 write 时就落地，属于流式响应，下一阶段再做。
//    当前行为：响应全部缓存，end() 时一次算出长度发出。对 JSON 接口足够。
// =============================================================================
#include <httpmech/server.hpp>

#include "net_bridge.hpp"
#include "ws_internal.hpp"

#include <netmech/conn.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httpmech {

namespace detail {
class ServerSession;
} // namespace detail

// =============================================================================
//  响应写入器的内部状态
// =============================================================================

struct ResponseWriter::Impl {
    detail::ServerSession* session  = nullptr;
    const HttpRequest*     request  = nullptr;
    Status                 status   = Status::ok;
    std::uint16_t          code     = 200;
    std::string            reason;
    Headers                headers;
    std::string            body;
    bool                   ended    = false;   ///< 应用调过 end()
    bool                   sent     = false;   ///< 已经落成字节发出去
    bool                   aborted  = false;
    bool                   upgraded = false;
    bool                   no_body  = false;   ///< HEAD：算长度但不发体

    void reset_for_request(bool head_request)
    {
        status   = Status::ok;
        code     = 200;
        reason.clear();
        headers.clear();
        body.clear();
        ended    = false;
        sent     = false;
        aborted  = false;
        upgraded = false;
        no_body  = head_request;
    }
};

// =============================================================================
//  服务端内部状态
// =============================================================================

struct HttpServer::Impl {
    std::shared_ptr<netmech::Mechanism> mech;
    ServerConfig                        cfg;
    std::shared_ptr<RequestHandler>     handler;

    std::atomic<bool>          running{false};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> proto_errors{0};
    std::atomic<std::size_t>   conns{0};

    std::shared_ptr<netmech::Linkable>                bridge;
    Endpoint                                          bound;   ///< 实际监听地址

    std::mutex                                        sessions_mu;
    std::vector<std::weak_ptr<detail::ServerSession>> sessions;

    // 空闲清理线程：netmech 没有对外的定时器，这里用一个 1 秒节拍的线程兜底
    std::thread             sweeper;
    std::mutex              sleep_mu;
    std::condition_variable sleep_cv;
    bool                    stop_sweeper = false;

    void add_session(const std::shared_ptr<detail::ServerSession>& s)
    {
        std::lock_guard<std::mutex> lk(sessions_mu);
        sessions.emplace_back(s);
    }

    void sweep_loop();
    void stop_sweeper_thread();
};

namespace detail {

// =============================================================================
//  一条连接
// =============================================================================

class ServerSession : public netmech::ConnHandler,
                      public std::enable_shared_from_this<ServerSession> {
public:
    ServerSession(std::shared_ptr<HttpServer::Impl> owner, netmech::Address peer)
        : m_owner(std::move(owner))
        , m_peer(peer)
        , m_parser(httpmech::Parser::Role::server, make_parser_cfg(m_owner->cfg))
    {
        m_read_buf        = std::make_shared<std::vector<char>>(kReadChunk);
        m_writer.session  = this;
        m_last_activity   = std::chrono::steady_clock::now();
    }

    ~ServerSession() override = default;

    ServerSession(const ServerSession&)            = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    ResponseWriter::Impl* writer_state() noexcept { return &m_writer; }
    const ServerConfig&   cfg() const noexcept { return m_owner->cfg; }
    netmech::Address      peer_address() const noexcept { return m_peer; }
    Endpoint              peer() const noexcept { return detail::from_address(m_peer); }

    // --- netmech::ConnHandler ---

    bool on_open(std::shared_ptr<netmech::Conn> c) override
    {
        m_conn = std::move(c);
        m_owner->conns.fetch_add(1, std::memory_order_relaxed);
        return start_read();
    }

    void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override
    {
        if (!m_conn || m_closed.load(std::memory_order_relaxed)) {
            return;
        }
        if (ec) {
            close_now();
            return;
        }
        m_last_activity = std::chrono::steady_clock::now();
        m_busy          = true;

        const auto* p = static_cast<const std::uint8_t*>(buf.data);
        if (m_is_ws) {
            if (m_ws) {
                m_ws->on_bytes(span<const std::uint8_t>{p, n});
            }
        } else {
            m_parser.feed(span<const std::uint8_t>{p, n}, m_ec);
            pump();
        }

        m_busy = false;
        if (m_closed.load(std::memory_order_relaxed)) {
            return;
        }
        if (m_close_after_all.load(std::memory_order_acquire)) {
            // 有"写完就关"的报文在途：**不能在这里关**，
            // 否则那帧还没写出去就被掐断（WS 的 close 帧就栽在这上面）。
            // 交给 on_write：全部落地后由它收尾。
            return;
        }
        if (!start_read()) {
            close_now();
        }
    }

    void on_write(std::error_code ec, netmech::ConstBuf buf, std::size_t /*n*/) override
    {
        m_inflight_bytes.fetch_sub(buf.size, std::memory_order_acq_rel);
        const auto left = m_inflight.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (ec) {
            close_now();
            return;
        }
        // "写完就关"：必须等**所有**已受理的写都落地。
        // 早先这是个 bool 标志 + 队列，谁先完成谁就关 —— close 帧排在别人后面时会被吞掉。
        if (left == 0 && m_close_after_all.load(std::memory_order_acquire)) {
            close_now();
        }
    }

    void on_close() override
    {
        if (m_ws) {
            m_ws->on_transport_closed();
        }
        {
            std::lock_guard<std::mutex> lk(m_conn_mu);
            m_conn.reset();
        }
        if (!m_closed.exchange(true)) {
            m_owner->conns.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // --- 空闲检查（HttpServer 的清理线程调用）---

    void check_idle(std::chrono::milliseconds idle)
    {
        if (m_closed.load(std::memory_order_relaxed) || m_busy) {
            return;
        }
        if (m_is_ws) {
            if (m_ws) {
                m_ws->tick(std::chrono::steady_clock::now());
            }
            return;   // WS 的空闲超时由 WebSocketConfig 管
        }
        if (std::chrono::steady_clock::now() - m_last_activity > idle) {
            close_now();
        }
    }

    // --- ResponseWriter / WebSocket 调用 ---

    /// @return false 表示未受理（已关闭 / 反压），调用方自行决定后续动作。
    bool send(std::string wire, bool close_after);

    /// 传输是否已经关死（WS 层靠它区分"反压"和"连接没了"）。
    bool closed_flag() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// 已受理但未写完的字节数（WS 层拿它做背压判据）。
    std::size_t inflight_bytes() const noexcept
    {
        return m_inflight_bytes.load(std::memory_order_relaxed);
    }

    /// 帧层发原始字节。返回 false 表示连接已不可写。
    bool send_bytes(std::string wire, bool close_after)
    {
        return send(std::move(wire), close_after);
    }

    /// 升级为 WebSocket：建帧层会话并回调 on_open。
    ///
    /// @param[in] headers_raw 握手请求的头部原文（供 handshake_header() 查）
    bool switch_to_websocket(WebSocketHandlers handlers, WebSocketConfig wcfg,
                             std::string path, std::string real_ip, std::string headers_raw);

    void close_now() noexcept
    {
        if (m_closed.exchange(true)) {
            return;
        }
        // 锁内只取 shared_ptr，**不在持锁时调 close()** ——
        // close() 可能同步回调到 on_close，on_close 又要拿这把锁。
        std::shared_ptr<netmech::Conn> c;
        {
            std::lock_guard<std::mutex> lk(m_conn_mu);
            c = m_conn;
        }
        if (c) {
            c->close();
        }
    }

    /// 锁内取一份 m_conn，供发送路径使用。
    std::shared_ptr<netmech::Conn> conn_snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_conn_mu);
        return m_conn;
    }

private:
    static constexpr std::size_t kReadChunk = 16 * 1024;

    static httpmech::Parser::Config make_parser_cfg(const ServerConfig& cfg)
    {
        httpmech::Parser::Config pc;
        pc.limits              = cfg.limits;
        pc.body_mode           = cfg.body_mode;
        pc.initial_buffer_size = 8 * 1024;
        return pc;
    }

    bool start_read();
    void pump();
    void dispatch_aggregate();
    void dispatch_stream_events(span<const std::uint8_t> body, bool is_end);
    void respond_protocol_error(Status code);
    void finish_writer(bool keep_alive);

    std::shared_ptr<HttpServer::Impl> m_owner;
    std::shared_ptr<netmech::Conn>    m_conn;
    netmech::Address                  m_peer;

    httpmech::Parser            m_parser;
    std::error_code             m_ec;
    ResponseWriter::Impl        m_writer;

    /// 升级成 WebSocket 之后，字节不再进 HTTP 解析器
    bool                                        m_is_ws = false;
    std::shared_ptr<detail::WsSessionImpl>      m_ws;

    std::shared_ptr<std::vector<char>>       m_read_buf;
    // 发送路径可以被多个线程同时进入（应用的 send_*()、sweeper 线程的自动 ping）。
    // 所以这里**不留任何非原子簿记**：
    //   · 缓冲的存活由 netmech 的 ConstBuf::owner 负责，本层不持有队列；
    //   · 在途笔数用原子计数，"写完就关"用原子标志 + 计数归零判定。
    mutable std::mutex        m_conn_mu;                          // 保护 m_conn 的读写
    std::atomic<std::size_t>  m_inflight{0};                      // 已受理、未回调的写笔数
    std::atomic<std::size_t>  m_inflight_bytes{0};                // 同一批的字节总数（背压判据）
    std::atomic<bool>         m_close_after_all{false};           // 全部落地后关闭

    std::atomic<bool> m_closed{false};
    bool              m_stream_headers_done = false;

    bool              m_busy              = false;
    std::uint64_t     m_served            = 0;

    std::chrono::steady_clock::time_point m_last_activity;
};

// =============================================================================
//  小工具
// =============================================================================

namespace {

/// IMF-fixdate：`Sun, 06 Nov 1994 08:49:37 GMT`
void append_date_header(std::string& out)
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    static const char* kDays[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[tm.tm_wday % 7], tm.tm_mday, kMonths[tm.tm_mon % 12],
                  tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    out.append(buf);
}

/// 对端地址是否在受信任代理列表里。
///
/// 支持 "127.0.0.1"（精确）与 IPv4 前缀 "10.0.0.0/8"。
/// 只做 IPv4 比较：部署里 nginx 与进程同机（回环）或同网段，够用。
bool is_trusted_proxy(const netmech::Address& peer, const std::vector<std::string>& list)
{
    if (list.empty()) {
        return false;
    }
    const std::string ip = peer.address_string();
    if (ip.find(':') != std::string::npos) {
        return false;   // IPv6 暂不支持前缀匹配
    }

    for (const auto& raw : list) {
        if (raw.empty()) {
            continue;
        }
        const std::size_t slash = raw.find('/');
        if (slash == std::string::npos) {
            if (raw == ip) {
                return true;
            }
            continue;
        }
        // CIDR：只比较前 prefix/8 个八位组
        const std::string  base = raw.substr(0, slash);
        const unsigned     plen = static_cast<unsigned>(std::strtoul(raw.c_str() + slash + 1, nullptr, 10));
        if (plen > 32) {
            continue;
        }
        const unsigned octets = plen / 8;
        std::size_t a = 0, b = 0;
        unsigned    matched = 0;
        bool        ok      = true;
        while (matched < octets) {
            const std::size_t ae = base.find('.', a);
            const std::size_t be = ip.find('.', b);
            if (ae == std::string::npos || be == std::string::npos) {
                ok = false;
                break;
            }
            if (base.substr(a, ae - a) != ip.substr(b, be - b)) {
                ok = false;
                break;
            }
            a = ae + 1;
            b = be + 1;
            ++matched;
        }
        if (ok && matched == octets) {
            return true;
        }
    }
    return false;
}

} // namespace

// =============================================================================
//  ServerSession 实现
// =============================================================================

bool ServerSession::start_read()
{
    if (!m_conn || m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    netmech::Buf buf;
    buf.owner = m_read_buf;
    buf.data  = m_read_buf->data();
    buf.size  = m_read_buf->size();
    return m_conn->read(buf, netmech::ReadMode::some);
}

void ServerSession::pump()
{
    for (;;) {
        const auto r = m_parser.next(m_ec);

        if (m_ec) {
            m_owner->proto_errors.fetch_add(1, std::memory_order_relaxed);
            if (m_owner->handler) {
                m_owner->handler->on_protocol_error(m_parser.request(), m_ec);
            }
            Status code = Status::bad_request;
            switch (static_cast<Errc>(m_ec.value())) {
            case Errc::header_too_large: code = Status::header_fields_too_large; break;
            case Errc::too_many_headers: code = Status::header_fields_too_large; break;
            case Errc::body_too_large:   code = Status::payload_too_large; break;
            case Errc::unsupported_transfer_coding: code = Status::not_implemented; break;
            default: break;
            }
            respond_protocol_error(code);
            return;
        }

        switch (r) {
        case httpmech::Parser::Result::need_more:
            return;   // 等下一批数据

        case httpmech::Parser::Result::message:
            dispatch_aggregate();
            if (m_is_ws) {
                // 已经升级成 WebSocket：HTTP 解析器退出舞台，
                // 后面的字节全部交给帧层（on_read 里分流）。
                return;
            }
            if (m_closed.load(std::memory_order_relaxed) ||
                m_close_after_all.load(std::memory_order_acquire)) {
                return;
            }
            break;

        case httpmech::Parser::Result::headers:
            dispatch_stream_events({}, false);
            break;

        case httpmech::Parser::Result::body_chunk:
            dispatch_stream_events(m_parser.body_chunk(), false);
            break;

        case httpmech::Parser::Result::message_end:
            dispatch_stream_events({}, true);
            if (m_closed.load(std::memory_order_relaxed) ||
                m_close_after_all.load(std::memory_order_acquire)) {
                return;
            }
            break;
        }
    }
}

void ServerSession::dispatch_aggregate()
{
    HttpRequest& req = m_parser.request_mut();

    // 代理头识别：只有对端在 trusted_proxies 里，X-Real-IP 一类才被采信
    req.from_trusted_proxy = is_trusted_proxy(m_peer, m_owner->cfg.trusted_proxies);

    m_writer.reset_for_request(req.method == Method::head);
    m_writer.request = &req;

    m_owner->requests.fetch_add(1, std::memory_order_relaxed);
    ++m_served;

    if (m_owner->handler) {
        ResponseWriter out{&m_writer};
        m_owner->handler->on_request(req, out);
    }
    finish_writer(req.keep_alive);
}

void ServerSession::dispatch_stream_events(span<const std::uint8_t> body, bool is_end)
{
    HttpRequest& req = m_parser.request_mut();

    if (!m_stream_headers_done) {
        m_stream_headers_done = true;
        m_writer.reset_for_request(req.method == Method::head);
        m_writer.request = &req;
        m_owner->requests.fetch_add(1, std::memory_order_relaxed);
        ++m_served;

        ResponseWriter out{&m_writer};
        if (m_owner->handler && !m_owner->handler->on_headers(req, out)) {
            // 应用拒收：直接回 413 并关连接（省带宽）
            m_writer.code   = static_cast<std::uint16_t>(Status::payload_too_large);
            m_writer.reason = reason_phrase(Status::payload_too_large);
            finish_writer(false);
            return;
        }
        if (!body.empty() && m_owner->handler) {
            m_owner->handler->on_body_data(req, body);
        }
        return;
    }

    if (!is_end) {
        if (!body.empty() && m_owner->handler) {
            m_owner->handler->on_body_data(req, body);
        }
        return;
    }

    if (m_owner->handler) {
        ResponseWriter out{&m_writer};
        m_owner->handler->on_body_end(req, out);
    }
    finish_writer(req.keep_alive);
    m_stream_headers_done = false;
}

void ServerSession::respond_protocol_error(Status code)
{
    m_writer.reset_for_request(false);
    m_writer.code   = static_cast<std::uint16_t>(code);
    m_writer.reason = reason_phrase(code);
    m_writer.headers.set("Content-Length", "0");
    finish_writer(false);
}

void ServerSession::finish_writer(bool keep_alive)
{
    if (m_writer.sent || m_writer.aborted) {
        return;
    }
    m_writer.sent = true;

    const ServerConfig& cfg = m_owner->cfg;

    if (!m_writer.reason.empty()) {
        // 应用自定义原因短语，原样用
    } else {
        m_writer.reason = reason_phrase(status_from(m_writer.code));
    }

    const bool too_many = (cfg.max_requests_per_connection != 0 &&
                           m_served >= cfg.max_requests_per_connection);
    const bool ka       = keep_alive && !too_many;

    std::string wire;
    wire.reserve(256 + m_writer.body.size());
    wire.append("HTTP/1.1 ");
    wire.append(std::to_string(m_writer.code));
    wire.push_back(' ');
    wire.append(m_writer.reason.empty() ? "OK" : m_writer.reason);
    wire.append("\r\n");

    if (!cfg.server_name.empty() && !m_writer.headers.has("Server")) {
        wire.append("Server: ");
        wire.append(cfg.server_name);
        wire.append("\r\n");
    }
    if (cfg.emit_date && !m_writer.headers.has("Date")) {
        wire.append("Date: ");
        append_date_header(wire);
        wire.append("\r\n");
    }

    const bool no_content = (m_writer.code == 204 || m_writer.code == 304 ||
                             (m_writer.code >= 100 && m_writer.code < 200));
    if (!no_content && !m_writer.headers.has("Content-Length")) {
        wire.append("Content-Length: ");
        wire.append(std::to_string(m_writer.body.size()));
        wire.append("\r\n");
    }

    wire.append("Connection: ");
    wire.append(ka ? "keep-alive" : "close");
    wire.append("\r\n");

    for (std::size_t i = 0; i < m_writer.headers.size(); ++i) {
        wire.append(m_writer.headers.name_at(i));
        wire.append(": ");
        wire.append(m_writer.headers.value_at(i));
        wire.append("\r\n");
    }
    wire.append("\r\n");

    if (!no_content && !m_writer.no_body) {
        wire.append(m_writer.body);
    }

    if (!send(std::move(wire), !ka)) {
        close_now();   // 响应发不出去（对端已断 / 反压）：关掉，别让客户端干等
    }

    // 为下一条请求做准备（keep-alive）。升级成 WebSocket 之后不适用。
    if (ka && !m_is_ws) {
        m_parser.reset();
        m_writer.reset_for_request(false);
        m_stream_headers_done = false;
    }
}

bool ServerSession::switch_to_websocket(WebSocketHandlers handlers, WebSocketConfig wcfg,
                                        std::string path, std::string real_ip,
                                        std::string headers_raw)
{
    if (m_is_ws || !conn_snapshot()) {
        return false;
    }

    std::weak_ptr<ServerSession> weak = weak_from_this();

    detail::WsIo io;
    io.send = [weak](std::string wire, bool close_after) -> bool {
        if (auto self = weak.lock()) {
            return self->send_bytes(std::move(wire), close_after);
        }
        return false;
    };
    io.close = [weak] {
        if (auto self = weak.lock()) {
            self->close_now();
        }
    };
    io.pending = [weak]() -> std::size_t {
        if (auto self = weak.lock()) {
            return self->inflight_bytes();
        }
        return 0;
    };
    io.is_closed = [weak]() -> bool {
        if (auto self = weak.lock()) {
            return self->closed_flag();
        }
        return true;   // 会话没了就等于关
    };

    m_ws = std::make_shared<detail::WsSessionImpl>(
        detail::WsSessionImpl::Role::server, std::move(io), std::move(wcfg),
        std::move(handlers), std::move(path), std::move(real_ip), detail::from_address(m_peer),
        std::move(headers_raw));
    m_is_ws = true;

    // 进 WS 模式后，HTTP 解析器不再使用；缓冲里若有残留字节（pipelining）
    // 直接丢弃 —— 升级之后协议已经变了。
    m_ws->start();
    return true;
}

bool ServerSession::send(std::string wire, bool close_after)
{
    auto c = conn_snapshot();
    if (!c || m_closed.load(std::memory_order_acquire)) {
        return false;
    }
    auto payload = std::make_shared<std::string>(std::move(wire));
    netmech::ConstBuf cb;
    cb.owner = payload;
    cb.data  = payload->data();
    cb.size  = payload->size();

    // 登记必须在发起之前：否则写可能在登记前完成，on_write 把计数减成负数。
    m_inflight.fetch_add(1, std::memory_order_acq_rel);
    m_inflight_bytes.fetch_add(cb.size, std::memory_order_acq_rel);
    if (close_after) {
        m_close_after_all.store(true, std::memory_order_release);
    }

    if (!c->write(cb)) {
        // netmech 的 false 有"反压"这种正常情况（**不断链**），
        // 所以这里不再 close_now()，把结果如实交给调用方。
        m_inflight_bytes.fetch_sub(cb.size, std::memory_order_acq_rel);
        const auto left = m_inflight.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (close_after && left == 0) {
            close_now();   // 想关却没发出去，也不能挂着
        }
        return false;
    }
    return true;
}

// =============================================================================
//  监听桥：把 netmech 的 accept 转成"建一条 ServerSession"
// =============================================================================

class AcceptBridge : public netmech::Linkable {
public:
    explicit AcceptBridge(std::shared_ptr<HttpServer::Impl> owner) : m_owner(std::move(owner)) {}

    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override
    {
        if (!m_owner->running.load(std::memory_order_relaxed)) {
            return nullptr;   // 停止中：让库直接关掉这条连接
        }
        auto s = std::make_shared<ServerSession>(m_owner, c.remote_address());
        m_owner->add_session(s);
        return s;
    }

private:
    std::shared_ptr<HttpServer::Impl> m_owner;
};

} // namespace detail

// =============================================================================
//  HttpServer::Impl
// =============================================================================

void HttpServer::Impl::sweep_loop()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lk(sleep_mu);
            sleep_cv.wait_for(lk, std::chrono::seconds(1), [this] { return stop_sweeper; });
            if (stop_sweeper) {
                return;
            }
        }

        std::vector<std::shared_ptr<detail::ServerSession>> alive;
        {
            std::lock_guard<std::mutex> lk(sessions_mu);
            auto it = sessions.begin();
            while (it != sessions.end()) {
                if (auto s = it->lock()) {
                    alive.push_back(s);
                    ++it;
                } else {
                    it = sessions.erase(it);
                }
            }
        }
        for (auto& s : alive) {
            s->check_idle(cfg.idle_timeout);
        }
    }
}

void HttpServer::Impl::stop_sweeper_thread()
{
    {
        std::lock_guard<std::mutex> lk(sleep_mu);
        if (stop_sweeper) {
            return;   // 已经停过（幂等）
        }
        stop_sweeper = true;
    }
    sleep_cv.notify_all();
    if (sweeper.joinable()) {
        sweeper.join();
    }
}

// =============================================================================
//  HttpServer
// =============================================================================

HttpServer::HttpServer() : m_impl(std::make_shared<Impl>()) {}

HttpServer::~HttpServer()
{
    stop();
    m_impl->stop_sweeper_thread();
}

std::shared_ptr<HttpServer> HttpServer::create(ServerConfig                    cfg,
                                              std::shared_ptr<RequestHandler> handler)
{
    if (!handler) {
        return nullptr;
    }
    // 自己建 io 运行时：调用方不需要认识 netmech。
    // netmech 的线程池是**进程级懒建**的，所以这里 create() 不会白起线程；
    // 真正用到网络时才建（线程数由 netmech::initialize() 决定，默认 hw 核数）。
    return create_with_mechanism(netmech::Mechanism::create(), std::move(cfg),
                                 std::move(handler));
}

std::shared_ptr<HttpServer> HttpServer::create_with_mechanism(
    std::shared_ptr<netmech::Mechanism> mech, ServerConfig cfg,
    std::shared_ptr<RequestHandler> handler)
{
    if (!mech || !handler) {
        return nullptr;
    }
    auto srv             = std::shared_ptr<HttpServer>(new HttpServer());
    srv->m_impl->mech    = std::move(mech);
    srv->m_impl->cfg     = std::move(cfg);
    srv->m_impl->handler = std::move(handler);
    return srv;
}

std::error_code HttpServer::start()
{
    if (m_impl->running.load()) {
        return make_error_code(Errc::already_started);
    }
    if (!m_impl->mech) {
        return make_error_code(Errc::not_started);
    }

    // SO_LINGER{1,0} 会在 close 时发 RST 丢掉最后那一包 —— 对 HTTP 是
    // 静默丢响应，必须拦在启动阶段（错误信息里写了怎么改）。
    if (!m_impl->cfg.allow_linger_zero && m_impl->mech->linger_zero()) {
        return make_error_code(Errc::linger_zero_enabled);
    }

    // running 必须先置 true：AcceptBridge 会用它判断"是否还在服务"
    m_impl->running.store(true);

    auto bridge     = std::make_shared<detail::AcceptBridge>(m_impl);
    m_impl->bridge  = bridge;

    const auto ec = m_impl->mech->listen(detail::to_address(m_impl->cfg.local), bridge,
                                         m_impl->cfg.backlog);
    if (ec) {
        m_impl->bridge.reset();
        m_impl->running.store(false);
        return ec;
    }

    // 取回**实际**绑定地址：cfg.local.port == 0 时端口是系统分配的，
    // 靠 netmech 的 listening_address() 才拿得到。
    if (auto actual = m_impl->mech->listening_address(detail::to_address(m_impl->cfg.local))) {
        m_impl->bound = detail::from_address(*actual);
    } else {
        m_impl->bound = m_impl->cfg.local;
    }

    m_impl->sweeper = std::thread([impl = m_impl] { impl->sweep_loop(); });
    return std::error_code{};
}

void HttpServer::stop() noexcept
{
    if (!m_impl->running.exchange(false)) {
        return;
    }

    // 1) 只停**自己这个**监听器（netmech 的 unlisten；不再用 close_all）
    if (m_impl->mech) {
        const Endpoint addr = (m_impl->bound.port() != 0) ? m_impl->bound : m_impl->cfg.local;
        m_impl->mech->unlisten(detail::to_address(addr));
    }
    m_impl->bridge.reset();

    // 2) 关掉本服务建立的连接（unlisten 只停 accept，不动既有连接）
    std::vector<std::shared_ptr<detail::ServerSession>> alive;
    {
        std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
        for (auto& w : m_impl->sessions) {
            if (auto s = w.lock()) {
                alive.push_back(s);
            }
        }
        m_impl->sessions.clear();
    }
    for (auto& s : alive) {
        s->close_now();
    }

    m_impl->stop_sweeper_thread();
}

Endpoint HttpServer::local() const
{
    return (m_impl->bound.port() != 0) ? m_impl->bound : m_impl->cfg.local;
}

bool HttpServer::running() const noexcept
{
    return m_impl->running.load();
}

std::size_t HttpServer::connection_count() const noexcept
{
    return m_impl->conns.load();
}

std::uint64_t HttpServer::requests_total() const noexcept
{
    return m_impl->requests.load();
}

std::uint64_t HttpServer::protocol_errors_total() const noexcept
{
    return m_impl->proto_errors.load();
}

// =============================================================================
//  ResponseWriter
// =============================================================================

void ResponseWriter::set_status(Status s, std::string_view reason)
{
    m_impl->status = s;
    m_impl->code   = static_cast<std::uint16_t>(s);
    m_impl->reason.assign(reason);
}

void ResponseWriter::set_status(std::uint16_t code, std::string_view reason)
{
    m_impl->status = status_from(code);
    m_impl->code   = code;
    m_impl->reason.assign(reason);
}

void ResponseWriter::set_header(std::string_view name, std::string_view value)
{
    m_impl->headers.set(name, value);
}

void ResponseWriter::add_header(std::string_view name, std::string_view value)
{
    m_impl->headers.add(name, value);
}

bool ResponseWriter::write(span<const std::uint8_t> data)
{
    if (m_impl->sent || m_impl->aborted) {
        return false;
    }
    m_impl->body.append(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

bool ResponseWriter::write(std::string_view text)
{
    return write(span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                          text.size()});
}

void ResponseWriter::end()
{
    if (m_impl->sent || m_impl->aborted) {
        return;
    }
    m_impl->ended = true;
}

void ResponseWriter::end_status(Status s, std::string_view text)
{
    set_status(s);
    if (!text.empty()) {
        set_content_type("text/plain; charset=utf-8");
        write(text);
    }
    end();
}

bool ResponseWriter::headers_sent() const noexcept
{
    return m_impl->sent;
}

bool ResponseWriter::upgraded() const noexcept
{
    return m_impl->upgraded;
}

std::uint16_t ResponseWriter::status_code() const noexcept
{
    return m_impl->code;
}

std::size_t ResponseWriter::body_bytes() const noexcept
{
    return m_impl->body.size();
}

void ResponseWriter::abort() noexcept
{
    m_impl->aborted = true;
    if (m_impl->session) {
        m_impl->session->close_now();
    }
}

bool ResponseWriter::upgrade_websocket(WebSocketHandlers handlers, const WebSocketConfig* cfg)
{
    if (m_impl->sent || m_impl->aborted || m_impl->upgraded) {
        return false;
    }
    if (m_impl->session == nullptr || m_impl->request == nullptr) {
        return false;
    }

    const HttpRequest& req = *m_impl->request;
    if (!req.is_websocket_upgrade()) {
        return false;
    }
    // 只支持 13（RFC 6455）。老草案版本（8 / 7）不认。
    if (!iequals(trim_ows(req.header("Sec-WebSocket-Version")), "13")) {
        return false;
    }
    const auto key = trim_ows(req.header("Sec-WebSocket-Key"));
    if (key.empty()) {
        return false;
    }

    const std::string accept = make_websocket_accept(key);

    // 子协议：按约定原样回显第一个
    std::string sub;
    {
        const auto proto = req.header("Sec-WebSocket-Protocol");
        const auto comma = proto.find(',');
        sub = std::string{trim_ows(comma == std::string_view::npos ? proto : proto.substr(0, comma))};
    }

    std::string resp;
    resp.reserve(256);
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n";
    if (!sub.empty()) {
        resp += "Sec-WebSocket-Protocol: ";
        resp += sub;
        resp += "\r\n";
    }
    resp += "\r\n";

    // 握手请求头原文，供 WebSocketSession::handshake_header()
    std::string raw;
    for (std::size_t i = 0; i < req.headers.size(); ++i) {
        const auto f = req.headers.all()[i];
        raw.append(f.name.data(), f.name.size());
        raw += ": ";
        raw.append(f.value.data(), f.value.size());
        raw += "\r\n";
    }

    m_impl->sent     = true;   // 后面 finish_writer 不会再补一个响应
    m_impl->upgraded = true;

    m_impl->session->send(std::move(resp), false);

    const WebSocketConfig& wcfg =
        (cfg != nullptr) ? *cfg : m_impl->session->cfg().websocket;

    return m_impl->session->switch_to_websocket(
        std::move(handlers), wcfg, std::string{req.path},
        std::string{req.real_ip()}, std::move(raw));
}

} // namespace httpmech
