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
//  响应写出：
//    · 默认整块缓冲，end() 时按 Content-Length 一次发出（JSON 接口足够）；
//    · 累计写入超过 ServerConfig::max_buffered_response 时自动切
//      Transfer-Encoding: chunked（ResponseWriter::write →
//      ServerSession::start_stream_response / send_stream_chunk /
//      finish_stream_response）：之后每次 write 直接发一个 chunk，end() 发 0 块。
//    · HEAD / 1xx / 204 / 304 不切流式（它们本来就没有 body）。
// =============================================================================
#include <httpmech/server.hpp>

#include "net_bridge.hpp"
#include "ws_internal.hpp"

#include <netmech/conn.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <string_view>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace {

/// steady_clock 的纳秒计数（跨线程比较的统一刻度）。
inline std::int64_t now_ns_v()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t to_ns_v(std::chrono::milliseconds d)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

} // namespace

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
    bool                   chunked  = false;   ///< 已切 chunked 流式
    bool                   stream_ended = false;

    // --- 异步响应体（阶段 2）---
    BodyProviderSpec provider_spec;
    bool          provider_active      = false;  ///< 头已发、正在逐块投递
    bool          provider_done_called = false;  ///< chunked 模式 provider 调了 done()
    bool          provider_finished    = false;
    bool          provider_chunked     = false;  ///< length == 0
    std::uint64_t provider_length      = 0;
    std::uint64_t provider_written     = 0;
    std::size_t   body_bytes_total     = 0;      ///< 累计投递字节数（供 after）
    std::function<void(const ResponseSummary&)> completion_hook;
    bool          completion_fired     = false;

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
        chunked  = false;
        stream_ended = false;

        provider_spec         = BodyProviderSpec{};
        provider_active       = false;
        provider_done_called  = false;
        provider_finished     = false;
        provider_chunked      = false;
        provider_length       = 0;
        provider_written      = 0;
        body_bytes_total      = 0;
        completion_hook       = nullptr;
        completion_fired      = false;
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
        m_last_activity_ns.store(now_ns_v(), std::memory_order_relaxed);
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
        // 限制单连接的发送队列字节数：异步响应体靠它做背压，
        // 慢客户端不会把整文件堆进内存（0 = 不限制，netmech 语义）。
        if (m_conn) {
            m_conn->set_send_queue_limit(m_owner->cfg.max_send_queue_bytes, 0);
        }
        return start_read();
    }

    void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override
    {
        if (!m_conn || m_closed.load(std::memory_order_relaxed)) {
            return;
        }
        if (ec) {
            // 对端半关（FIN）但本端还有在途响应写：让它写完再关，
            // 别掐断 keep-alive 响应（netmech close 中止在途写而非冲刷）。
            // 真错误也会在 on_write 的 ec 里归还，届时再关（m2）。
            if (m_inflight.load(std::memory_order_acquire) > 0) {
                m_close_after_all.store(true, std::memory_order_release);
            } else {
                close_now();
            }
            return;
        }
        const std::int64_t now_ns = now_ns_v();
        m_last_activity_ns.store(now_ns, std::memory_order_relaxed);
        m_busy.store(true, std::memory_order_release);

        // 本请求的第一个字节：记下起点（头部超时从这一刻算起）。
        if (!m_req_headers_done.load(std::memory_order_acquire)) {
            std::int64_t expected = 0;
            (void)m_request_start_ns.compare_exchange_strong(
                expected, now_ns, std::memory_order_relaxed);
        }

        const auto* p = static_cast<const std::uint8_t*>(buf.data);
        if (m_is_ws.load(std::memory_order_acquire)) {
            if (m_ws) {
                m_ws->on_bytes(span<const std::uint8_t>{p, n});
            }
        } else {
            m_parser.feed(span<const std::uint8_t>{p, n}, m_ec);
            pump();
        }

        m_busy.store(false, std::memory_order_release);
        if (m_closed.load(std::memory_order_relaxed)) {
            return;
        }
        if (m_provider_active_flag.load(std::memory_order_acquire)) {
            // 异步响应体还没发完：暂停读，否则 pipelining 的下一条请求会
            // 在 provider 状态机还没收尾时进来。由 finish_provider() 恢复。
            m_read_suspended = true;
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
        // 异步响应体：一块落地后，post 回 io 线程取下一块。
        // 用 post 而不是直接调用，避免在 netmech 的完成回调里递归驱动。
        if (left == 0 && m_provider_active_flag.load(std::memory_order_acquire)) {
            schedule_provider_pump();
        }
        // "写完就关"：必须等**所有**已受理的写都落地。
        // 早先这是个 bool 标志 + 队列，谁先完成谁就关 —— close 帧排在别人后面时会被吞掉。
        if (left == 0 && m_close_after_all.load(std::memory_order_acquire)) {
            close_now();
        }
    }

    void on_close() override
    {
        if (m_writer.provider_active) {
            // 连接被提前关掉：仍要让 after 钩子收到一次「未完成」的摘要，
            // 否则访问日志会永远缺这一条。
            fire_completion(false);
            m_writer.provider_active = false;
            m_provider_active_flag.store(false, std::memory_order_release);
        }
        if (m_ws) {
            m_ws->on_transport_closed();
        }
        {
            std::lock_guard<std::mutex> lk(m_conn_mu);
            m_conn.reset();
        }
        // ★ 计数用独立的 m_counted 收口，不能挂 m_closed 上：
        //   close_now()（协议错误 / 超时 / abort / 发送失败）会先置 m_closed，
        //   之后 netmech 回调这里时 `!m_closed.exchange(true)` 不成立 ——
        //   connection_count() 只增不减。以前只有「对端先断」能减。
        if (!m_counted.exchange(true, std::memory_order_acq_rel)) {
            m_owner->conns.fetch_sub(1, std::memory_order_relaxed);
        }
        m_closed.store(true, std::memory_order_release);
    }

    // --- 空闲检查（HttpServer 的清理线程调用）---

    void check_idle(std::chrono::milliseconds idle)
    {
        if (m_closed.load(std::memory_order_relaxed) ||
            m_busy.load(std::memory_order_acquire)) {
            return;
        }
        // 异步响应体正在发送时，连接既不空闲也不该被 idle 超时掐断。
        if (m_provider_active_flag.load(std::memory_order_acquire)) {
            return;
        }
        if (m_is_ws.load(std::memory_order_acquire)) {
            if (m_ws) {
                m_ws->tick(std::chrono::steady_clock::now());
            }
            return;   // WS 的空闲超时由 WebSocketConfig 管
        }

        // ★ 三个阶段分开判。以前只有一条"距上次任意数据多久 > idle_timeout"，
        //   于是 `ServerConfig::header_timeout` / `body_idle_timeout` 两个配置
        //   **一次都没被读过**（只出现在头文件里）—— 防 slowloris 只是文档。
        const std::int64_t now    = now_ns_v();
        const std::int64_t last   = m_last_activity_ns.load(std::memory_order_relaxed);
        const std::int64_t start  = m_request_start_ns.load(std::memory_order_relaxed);
        const bool         hdone  = m_req_headers_done.load(std::memory_order_acquire);

        // ① 头部阶段（请求刚开始、头还没收完）：**从第一个字节算起**。
        //    slowloris 的招式是"每隔一会儿发一个字节"，永远有"新数据"，
        //    所以绝不能拿"距上次数据"当判据。
        if (start != 0 && !hdone) {
            const auto ht = m_owner->cfg.header_timeout;
            if (ht.count() > 0 && now - start > to_ns_v(ht)) {
                close_now();
                return;
            }
            return;   // 头还没收完：body 判据不适用
        }

        // ② body 阶段：两次收到数据之间的最大间隔
        if (start != 0 && hdone) {
            const auto bt = m_owner->cfg.body_idle_timeout;
            if (bt.count() > 0 && now - last > to_ns_v(bt)) {
                close_now();
                return;
            }
            return;
        }

        // ③ 没有请求在途：全局空闲超时
        if (last != 0 && now - last > to_ns_v(idle)) {
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
    friend class httpmech::ResponseWriter;   // 流式响应直接调 start/send_stream_chunk
    friend class httpmech::DataSink;         // DataSink::write/writable/done 转到这里

    static constexpr std::size_t kReadChunk      = 16 * 1024;
    static constexpr std::size_t kProviderChunk  = 256 * 1024;   ///< 与 cpp-httplib 同量级

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
    void reset_for_next_request(bool keep_alive);
    /// 超过 ServerConfig::max_buffered_response：先把响应头按 chunked 发出去。
    bool start_stream_response();
    bool send_stream_chunk(span<const std::uint8_t> data);
    /// 发 0 长度块收尾（end() 调用）。
    void finish_stream_response();

    // --- 异步响应体（阶段 2）---
    bool provider_write(span<const std::uint8_t> data);
    bool provider_writable() const noexcept;
    void provider_done() noexcept;
    void pump_provider();
    void schedule_provider_pump();
    void finish_provider(bool ok);
    void fire_completion(bool ok);

    std::shared_ptr<HttpServer::Impl> m_owner;
    std::shared_ptr<netmech::Conn>    m_conn;
    netmech::Address                  m_peer;

    httpmech::Parser            m_parser;
    std::error_code             m_ec;
    ResponseWriter::Impl        m_writer;

    /// 升级成 WebSocket 之后，字节不再进 HTTP 解析器。
    /// 原子：io 线程置位、sweeper 线程读（check_idle 的分支）。
    std::atomic<bool>                           m_is_ws{false};
    /// on_close 里「已经减过 conns 计数」的闸（与 m_closed 分开，见 on_close）。
    std::atomic<bool>                           m_counted{false};
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

    /// sweeper 线程只读原子量，不直接碰 m_writer（避免数据竞争）。
    std::atomic<bool> m_provider_active_flag{false};
    std::atomic<bool> m_provider_pump_scheduled{false};
    /// io 线程专用：on_read 因异步发送暂停过，由 finish_provider 恢复。
    bool              m_read_suspended = false;

    /// 应用回调正在执行（io 线程置位、sweeper 读 —— 必须原子）。
    /// 非原子时 sweeper 会读到陈旧的 false，把**正在长处理**的连接判成空闲
    /// 并 close_now()，业务请求在回调执行中被掐断。
    std::atomic<bool>         m_busy{false};
    std::uint64_t             m_served = 0;

    /// 上次收到任意数据的时刻（纳秒）。跨线程读写，所以用原子整数而不是
    /// time_point（后者没有原子版本，32 位平台上 64 位读还会被撕裂）。
    std::atomic<std::int64_t> m_last_activity_ns{0};

    /// 本请求收到**第一个字节**的时刻（0 = 当前没有在收的请求）。
    /// 头部超时的基准：slowloris 的特征是"一直在慢慢发"，用"距上次数据多久"
    /// 抓不到，必须用"从请求开始到现在多久"。
    std::atomic<std::int64_t> m_request_start_ns{0};

    /// 本请求的头是否已经收完（pump 收到 headers / message 时置位）。
    std::atomic<bool>         m_req_headers_done{false};
};

// =============================================================================
//  小工具
// =============================================================================

namespace {

/// IMF-fixdate：`Sun, 06 Nov 1994 08:49:37 GMT`
/// 逗号列表里是否含某 token（大小写不敏感），用于应用自定义的 Connection 头。
bool token_list_contains(std::string_view list, std::string_view token) noexcept
{
    std::size_t i = 0;
    while (i <= list.size()) {
        const std::size_t comma = list.find(',', i);
        const std::size_t end   = (comma == std::string_view::npos) ? list.size() : comma;
        if (httpmech::iequals(trim_ows(list.substr(i, end - i)), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        i = comma + 1;
    }
    return false;
}

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
/// 解析点分四段 IPv4。失败返回 false（不做任何宽松猜测）。
bool parse_ipv4(std::string_view s, std::uint32_t& out) noexcept
{
    std::uint32_t v = 0;
    int           parts = 0;
    std::size_t   i = 0;
    while (i < s.size()) {
        unsigned octet = 0;
        std::size_t digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            octet = octet * 10 + static_cast<unsigned>(s[i] - '0');
            if (octet > 255) {
                return false;
            }
            ++i;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
        v = (v << 8) | octet;
        ++parts;
        if (i < s.size()) {
            if (s[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    if (parts != 4) {
        return false;
    }
    out = v;
    return true;
}

bool is_trusted_proxy(const netmech::Address& peer, const std::vector<std::string>& list)
{
    if (list.empty()) {
        return false;
    }
    const std::string ip = peer.address_string();
    if (ip.find(':') != std::string::npos) {
        return false;   // IPv6 暂不支持前缀匹配
    }

    std::uint32_t peer_ip = 0;
    if (!parse_ipv4(ip, peer_ip)) {
        return false;
    }

    for (const auto& raw : list) {
        if (raw.empty()) {
            continue;
        }
        const std::string_view entry(raw);
        const std::size_t slash = entry.find('/');
        if (slash == std::string_view::npos) {
            if (raw == ip) {
                return true;
            }
            continue;
        }

        // ★ 按**位**比较，不是按整八位组截断。
        //
        //   以前只比前 `plen/8` 个八位组，余数位直接扔掉：`10.0.0.0/25`
        //   行为等同 `/24`、`192.168.1.0/30` 等同 `/24` —— **超指配的前缀会被
        //   验证为合法**，X-Real-IP 这类身份头被采信的范围比用户意图宽。
        //   安全相关的判断不能在"差不多"上让步。
        unsigned plen = 0;
        bool     ok   = true;
        for (char ch : entry.substr(slash + 1)) {
            if (ch < '0' || ch > '9') {
                ok = false;
                break;
            }
            plen = plen * 10 + static_cast<unsigned>(ch - '0');
            if (plen > 32) {
                ok = false;
                break;
            }
        }
        if (!ok || slash + 1 >= entry.size()) {
            continue;   // 前缀长度非法（空 / 非数字 / >32）
        }

        std::uint32_t base_ip = 0;
        if (!parse_ipv4(entry.substr(0, slash), base_ip)) {
            continue;   // 基址不是合法 IPv4（含 "010.0.0.0" 这种）
        }

        const std::uint32_t mask =
            (plen == 0) ? 0u : (0xFFFFFFFFu << (32 - plen));
        if ((base_ip & mask) == (peer_ip & mask)) {
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
            // 聚合模式：头部已收齐但 body 还在途时，也要切到 body 阶段
            // 判据（body_idle_timeout），否则慢上传会被 header_timeout
            // 从首字节计时误杀（M7）。
            if (m_parser.headers_complete()) {
                m_req_headers_done.store(true, std::memory_order_release);
            }
            return;   // 等下一批数据

        case httpmech::Parser::Result::message:
            // 头和体都到齐了（聚合模式下一次拿到整条消息）
            m_req_headers_done.store(true, std::memory_order_release);
            dispatch_aggregate();
            if (m_is_ws.load(std::memory_order_acquire)) {
                // 已经升级成 WebSocket：HTTP 解析器退出舞台，
                // 后面的字节全部交给帧层（on_read 里分流）。
                return;
            }
            if (m_closed.load(std::memory_order_relaxed) ||
                m_close_after_all.load(std::memory_order_acquire) ||
                m_provider_active_flag.load(std::memory_order_acquire)) {
                // 异步响应体还没发完：此时解析器尚未 reset，继续 next() 会
                // 把同一条消息反复派发。等 finish_provider() 复位后再读。
                return;
            }
            break;

        case httpmech::Parser::Result::headers:
            // 流式模式：后面的字节都是 body，切到 body_idle_timeout 判据
            m_req_headers_done.store(true, std::memory_order_release);
            dispatch_stream_events({}, false);
            break;

        case httpmech::Parser::Result::body_chunk:
            dispatch_stream_events(m_parser.body_chunk(), false);
            break;

        case httpmech::Parser::Result::message_end:
            dispatch_stream_events({}, true);
            if (m_closed.load(std::memory_order_relaxed) ||
                m_close_after_all.load(std::memory_order_acquire) ||
                m_provider_active_flag.load(std::memory_order_acquire)) {
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
            m_writer.ended  = true;   // 库主动完成的完整响应，勿走 500 兜底
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
    if (!m_provider_active_flag.load(std::memory_order_acquire)) {
        m_stream_headers_done = false;
    }
}

void ServerSession::respond_protocol_error(Status code)
{
    // 已经发过响应（流式模式下头先发出去了）就没有"再回一个 400"的机会了：
    // 只能直接断开，否则客户端会等一个永远不会结束的 body。
    if (m_writer.sent || m_writer.aborted) {
        close_now();
        return;
    }
    m_writer.reset_for_request(false);
    m_writer.code   = static_cast<std::uint16_t>(code);
    m_writer.reason = reason_phrase(code);
    m_writer.headers.set("Content-Length", "0");
    // ★ 协议错误是库主动完成的完整响应，标记 ended 以区别于"应用忘调
    //   end()"——否则 finish_writer 的 !ended→500 兜底会把映射好的
    //   400/431/413/501 全覆盖成 500（M1）。
    m_writer.ended  = true;
    finish_writer(false);
}

bool ServerSession::start_stream_response()
{
    if (m_writer.sent || m_writer.aborted) {
        return false;
    }
    const ServerConfig& cfg = m_owner->cfg;

    if (!m_writer.reason.empty()) {
        // 应用自定义原因短语
    } else {
        m_writer.reason = reason_phrase(status_from(m_writer.code));
    }

    const bool too_many = (cfg.max_requests_per_connection != 0 &&
                           m_served >= cfg.max_requests_per_connection);
    bool       ka       = (m_writer.request != nullptr) && m_writer.request->keep_alive && !too_many;
    const auto app_connection = m_writer.headers.get("Connection");
    if (!app_connection.empty() && token_list_contains(app_connection, "close")) {
        ka = false;
    }

    std::string wire;
    wire.reserve(256);
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
    // chunked 与 Content-Length 互斥
    wire.append("Transfer-Encoding: chunked\r\n");
    if (app_connection.empty()) {
        wire.append("Connection: ");
        wire.append(ka ? "keep-alive" : "close");
        wire.append("\r\n");
    }
    for (std::size_t i = 0; i < m_writer.headers.size(); ++i) {
        wire.append(m_writer.headers.name_at(i));
        wire.append(": ");
        wire.append(m_writer.headers.value_at(i));
        wire.append("\r\n");
    }
    wire.append("\r\n");

    m_writer.sent    = true;
    m_writer.chunked = true;
    if (!send(std::move(wire), false)) {
        m_writer.aborted = true;
        close_now();
        return false;
    }
    // 之前攒下的 body 作为第一个块
    if (!m_writer.body.empty()) {
        const std::string first = std::move(m_writer.body);
        m_writer.body.clear();
        if (!send_stream_chunk(span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(first.data()), first.size()})) {
            return false;
        }
    }
    return true;
}

bool ServerSession::send_stream_chunk(span<const std::uint8_t> data)
{
    if (data.empty()) {
        return true;
    }
    std::string chunk;
    chunk.reserve(data.size() + 16);
    // 十六进制长度
    char        hex[17];
    std::size_t n = 0;
    std::size_t v = data.size();
    do {
        const unsigned d = static_cast<unsigned>(v & 0xF);
        hex[n++] = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
        v >>= 4;
    } while (v != 0);
    for (std::size_t i = 0; i < n / 2; ++i) {
        std::swap(hex[i], hex[n - 1 - i]);
    }
    chunk.append(hex, n);
    chunk.append("\r\n");
    chunk.append(reinterpret_cast<const char*>(data.data()), data.size());
    chunk.append("\r\n");
    if (!send(std::move(chunk), false)) {
        m_writer.aborted = true;
        close_now();
        return false;
    }
    return true;
}

void ServerSession::finish_stream_response()
{
    if (m_writer.stream_ended || m_writer.aborted) {
        return;
    }
    m_writer.stream_ended = true;
    const ServerConfig& cfg = m_owner->cfg;
    const bool too_many = (cfg.max_requests_per_connection != 0 &&
                           m_served >= cfg.max_requests_per_connection);
    bool       ka       = (m_writer.request != nullptr) && m_writer.request->keep_alive && !too_many;
    const auto app_connection = m_writer.headers.get("Connection");
    if (!app_connection.empty() && token_list_contains(app_connection, "close")) {
        ka = false;
    }
    if (!send(std::string("0\r\n\r\n"), !ka)) {
        // 终止块发不出去（反压/对端已断）：与 start_stream_response /
        // send_stream_chunk 对称，abort + 关连接。否则 keep-alive 复用
        // 时下一条响应会接在未终止的 chunked 流后→协议损坏（m1）。
        m_writer.aborted = true;
        close_now();
    }
}

/// keep-alive 收尾：解析器与 writer 复位（HTTP/1.0 或 close 时只清状态）。
void ServerSession::reset_for_next_request(bool keep_alive)
{
    if (keep_alive && !m_is_ws.load(std::memory_order_acquire)) {
        m_parser.reset();
        m_writer.reset_for_request(false);
        m_stream_headers_done = false;
        m_req_headers_done.store(false, std::memory_order_release);
        m_request_start_ns.store(0, std::memory_order_relaxed);
    }
}

void ServerSession::finish_writer(bool keep_alive)
{
    if (m_writer.stream_ended) {
        // end() 里已经发过 0 块；这里只负责把连接复位（或让它按 close 收尾）。
        const ServerConfig& cfg = m_owner->cfg;
        const bool too_many = (cfg.max_requests_per_connection != 0 &&
                               m_served >= cfg.max_requests_per_connection);
        fire_completion(true);
        reset_for_next_request(keep_alive && !too_many);
        return;
    }
    if (m_writer.sent || m_writer.aborted) {
        // upgrade_websocket() 自己把 101 发出去了，这里走不到发送逻辑，
        // 但 after 钩子仍要收到一次摘要（否则 WS 请求永远没有访问日志）。
        if (m_writer.upgraded) { fire_completion(true); }
        return;
    }

    // ★ 应用没调 end()：说明它中途返回了（或忘了）。
    //
    //   头文件承诺"库会补一个 500，记 `Errc::response_incomplete`"，
    //   但以前这里**不检查** —— 直接把当前状态按 200 + 半截 body 发出去，
    //   客户端看到的是"请求成功"。忘调 end() 是应用最常见的低级错误之一，
    //   把它伪装成成功比直接报错难查得多。
    if (!m_writer.ended) {
        m_writer.code = 500;
        m_writer.reason.clear();
        m_writer.body.clear();
        m_writer.headers.clear();
        m_writer.headers.set("Content-Length", "0");
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
    bool       ka       = keep_alive && !too_many;

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
    const bool has_provider = static_cast<bool>(m_writer.provider_spec.provider);
    if (has_provider && m_writer.provider_spec.length == 0) {
        // 未知长度：chunked，由 provider 调 done() 结束。
        wire.append("Transfer-Encoding: chunked\r\n");
    } else if (!no_content && !m_writer.headers.has("Content-Length")) {
        wire.append("Content-Length: ");
        wire.append(std::to_string(has_provider ? m_writer.provider_spec.length
                                                : m_writer.body.size()));
        wire.append("\r\n");
    }

    // 应用自己设置了 Connection（例如提前要求 close）时不要再追加一个同名头；
    // 值为 close 时还要按 close 收尾（以前只看请求的 keep_alive，应用的
    // Connection: close 被完全忽略）。
    const auto app_connection = m_writer.headers.get("Connection");
    if (!app_connection.empty()) {
        if (token_list_contains(app_connection, "close")) {
            ka = false;
        }
    } else {
        wire.append("Connection: ");
        wire.append(ka ? "keep-alive" : "close");
        wire.append("\r\n");
    }

    for (std::size_t i = 0; i < m_writer.headers.size(); ++i) {
        wire.append(m_writer.headers.name_at(i));
        wire.append(": ");
        wire.append(m_writer.headers.value_at(i));
        wire.append("\r\n");
    }
    wire.append("\r\n");

    if (!no_content && !m_writer.no_body && !has_provider) {
        wire.append(m_writer.body);
    }

    // HEAD：Content-Length 已经按 provider 的总长度写好，这里只发头，
    // 不启动状态机（否则会把整个文件读出来又丢弃）。
    if (has_provider && !m_writer.no_body) {
        m_writer.provider_active   = true;
        m_writer.provider_length  = m_writer.provider_spec.length;
        m_writer.provider_chunked = (m_writer.provider_spec.length == 0);
        m_writer.provider_written = 0;
        m_provider_active_flag.store(true, std::memory_order_release);

        // provider 期间所有写都不带 close_after：关闭由 finish_provider 统一决定，
        // 否则 on_write 的 close_after_all 分支会在最后一块落地时先掐断连接。
        if (!send(std::move(wire), false)) {
            fire_completion(false);
            m_writer.provider_active = false;
            m_provider_active_flag.store(false, std::memory_order_release);
            close_now();
            return;
        }
        pump_provider();   // 头还在途时由 on_write 驱动；否则立即投第一块
        return;            // 不 reset，等异步完成
    }

    if (!send(std::move(wire), !ka)) {
        close_now();   // 响应发不出去（对端已断 / 反压）：关掉，别让客户端干等
    } else {
        fire_completion(true);
    }

    // 为下一条请求做准备（keep-alive）。升级成 WebSocket 之后不适用。
    reset_for_next_request(ka);
}

// =============================================================================
//  异步响应体状态机（阶段 2）
//
//  驱动方式：finish_writer() 发完响应头 → pump_provider() 取第一块 →
//  Conn::write 完成回调 on_write() → Conn::post() 回 io 线程 → pump_provider()。
//  每一轮最多只有 1 块在途，内存占用与文件大小无关，这就是背压。
// =============================================================================

bool ServerSession::provider_writable() const noexcept
{
    const auto limit = m_owner->cfg.max_send_queue_bytes;
    return limit == 0 ||
           m_inflight_bytes.load(std::memory_order_relaxed) < limit;
}

void ServerSession::provider_done() noexcept
{
    if (m_writer.provider_active) {
        m_writer.provider_done_called = true;
    }
}

bool ServerSession::provider_write(span<const std::uint8_t> data)
{
    if (!m_writer.provider_active || m_writer.provider_finished ||
        m_closed.load(std::memory_order_relaxed)) {
        return false;
    }
    if (data.empty()) { return true; }

    // 超出声明的 Content-Length 会把多出来的字节变成客户端眼里的下一条响应。
    if (!m_writer.provider_chunked) {
        const auto remaining = m_writer.provider_length - m_writer.provider_written;
        if (data.size() > remaining) { return false; }
    }

    std::string wire;
    if (m_writer.provider_chunked) {
        char hex[32]{};
        std::snprintf(hex, sizeof(hex), "%zx\r\n", data.size());
        wire.append(hex);
        wire.append(reinterpret_cast<const char*>(data.data()), data.size());
        wire.append("\r\n");
    } else {
        wire.assign(reinterpret_cast<const char*>(data.data()), data.size());
    }

    // 已设置发送队列水位、且每连接只保持一块在途，所以返回 false 基本只可能是
    // 连接已关；不重试，交给状态机收尾，避免把块悄悄丢掉。
    if (!send(std::move(wire), false)) { return false; }

    m_writer.provider_written += data.size();
    m_writer.body_bytes_total += data.size();
    return true;
}

void ServerSession::schedule_provider_pump()
{
    if (m_provider_pump_scheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::shared_ptr<ServerSession> self = shared_from_this();
    auto c = conn_snapshot();
    if (!c) {
        m_provider_pump_scheduled.store(false, std::memory_order_release);
        return;
    }
    c->post([self]() mutable {
        self->m_provider_pump_scheduled.store(false, std::memory_order_release);
        self->pump_provider();
    }, false);
}

void ServerSession::pump_provider()
{
    if (!m_writer.provider_active || m_writer.provider_finished) { return; }
    if (m_closed.load(std::memory_order_relaxed)) { finish_provider(false); return; }

    // 背压：前一块还没落地，或队列水位没降下来，就停在这里等 on_write。
    if (m_inflight.load(std::memory_order_acquire) != 0) { return; }
    if (!provider_writable()) { return; }

    if (!m_writer.provider_chunked &&
        m_writer.provider_written >= m_writer.provider_length) {
        finish_provider(true);
        return;
    }
    if (m_writer.provider_chunked && m_writer.provider_done_called) {
        // 终止块必须发出去，否则 keep-alive 复用时下一条响应会接在
        // 未终止的 chunked 流后面（a00718d 的 m1 修复）。
        if (!send(std::string{"0\r\n\r\n"}, false)) {
            finish_provider(false);
            return;
        }
        finish_provider(true);
        return;
    }

    const std::uint64_t remaining = m_writer.provider_chunked
        ? static_cast<std::uint64_t>(kProviderChunk)
        : (m_writer.provider_length - m_writer.provider_written);
    const auto want = static_cast<std::size_t>(
        (std::min)(remaining, static_cast<std::uint64_t>(kProviderChunk)));

    const std::uint64_t before = m_writer.provider_written;
    DataSink sink{this};
    bool ok = false;
    try {
        ok = m_writer.provider_spec.provider(m_writer.provider_written, want, sink);
    } catch (...) {
        // provider 是应用回调：异常不能穿过 netmech 的 io 完成回调。
        ok = false;
    }
    if (!ok) {
        finish_provider(false);
        close_now();
        return;
    }
    if (m_writer.provider_written == before && !m_writer.provider_done_called) {
        // 既没投递也没 done：下一轮还会拿到同样的 offset，会无限循环。
        finish_provider(false);
        close_now();
        return;
    }
    if (m_inflight.load(std::memory_order_acquire) == 0 &&
        !m_writer.provider_finished) {
        // 该块已同步落地：post 而不是递归，避免大文件把栈打满。
        schedule_provider_pump();
    }
}

void ServerSession::fire_completion(bool ok)
{
    if (m_writer.completion_fired) { return; }
    m_writer.completion_fired = true;
    auto hook = m_writer.completion_hook;   // 拷一份：回调里可能改动 writer
    if (!hook) { return; }

    ResponseSummary sum;
    sum.status     = m_writer.code;
    sum.body_bytes = m_writer.body_bytes_total;
    sum.upgraded   = m_writer.upgraded;
    sum.keep_alive = ok && m_writer.request && m_writer.request->keep_alive;
    const auto start = m_request_start_ns.load(std::memory_order_relaxed);
    if (start != 0) {
        const auto ns = now_ns_v() - start;
        sum.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds{ns < 0 ? 0 : ns});
    }
    try { hook(sum); } catch (...) {}
}

void ServerSession::finish_provider(bool ok)
{
    if (m_writer.provider_finished) { return; }
    m_writer.provider_finished = true;
    m_writer.provider_active   = false;
    m_provider_active_flag.store(false, std::memory_order_release);

    if (m_writer.provider_spec.on_complete) {
        auto cb = m_writer.provider_spec.on_complete;
        try { cb(ok); } catch (...) {}
    }
    fire_completion(ok);

    if (!ok) { close_now(); return; }

    const ServerConfig& cfg = m_owner->cfg;
    const bool too_many = (cfg.max_requests_per_connection != 0 &&
                           m_served >= cfg.max_requests_per_connection);
    const bool keep = m_writer.request && m_writer.request->keep_alive &&
                      !too_many &&
                      !m_close_after_all.load(std::memory_order_acquire);
    if (!keep) {
        // 可能还有在途写（例如 chunked 的终止块）：交给 on_write 归零后再关，
        // 直接 close_now() 会掐断最后一包（netmech 的 close 不冲刷在途写）。
        if (m_inflight.load(std::memory_order_acquire) != 0) {
            m_close_after_all.store(true, std::memory_order_release);
        } else {
            close_now();
        }
        return;
    }

    reset_for_next_request(true);
    if (m_read_suspended) {
        m_read_suspended = false;
        if (!start_read()) { close_now(); }
    }
}

bool ServerSession::switch_to_websocket(WebSocketHandlers handlers, WebSocketConfig wcfg,
                                        std::string path, std::string real_ip,
                                        std::string headers_raw)
{
    if (m_is_ws.load(std::memory_order_acquire) || !conn_snapshot()) {
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
    m_is_ws.store(true, std::memory_order_release);

    // 进 WS 模式后，HTTP 解析器不再使用。但缓冲里的残留字节**不能丢**：
    // 客户端收到 101 之后可能立刻就把第一帧发出来了（同机 / 低 RTT 场景
    // 尤其常见），那些字节此刻正躺在解析器缓冲里。
    // 以前这里直接丢弃，表现为"首帧静默消失"——而客户端那一侧是做了交接的
    // （client.cpp 的 "101 之后可能已经跟着帧了"），服务端没做。
    const auto rest = m_parser.pending_bytes();
    m_ws->start();
    if (!rest.empty()) {
        m_ws->on_bytes(rest);
    }
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
    auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, cfg.runtime);
    return create_with_mechanism(std::move(mech), std::move(cfg),
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
    set_status(static_cast<std::uint16_t>(s), reason);
}

void ResponseWriter::set_status(std::uint16_t code, std::string_view reason)
{
    if (m_impl->sent) {
        return;   // 头已发出：修改对线路无影响，与 headers_sent() 文档对齐（m3）
    }
    m_impl->status = status_from(code);
    m_impl->code   = code;

    // ★ 原因短语里出现 CR/LF 会把响应劈成两条（response splitting）。
    //   解析侧把 TE+CL 冲突、obs-fold 都拒了，生成侧应当对称。
    //   发现非法就回落到标准短语 —— 宁可措辞不准，也不要拼出一条被截断的报文。
    if (reason.find_first_of("\r\n") != std::string_view::npos) {
        m_impl->reason.clear();
        return;
    }
    m_impl->reason.assign(reason);
}

void ResponseWriter::set_header(std::string_view name, std::string_view value)
{
    if (m_impl->sent) {
        return;   // 头已发出（m3）
    }
    // 头名必须是 token（不含 CR/LF/冒号），值里不能有 CR/LF —— 否则应用
    // 拼进来的脏数据会变成第二条响应。解析侧已经这么严了，生成侧对称处理。
    if (name.empty() || name.find_first_of("\r\n:") != std::string_view::npos) {
        return;
    }
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        return;
    }
    m_impl->headers.set(name, value);
}

void ResponseWriter::add_header(std::string_view name, std::string_view value)
{
    if (m_impl->sent) {
        return;   // 头已发出（m3）
    }
    if (name.empty() || name.find_first_of("\r\n:") != std::string_view::npos) {
        return;
    }
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        return;
    }
    m_impl->headers.add(name, value);
}

bool ResponseWriter::write(span<const std::uint8_t> data)
{
    if (m_impl->aborted || m_impl->stream_ended) {
        return false;
    }
    if (m_impl->provider_spec.provider) {
        return false;   // 已注册异步响应体，两种通道不要混用
    }
    if (!m_impl->sent) {
        // 还没发头：攒到 ServerConfig::max_buffered_response 就切 chunked 流式
        //（头文件承诺的行为；以前这里没有实现，大响应会整体驻留内存）。
        const bool no_content = (m_impl->code == 204 || m_impl->code == 304 ||
                                 (m_impl->code >= 100 && m_impl->code < 200));
        const bool body_allowed = !m_impl->no_body && !no_content;
        const auto& cfg = m_impl->session->cfg();
        if (body_allowed && cfg.max_buffered_response != 0 &&
            m_impl->body.size() + data.size() > cfg.max_buffered_response) {
            if (!m_impl->session->start_stream_response()) {
                return false;
            }
        } else {
            m_impl->body.append(reinterpret_cast<const char*>(data.data()), data.size());
            m_impl->body_bytes_total += data.size();
            return true;
        }
    }
    // 已切 chunked：直接作为一个块发出
    if (m_impl->chunked) {
        if (!m_impl->session->send_stream_chunk(data)) { return false; }
        m_impl->body_bytes_total += data.size();
        return true;
    }
    return false;
}

bool ResponseWriter::write(std::string_view text)
{
    return write(span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                          text.size()});
}

void ResponseWriter::end()
{
    if (m_impl->chunked) {
        m_impl->session->finish_stream_response();
        m_impl->ended = true;
        return;
    }
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
    // 累计值：切 chunked 之后 body 已被 move 走，返回 body.size() 会变成 0。
    return m_impl->body_bytes_total;
}

void ResponseWriter::set_body_provider(BodyProviderSpec spec)
{
    if (m_impl->sent || m_impl->aborted || !spec.provider) {
        return;   // 头已发出（或无效）就退回普通响应
    }
    m_impl->provider_spec = std::move(spec);
}

void ResponseWriter::set_completion_hook_(
    std::function<void(const ResponseSummary&)> cb)
{
    if (m_impl->sent || m_impl->aborted) { return; }
    m_impl->completion_hook = std::move(cb);
}

// --- DataSink：只做转发，不暴露 ServerSession / netmech::Conn ---

bool DataSink::write(span<const std::uint8_t> data)
{
    return m_session && m_session->provider_write(data);
}

bool DataSink::write(std::string_view text)
{
    return write(span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool DataSink::writable() const noexcept
{
    return m_session && m_session->provider_writable();
}

void DataSink::done() noexcept
{
    if (m_session) { m_session->provider_done(); }
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
