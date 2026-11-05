// =============================================================================
//  httpmech - 内部：WebSocket 会话实现
// =============================================================================
//  与 ServerSession 的关系：这里**不直接依赖** server.cpp 的类型，
//  只通过 `WsIo` 里的两个回调往外发字节 / 关传输。这样帧层可以单独测，
//  也不会把 server.cpp 的细节拖进来。
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <httpmech/endpoint.hpp>
#include <httpmech/websocket.hpp>

namespace httpmech {
namespace detail {

/// 帧层往外说话用的两个动作。
struct WsIo {
    /// 发送原始字节；close_after = true 表示写完就把 TCP 关了。
    std::function<bool(std::string, bool)> send;
    /// 立刻关闭底层传输（不优雅）。
    std::function<void()> close;
    /// 已受理但还没写完的字节数（背压判据）。
    std::function<std::size_t()> pending;
    /// 底层传输是否已经关死（用来区分"反压"和"连接没了"）。
    std::function<bool()> is_closed;
};

class WsSessionImpl : public WebSocketSession,
                      public std::enable_shared_from_this<WsSessionImpl> {
public:
    /// 两端在掩码上是不对称的（RFC 6455 §5.1）：
    ///   客户端 → 服务端：**必须**打掩码
    ///   服务端 → 客户端：**必须不打**掩码
    /// 所以同一个帧层要按角色决定"收的时候要不要求掩码、发的时候打不打"。
    enum class Role : std::uint8_t { server, client };

    WsSessionImpl(Role                        role,
                  WsIo                        io,
                  WebSocketConfig             cfg,
                  WebSocketHandlers           handlers,
                  std::string                 path,
                  std::string                 real_ip,
                  Endpoint                     peer,
                  std::string                 handshake_headers_raw);

    ~WsSessionImpl() override = default;

    // --- WebSocketSession ---
    bool send_text(std::string_view text) override;
    bool send_binary(span<const std::uint8_t> data) override;
    bool send_ping(span<const std::uint8_t> payload) override;

    void close(std::uint16_t code, std::string_view reason) override;

    [[nodiscard]] bool                 is_open() const noexcept override;
    [[nodiscard]] Endpoint             remote() const noexcept override { return m_peer; }
    [[nodiscard]] std::string_view     real_ip() const noexcept override { return m_real_ip; }
    [[nodiscard]] std::string_view     path() const noexcept override { return m_path; }
    [[nodiscard]] std::string_view     handshake_header(std::string_view name) const noexcept override;
    [[nodiscard]] std::size_t          pending_send_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t        messages_received() const noexcept override { return m_recv_msgs; }

    // --- 供 ServerSession 驱动 ---

    /// 握手完成后调用一次：触发 on_open。
    void start();

    /// 喂入原始字节（由 TCP 读回调转来）。
    void on_bytes(span<const std::uint8_t> data);

    /// 周期性检查（ping / 空闲超时）。
    void tick(std::chrono::steady_clock::time_point now);

    /// 连接已断开：只清理状态，不再往回写。
    void on_transport_closed();

private:
    enum class State { open, closing, closed };

    void        feed_one_frame(std::error_code& ec);
    void        deliver_message(std::string&& payload, bool is_text);
    bool        send_frame(std::uint8_t opcode, bool fin, span<const std::uint8_t> payload,
                           bool close_after = false);
    /// 传输层已经不可用：通知应用一次 on_close，然后关干净。
    void        fail_transport();

    void        protocol_error(std::uint16_t code, std::string_view why);

    Role                               m_role;
    WsIo                               m_io;
    WebSocketConfig                    m_cfg;
    WebSocketHandlers                  m_handlers;
    std::string                        m_path;
    std::string                        m_real_ip;
    Endpoint                           m_peer;
    std::string                        m_headers_raw;

    std::string          m_in;          ///< 接收缓冲（未消费字节）
    std::string          m_msg;         ///< 分片重组中的消息
    bool                 m_msg_is_text = false;
    bool                 m_in_fragment = false;

    // 下面这几个字段会被**两个线程**碰：io 线程（读写回调）与服务器的
    // sweeper 线程（tick → 自动 ping / 空闲超时）。send_*() 还可能来自应用线程。
    // 所以一律用原子量，而不是"靠调用方保证单线程"。
    std::atomic<State>   m_state{State::open};
    std::atomic<bool>    m_close_sent{false};
    std::uint64_t        m_recv_msgs = 0;

    std::atomic<std::uint32_t>         m_rng{0x9E3779B9u};    ///< 客户端掩码（原子递增取数）

    // 时间戳统一存成 steady_clock 的纳秒计数（int64），才能放进原子量。
    std::atomic<std::int64_t> m_last_rx_ns{0};
    std::atomic<std::int64_t> m_last_ping_ns{0};
    std::atomic<std::int64_t> m_close_deadline_ns{0};   ///< 0 = 未设置
};

} // namespace detail
} // namespace httpmech
