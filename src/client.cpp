// =============================================================================
//  httpmech - 客户端实现（HttpClient / WebSocketClient）
// =============================================================================
//  两者共用同一套骨架：
//      mech->connect(remote, linkable, timeout)
//        → Linkable::on_linkup 拿到 Conn
//        → 建一个 ConnHandler 会话（读写循环）
//        → 协议层（HTTP 解析器 / WS 帧层）
//
//  线程模型：
//    · 回调都在 io 线程上串行（继承 netmech 的约定）。
//    · **超时**不另开线程去调用户回调 —— 计时器只关连接，真正的收尾统一走
//      on_close（仍是 io 线程），应用不必考虑并发。
//    · 连接建立后的 ConnHandler 由 netmech 接管（on_linkup 的返回值），
//      这里不用自己保活。
// =============================================================================
#include <httpmech/client.hpp>

#include "sha1_base64.hpp"
#include "ws_internal.hpp"

#include <netmech/conn.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace httpmech {

namespace {

std::string host_of(const netmech::Address& a)
{
    std::string h = a.address_string();
    if (a.port() != 80) {
        h += ':';
        h += std::to_string(a.port());
    }
    return h;
}

/// 低开销计时器：到点只执行一个动作（通常是关连接），不碰用户回调。
class Ticker {
public:
    explicit Ticker(std::chrono::milliseconds period) : m_period(period) {}
    ~Ticker() { stop(); }

    void start(std::function<void()> on_tick)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_thread.joinable()) {
            return;
        }
        m_thread = std::thread([this, tick = std::move(on_tick)] {
            std::unique_lock<std::mutex> inner(m_mu);
            while (!m_stop) {
                if (m_cv.wait_for(inner, m_period, [this] { return m_stop; })) {
                    return;
                }
                inner.unlock();
                tick();
                inner.lock();
            }
        });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    std::chrono::milliseconds m_period;
    std::thread               m_thread;
    std::mutex                m_mu;
    std::condition_variable   m_cv;
    bool                      m_stop = false;
};

std::string make_client_key()
{
    // 16 字节随机 → base64 后 24 字符（RFC 6455 §4.1 要求的形态）
    std::random_device               rd;
    std::mt19937                     gen(rd());
    std::uniform_int_distribution<>  dis(0, 255);
    std::uint8_t                     raw[16];
    for (auto& b : raw) {
        b = static_cast<std::uint8_t>(dis(gen));
    }
    return detail::base64_encode(raw, sizeof(raw));
}

} // namespace

// =============================================================================
//  HttpClient
// =============================================================================

class HttpClientSession;

struct HttpClient::Impl : public std::enable_shared_from_this<HttpClient::Impl> {
    struct Job {
        RequestSpec                           spec;
        std::shared_ptr<ResponseHandler>      handler;
        std::chrono::steady_clock::time_point deadline;
    };

    std::shared_ptr<netmech::Mechanism> mech;
    ClientConfig                        cfg;

    std::mutex      mu;
    std::deque<Job> queue;
    Job             current;
    bool            has_current    = false;
    bool            closed         = false;
    bool            timed_out      = false;
    bool            connecting     = false;
    bool            ticker_started = false;

    std::shared_ptr<netmech::Linkable> linkable;       ///< 必须保活：netmech 只存 weak_ptr
    std::shared_ptr<HttpClientSession> conn;
    std::size_t                        served_on_conn = 0;

    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> failures{0};

    Ticker ticker{std::chrono::milliseconds(200)};

    void advance();
    void on_connected(std::shared_ptr<HttpClientSession> s);
    void do_connect();
    void complete();                      ///< 清掉在途标记并取下一笔
    void on_conn_closed();
    void fail_all(std::error_code ec);
    void ensure_ticker();
};

namespace {

class HttpLinkable : public netmech::Linkable {
public:
    std::weak_ptr<HttpClient::Impl> owner;

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override;

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        if (auto o = owner.lock()) {
            {
                std::lock_guard<std::mutex> lk(o->mu);
                o->connecting = false;
            }
            o->fail_all(ec);
        }
    }
};

} // namespace

/// 一条到服务端的 HTTP 连接。串行：同一时刻只有一笔请求在途。
class HttpClientSession : public netmech::ConnHandler,
                          public std::enable_shared_from_this<HttpClientSession> {
public:
    HttpClientSession(std::shared_ptr<HttpClient::Impl> owner)
        : m_owner(std::move(owner))
        , m_parser(Parser::Role::client, make_cfg())
    {
        m_buf = std::make_shared<std::vector<char>>(16 * 1024);
    }

    bool on_open(std::shared_ptr<netmech::Conn> c) override
    {
        m_conn = std::move(c);
        m_owner->on_connected(shared_from_this());
        return start_read();
    }

    void on_read(std::error_code ec, netmech::Buf b, std::size_t n) override
    {
        if (!m_conn) {
            return;
        }
        if (ec) {
            close_conn();
            return;
        }
        const auto* p = static_cast<const std::uint8_t*>(b.data);
        m_parser.feed(span<const std::uint8_t>{p, n}, m_ec);
        pump();
        if (!m_closed.load() && m_conn && !start_read()) {
            close_conn();
        }
    }

    void on_write(std::error_code ec, netmech::ConstBuf /*b*/, std::size_t /*n*/) override
    {
        if (!m_pending.empty()) {
            m_pending.pop_front();
        }
        if (ec) {
            close_conn();
        }
    }

    void on_close() override
    {
        m_conn.reset();
        if (!m_closed.exchange(true)) {
            m_owner->on_conn_closed();
        }
    }

    bool usable() const noexcept { return m_conn && !m_closed.load(); }

    void close_conn() noexcept
    {
        if (m_conn) {
            m_conn->close();
        }
    }

    /// 发一笔请求。handler 由本会话保存到响应结束（连接内串行，不需要锁）。
    bool begin(const RequestSpec& spec, std::shared_ptr<ResponseHandler> handler, bool keep_alive)
    {
        m_handler    = std::move(handler);
        m_keep_alive = keep_alive;
        m_parser.reset();
        m_ec.clear();

        std::string wire;
        wire.reserve(256 + spec.body.size());
        wire += to_string(spec.method);
        wire.push_back(' ');
        wire += spec.target.empty() ? "/" : spec.target;
        wire.push_back(' ');
        wire += to_string(spec.version);
        wire += "\r\nHost: ";
        wire += host_of(m_owner->cfg.remote);
        wire += "\r\n";

        if (!m_owner->cfg.user_agent.empty() && !spec.headers.has("User-Agent")) {
            wire += "User-Agent: ";
            wire += m_owner->cfg.user_agent;
            wire += "\r\n";
        }
        wire += "Content-Length: ";
        wire += std::to_string(spec.body.size());
        wire += "\r\nConnection: ";
        wire += keep_alive ? "keep-alive" : "close";
        wire += "\r\n";

        for (std::size_t i = 0; i < spec.headers.size(); ++i) {
            const auto& name = spec.headers.name_at(i);
            // 这四个由库管，别让应用写出自相矛盾的报文
            if (iequals(name, "Host") || iequals(name, "Content-Length") ||
                iequals(name, "Connection") || iequals(name, "Transfer-Encoding")) {
                continue;
            }
            wire += name;
            wire += ": ";
            wire += spec.headers.value_at(i);
            wire += "\r\n";
        }
        wire += "\r\n";
        wire += spec.body;

        return send(std::move(wire), !keep_alive);
    }

private:
    Parser::Config make_cfg() const
    {
        Parser::Config pc;
        pc.limits    = m_owner->cfg.limits;
        pc.body_mode = m_owner->cfg.body_mode;
        return pc;
    }

    bool start_read()
    {
        netmech::Buf b;
        b.owner = m_buf;
        b.data  = m_buf->data();
        b.size  = m_buf->size();
        return m_conn->read(b, netmech::ReadMode::some);
    }

    bool send(std::string wire, bool close_after)
    {
        if (!m_conn) {
            return false;
        }
        auto payload = std::make_shared<std::string>(std::move(wire));
        netmech::ConstBuf cb;
        cb.owner = payload;
        cb.data  = payload->data();
        cb.size  = payload->size();
        m_pending.push_back(payload);
        if (!m_conn->write(cb)) {
            return false;
        }
        m_close_after = close_after;
        return true;
    }

    /// 一笔请求结束：先回调应用，再把控制权交回 Impl
    void finish(std::error_code ec, bool keep_alive)
    {
        auto handler = std::move(m_handler);
        m_handler.reset();
        if (handler) {
            handler->on_complete(ec);
        }
        m_owner->complete();
        if (!keep_alive) {
            close_conn();
        }
    }

    void pump()
    {
        for (;;) {
            const auto r = m_parser.next(m_ec);
            if (m_ec) {
                finish(m_ec, false);
                return;
            }
            switch (r) {
            case Parser::Result::need_more:
                return;

            case Parser::Result::message: {
                const HttpResponse& resp = m_parser.response();
                const bool ka = resp.keep_alive && m_owner->cfg.keep_alive &&
                                m_owner->served_on_conn < m_owner->cfg.max_requests_per_connection;
                if (m_handler) {
                    m_handler->on_response(resp);
                }
                finish({}, ka);
                return;
            }

            case Parser::Result::headers:
                if (m_handler) {
                    m_handler->on_response(m_parser.response());
                }
                break;

            case Parser::Result::body_chunk:
                if (m_handler) {
                    m_handler->on_body_data(m_parser.body_chunk());
                }
                break;

            case Parser::Result::message_end: {
                const bool ka = m_parser.response().keep_alive;
                finish({}, ka);
                return;
            }
            }
        }
    }

    std::shared_ptr<HttpClient::Impl>        m_owner;
    std::shared_ptr<netmech::Conn>           m_conn;
    std::shared_ptr<std::vector<char>>       m_buf;
    Parser                                   m_parser;
    std::error_code                          m_ec;
    std::deque<std::shared_ptr<std::string>> m_pending;

    std::shared_ptr<ResponseHandler> m_handler;
    bool                             m_keep_alive  = true;
    bool                             m_close_after = false;
    std::atomic<bool>                m_closed{false};
};

// --- Impl ---

void HttpClient::Impl::on_connected(std::shared_ptr<HttpClientSession> s)
{
    // 注意：这里已经有"在途"的那一笔（has_current == true），不能再调
    // advance() —— 它看到 has_current 会直接返回，于是请求永远发不出去。
    Job job;
    {
        std::lock_guard<std::mutex> lk(mu);
        conn = s;
        if (!has_current) {
            // 连接是空闲时建立的（例如上一笔刚完），交给 advance 取新活
            has_current = false;
        } else {
            job = current;
        }
    }
    if (job.handler) {
        s->begin(job.spec, job.handler, cfg.keep_alive);
    } else {
        advance();
    }
}

void HttpClient::Impl::ensure_ticker()
{
    bool need = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!ticker_started) {
            ticker_started = true;
            need           = true;
        }
    }
    if (!need) {
        return;
    }
    ticker.start([self = weak_from_this()] {
        auto o = self.lock();
        if (!o) {
            return;
        }
        std::shared_ptr<HttpClientSession> s;
        {
            std::lock_guard<std::mutex> lk(o->mu);
            if (!o->has_current ||
                std::chrono::steady_clock::now() <= o->current.deadline) {
                return;
            }
            o->timed_out = true;   // 收尾在 on_close 里做，保证回调线程一致
            s            = o->conn;
        }
        if (s) {
            s->close_conn();
        }
    });
}

void HttpClient::Impl::advance()
{
    Job                                job;
    std::shared_ptr<HttpClientSession> sess;
    bool                               need_connect = false;

    {
        std::lock_guard<std::mutex> lk(mu);
        if (closed || has_current || queue.empty()) {
            return;
        }
        current          = std::move(queue.front());
        queue.pop_front();
        has_current      = true;
        timed_out        = false;
        current.deadline = std::chrono::steady_clock::now() + cfg.request_timeout;
        job              = current;
        sess             = conn;
        if (!sess || !sess->usable()) {
            need_connect = true;
        }
    }

    ensure_ticker();

    if (!need_connect && sess) {
        sess->begin(job.spec, job.handler, cfg.keep_alive);
        return;
    }
    do_connect();
}

void HttpClient::Impl::do_connect()
{
    bool already = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (connecting) {
            already = true;
        } else {
            connecting     = true;
            conn.reset();
            served_on_conn = 0;
        }
    }
    if (already) {
        return;
    }

    auto lb = std::make_shared<HttpLinkable>();
    lb->owner = weak_from_this();
    {
        std::lock_guard<std::mutex> lk(mu);
        linkable = lb;                 // 保活：否则 connect 一返回就失效
    }
    const auto ec = mech->connect(
        cfg.remote, lb, static_cast<std::uint32_t>(cfg.connect_timeout.count()));
    if (ec) {
        {
            std::lock_guard<std::mutex> lk(mu);
            connecting = false;
        }
        fail_all(ec);
    }
}

void HttpClient::Impl::complete()
{
    {
        std::lock_guard<std::mutex> lk(mu);
        has_current = false;
    }
    advance();
}

void HttpClient::Impl::on_conn_closed()
{
    Job  job;
    bool had         = false;
    bool was_timeout = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        conn.reset();
        was_timeout = timed_out;
        timed_out   = false;
        if (has_current) {
            job         = current;
            had         = true;
            has_current = false;
        }
    }
    if (had) {
        failures.fetch_add(1, std::memory_order_relaxed);
        if (job.handler) {
            // 超时给专门的错误码，其余按"连接断了"上报
            job.handler->on_complete(was_timeout
                                         ? make_error_code(Errc::client_timeout)
                                         : make_error_code(Errc::not_connected));
        }
    }
    advance();
}

void HttpClient::Impl::fail_all(std::error_code ec)
{
    std::deque<Job> pending_jobs;
    Job             inflight;
    bool            had_inflight = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        pending_jobs.swap(queue);
        if (has_current) {
            inflight     = current;
            had_inflight = true;
            has_current  = false;
        }
    }
    if (had_inflight && inflight.handler) {
        inflight.handler->on_complete(ec);
    }
    for (auto& j : pending_jobs) {
        if (j.handler) {
            j.handler->on_complete(ec);
        }
    }
}

std::shared_ptr<netmech::ConnHandler> HttpLinkable::on_linkup(netmech::Conn& /*c*/)
{
    auto o = owner.lock();
    if (!o) {
        return nullptr;
    }
    std::shared_ptr<HttpClientSession> s;
    {
        std::lock_guard<std::mutex> lk(o->mu);
        o->connecting = false;
        o->conn       = std::make_shared<HttpClientSession>(o);
        s             = o->conn;
        ++o->served_on_conn;
    }
    return s;
}

// --- HttpClient 对外接口 ---

HttpClient::HttpClient() : m_impl(std::make_shared<Impl>()) {}

HttpClient::~HttpClient()
{
    close();
    m_impl->ticker.stop();
}

std::shared_ptr<HttpClient> HttpClient::create(std::shared_ptr<netmech::Mechanism> mech,
                                               ClientConfig                       cfg)
{
    if (!mech) {
        return nullptr;
    }
    auto c          = std::shared_ptr<HttpClient>(new HttpClient());
    c->m_impl->mech = std::move(mech);
    c->m_impl->cfg  = std::move(cfg);
    return c;
}

bool HttpClient::request(RequestSpec spec, std::shared_ptr<ResponseHandler> handler)
{
    if (!handler) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->closed || m_impl->queue.size() >= m_impl->cfg.max_pending_requests) {
            return false;
        }
        m_impl->queue.push_back(Impl::Job{std::move(spec), std::move(handler), {}});
    }
    m_impl->total.fetch_add(1, std::memory_order_relaxed);
    m_impl->advance();
    return true;
}

void HttpClient::close() noexcept
{
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->closed) {
            return;
        }
        m_impl->closed = true;
    }
    if (m_impl->conn) {
        m_impl->conn->close_conn();
    }
    m_impl->fail_all(make_error_code(Errc::not_connected));
}

bool HttpClient::connected() const noexcept
{
    auto s = m_impl->conn;
    return s && s->usable();
}

std::size_t HttpClient::pending() const noexcept
{
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->queue.size() + (m_impl->has_current ? 1 : 0);
}

std::uint64_t HttpClient::requests_total() const noexcept
{
    return m_impl->total.load();
}

std::uint64_t HttpClient::failures_total() const noexcept
{
    return m_impl->failures.load();
}

// =============================================================================
//  WebSocketClient
// =============================================================================

class WsHandshakeSession;

struct WebSocketClient::Impl : public std::enable_shared_from_this<WebSocketClient::Impl> {
    std::shared_ptr<netmech::Mechanism> mech;
    WebSocketClientConfig               cfg;
    WebSocketClientHandlers             handlers;

    std::mutex  mu;
    bool        connecting = false;
    bool        open       = false;
    std::string client_key;         ///< 握手用的一次性 key
    std::string expected_accept;    ///< 由它算出来的期望 accept
    std::chrono::steady_clock::time_point handshake_deadline;

    std::shared_ptr<netmech::Linkable>     linkable;    ///< 必须保活
    std::shared_ptr<netmech::Conn>         conn;
    std::shared_ptr<detail::WsSessionImpl> ws;
    Parser                                 parser{Parser::Role::client};
    std::deque<std::shared_ptr<std::string>> pending_out;

    Ticker ticker{std::chrono::milliseconds(200)};

    void on_bytes(span<const std::uint8_t> data);
    void finish_handshake();
    void fail(std::error_code ec);
};

namespace {

class WsLinkable : public netmech::Linkable {
public:
    std::weak_ptr<WebSocketClient::Impl> owner;

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override;

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        if (auto o = owner.lock()) {
            {
                std::lock_guard<std::mutex> lk(o->mu);
                o->connecting = false;
            }
            o->fail(ec);
        }
    }
};

/// 握手阶段：发 Upgrade 请求、读 101、成功后把连接交给帧层。
class WsHandshakeSession : public netmech::ConnHandler,
                           public std::enable_shared_from_this<WsHandshakeSession> {
public:
    explicit WsHandshakeSession(std::shared_ptr<WebSocketClient::Impl> owner)
        : m_owner(std::move(owner))
    {
        m_buf = std::make_shared<std::vector<char>>(16 * 1024);
    }

    bool on_open(std::shared_ptr<netmech::Conn> c) override
    {
        m_conn        = c;
        m_owner->conn = c;
        return send_handshake() && start_read();
    }

    void on_read(std::error_code ec, netmech::Buf b, std::size_t n) override
    {
        if (!m_conn) {
            return;
        }
        if (ec) {
            m_owner->fail(ec);
            return;
        }
        m_owner->on_bytes(span<const std::uint8_t>{
            static_cast<const std::uint8_t*>(b.data), n});

        // 注意：**读循环始终在这个 ConnHandler 手里**（netmech 只认它）。
        // 握手完成后字节交给帧层，但读还得我们继续发起，否则连接就停了。
        if (!start_read()) {
            if (!m_owner->open) {
                m_owner->fail(make_error_code(Errc::not_connected));
            }
        }
    }

    void on_write(std::error_code ec, netmech::ConstBuf /*b*/, std::size_t /*n*/) override
    {
        if (!m_pending.empty()) {
            m_pending.pop_front();
        }
        if (ec) {
            m_owner->fail(ec);
        }
    }

    void on_close() override
    {
        m_conn.reset();
        if (!m_owner->open) {
            m_owner->fail(make_error_code(Errc::not_connected));
        }
    }

private:
    bool send_handshake()
    {
        const auto& cfg = m_owner->cfg;

        std::string wire;
        wire += "GET ";
        wire += cfg.path.empty() ? "/" : cfg.path;
        wire += " HTTP/1.1\r\nHost: ";
        wire += cfg.host.empty() ? host_of(cfg.remote) : cfg.host;
        wire += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
        wire += m_owner->client_key;
        wire += "\r\nSec-WebSocket-Version: 13\r\n";

        if (!cfg.user_agent.empty()) {
            wire += "User-Agent: ";
            wire += cfg.user_agent;
            wire += "\r\n";
        }
        for (std::size_t i = 0; i < cfg.headers.size(); ++i) {
            const auto& n = cfg.headers.name_at(i);
            if (iequals(n, "Host") || iequals(n, "Upgrade") || iequals(n, "Connection") ||
                iequals(n, "Sec-WebSocket-Key") || iequals(n, "Sec-WebSocket-Version")) {
                continue;
            }
            wire += n;
            wire += ": ";
            wire += cfg.headers.value_at(i);
            wire += "\r\n";
        }
        wire += "\r\n";

        auto payload = std::make_shared<std::string>(std::move(wire));
        netmech::ConstBuf cb;
        cb.owner = payload;
        cb.data  = payload->data();
        cb.size  = payload->size();
        m_pending.push_back(payload);
        return m_conn->write(cb);
    }

    bool start_read()
    {
        netmech::Buf b;
        b.owner = m_buf;
        b.data  = m_buf->data();
        b.size  = m_buf->size();
        return m_conn->read(b, netmech::ReadMode::some);
    }

    std::shared_ptr<WebSocketClient::Impl>   m_owner;
    std::shared_ptr<netmech::Conn>           m_conn;
    std::shared_ptr<std::vector<char>>       m_buf;
    std::deque<std::shared_ptr<std::string>> m_pending;
};

std::shared_ptr<netmech::ConnHandler> WsLinkable::on_linkup(netmech::Conn& /*c*/)
{
    auto o = owner.lock();
    if (!o) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(o->mu);
    o->connecting = false;
    return std::make_shared<WsHandshakeSession>(o);
}

} // namespace

void WebSocketClient::Impl::on_bytes(span<const std::uint8_t> data)
{
    if (open) {
        if (ws) {
            ws->on_bytes(data);
        }
        return;
    }

    std::error_code ec;
    parser.feed(data, ec);
    if (ec) {
        fail(ec);
        return;
    }
    const auto r = parser.next(ec);
    if (ec) {
        fail(ec);
        return;
    }
    if (r != Parser::Result::message) {
        return;   // 响应头还没收全
    }

    const auto& resp = parser.response();
    if (resp.status_code != 101) {
        fail(make_error_code(Errc::bad_websocket_handshake));
        return;
    }
    if (trim_ows(resp.header("Sec-WebSocket-Accept")) != expected_accept) {
        fail(make_error_code(Errc::bad_websocket_handshake));
        return;
    }
    finish_handshake();
}

void WebSocketClient::Impl::finish_handshake()
{
    detail::WsIo io;
    std::weak_ptr<Impl> weak = weak_from_this();

    io.send = [weak](std::string wire, bool /*close_after*/) -> bool {
        auto o = weak.lock();
        if (!o || !o->conn) {
            return false;
        }
        auto payload = std::make_shared<std::string>(std::move(wire));
        netmech::ConstBuf cb;
        cb.owner = payload;
        cb.data  = payload->data();
        cb.size  = payload->size();
        o->pending_out.push_back(payload);   // 保活到写完成
        return o->conn->write(cb);
    };
    io.close = [weak] {
        if (auto o = weak.lock()) {
            if (o->conn) {
                o->conn->close();
            }
        }
    };

    WebSocketHandlers wrapped;
    wrapped.on_open = [this](WebSocketSession& s) {
        open = true;
        if (handlers.on_open) {
            handlers.on_open(s);
        }
    };
    wrapped.on_message = [this](WebSocketSession& s, span<const std::uint8_t> d, bool text) {
        if (handlers.on_message) {
            handlers.on_message(s, d, text);
        }
    };
    wrapped.on_close = [this](WebSocketSession& s, std::uint16_t code, std::string_view why) {
        open = false;
        if (handlers.on_close) {
            handlers.on_close(s, code, why);
        }
    };

    std::string raw_headers;
    for (std::size_t i = 0; i < parser.response().headers.size(); ++i) {
        const auto f = parser.response().headers.all()[i];
        raw_headers.append(f.name.data(), f.name.size());
        raw_headers += ": ";
        raw_headers.append(f.value.data(), f.value.size());
        raw_headers += "\r\n";
    }

    ws = std::make_shared<detail::WsSessionImpl>(
        detail::WsSessionImpl::Role::client, std::move(io), cfg.ws, std::move(wrapped),
        cfg.path, std::string{}, cfg.remote, std::move(raw_headers));

    // 101 之后可能已经跟着帧了：解析器缓冲里的残留原样交给帧层
    const auto leftover = parser.pending_bytes();
    ws->start();
    if (!leftover.empty()) {
        ws->on_bytes(leftover);
    }
}

void WebSocketClient::Impl::fail(std::error_code ec)
{
    {
        std::lock_guard<std::mutex> lk(mu);
        open       = false;
        connecting = false;
    }
    if (conn) {
        conn->close();
    }
    if (handlers.on_error) {
        handlers.on_error(ec);
    }
}

// --- WebSocketClient 对外接口 ---

WebSocketClient::WebSocketClient() : m_impl(std::make_shared<Impl>()) {}

WebSocketClient::~WebSocketClient()
{
    m_impl->ticker.stop();
}

std::shared_ptr<WebSocketClient> WebSocketClient::create(std::shared_ptr<netmech::Mechanism> mech,
                                                         WebSocketClientConfig              cfg)
{
    if (!mech) {
        return nullptr;
    }
    auto c         = std::shared_ptr<WebSocketClient>(new WebSocketClient());
    c->m_impl->mech = std::move(mech);
    c->m_impl->cfg  = std::move(cfg);
    return c;
}

bool WebSocketClient::connect(WebSocketClientHandlers handlers)
{
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->connecting || m_impl->open) {
            return false;
        }
        m_impl->connecting = true;
    }

    m_impl->handlers        = std::move(handlers);
    m_impl->client_key      = make_client_key();
    m_impl->expected_accept = make_websocket_accept(m_impl->client_key);
    m_impl->handshake_deadline =
        std::chrono::steady_clock::now() + m_impl->cfg.handshake_timeout;

    // 握手超时：只关连接，收尾走 on_close → fail()
    m_impl->ticker.start([self = m_impl] {
        std::lock_guard<std::mutex> lk(self->mu);
        if (self->open || !self->connecting) {
            return;
        }
        if (std::chrono::steady_clock::now() <= self->handshake_deadline) {
            return;
        }
        if (self->conn) {
            self->conn->close();   // 触发 on_close → fail(not_connected)
        }
    });

    auto lb = std::make_shared<WsLinkable>();
    lb->owner = m_impl;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->linkable = lb;   // 保活
    }

    const auto ec = m_impl->mech->connect(
        m_impl->cfg.remote, lb,
        static_cast<std::uint32_t>(m_impl->cfg.connect_timeout.count()));
    if (ec) {
        {
            std::lock_guard<std::mutex> lk(m_impl->mu);
            m_impl->connecting = false;
        }
        if (m_impl->handlers.on_error) {
            m_impl->handlers.on_error(ec);
        }
        return false;
    }
    return true;
}

void WebSocketClient::close(std::uint16_t code, std::string_view reason)
{
    if (m_impl->ws) {
        m_impl->ws->close(code, reason);
    }
}

bool WebSocketClient::connecting() const noexcept
{
    return m_impl->connecting;
}

bool WebSocketClient::is_open() const noexcept
{
    return m_impl->open;
}

std::uint64_t WebSocketClient::messages_received() const noexcept
{
    return m_impl->ws ? m_impl->ws->messages_received() : 0;
}

} // namespace httpmech
