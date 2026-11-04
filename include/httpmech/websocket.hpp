// =============================================================================
//  httpmech - WebSocket（服务端）
// =============================================================================
//  文件    : httpmech/websocket.hpp
//  用途    : 在 HTTP/1.1 之上完成 Upgrade 握手，之后把连接切成 WebSocket 帧模式。
//            只实现服务端 —— 我们前面永远有 nginx，客户端连的是 nginx。
//  依赖    : C++17 标准库（握手要 SHA-1 + Base64，实现在 src/ 里，不引第三方）。
//  编码    : UTF-8
//
//  ★ 为什么不复用 boost::beast
//    参考实现在 wtoe::HttpProtocol 里用 beast 的 websocket::stream，省掉了帧层。
//    但 beast 是 Boost 的一部分，而本家族的前提是"只带一份 header-only 的 Asio"。
//    帧层本身不复杂（RFC 6455 §5 只有 7 种 opcode + 3 种长度前缀 + mask），
//    自己写约 300 行，换来零依赖，值得。
//
//  ★ 与 HTTP 共用一个端口
//    nginx 转发的 WebSocket 也是 HTTP 请求（带 Upgrade 头），所以不需要协议嗅探：
//    照常走 HTTP 解析，路由里发现是升级请求就调 upgrade_websocket()。
//    HTTP/2 的 h2c 嗅探那套（参考实现的 ProtocolSniffer）在这里不适用 —— 我们不做 H2。
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <httpmech/common.hpp>
#include <httpmech/endpoint.hpp>
#include <httpmech/span.hpp>

namespace httpmech {

// =============================================================================
//  关闭码（RFC 6455 §7.4.1）
// =============================================================================

enum class WsCloseCode : std::uint16_t {
    normal            = 1000,
    going_away        = 1001,
    protocol_error    = 1002,
    unsupported_data  = 1003,
    no_status         = 1005,   ///< 只出现在"对端没给 status"的接收侧，不能主动发
    abnormal          = 1006,   ///< 同上，不能主动发
    invalid_payload   = 1007,
    policy_violation  = 1008,
    message_too_big   = 1009,
    mandatory_ext     = 1010,
    internal_error    = 1011,
};

/// 帧类型（RFC 6455 §5.2）。应用一般只关心 text / binary。
enum class WsOpcode : std::uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA,
};

// =============================================================================
//  配置
// =============================================================================

struct WebSocketConfig {
    /// 单条消息上限（跨分片累计）。超了回 `1009 Message Too Big` 并关闭。
    std::size_t max_message_bytes = 4 * 1024 * 1024;

    /// 单个帧上限。控制帧另有 125 字节硬限制（RFC 强制）。
    std::size_t max_frame_bytes = 1024 * 1024;

    /// 发送队列上限。超过时 `send_*()` 返回 false（背压，而不是无限攒）。
    std::size_t max_send_queue_bytes = 8 * 1024 * 1024;

    /// 主动 ping 间隔。0 = 不主动 ping。
    std::chrono::milliseconds ping_interval{25'000};

    /// 空闲超时：这么久没收到任何帧（含 pong）就断开。
    std::chrono::milliseconds idle_timeout{120'000};

    /// 是否严格校验文本帧的 UTF-8。默认关 —— 设备侧常发非标准字节，
    /// 严格校验会直接踢连接；要合规就打开。
    bool validate_utf8 = false;
};

// =============================================================================
//  会话
// =============================================================================

/// 一条已升级的 WebSocket 连接。
///
/// @par 生命周期
/// 由库持有 `shared_ptr`，回调里按引用传入。想在回调之外用（例如从业务线程推数据），
/// 请自己持有 shared_ptr —— 连接关闭后 `is_open()` 变 false，`send_*()` 返回 false，
/// 不会崩，也不会访问已释放资源。
///
/// @par 线程
/// 回调在**该连接的 io 线程**上串行执行（与 HTTP 一致）。
/// `send_*()` 可在任意线程调用（内部投递到 io 线程）。
class WebSocketSession {
public:
    virtual ~WebSocketSession() = default;

    // --- 发送 ---

    /// 发一帧文本。返回 false 表示已关闭或发送队列满。
    virtual bool send_text(std::string_view text) = 0;

    /// 发一帧二进制。
    virtual bool send_binary(span<const std::uint8_t> data) = 0;

    /// 发一个 ping（对端应回 pong）。应用一般不用调，库会按配置自动 ping。
    virtual bool send_ping(span<const std::uint8_t> payload = {}) = 0;

    // --- 关闭 ---

    /// 发起关闭握手（发 close 帧，等待对端回 close 或超时）。
    /// 幂等：重复调用无副作用。
    virtual void close(std::uint16_t code = static_cast<std::uint16_t>(WsCloseCode::normal),
                       std::string_view reason = {}) = 0;

    // --- 状态 ---

    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// 对端 TCP 地址（nginx 接的，通常是 127.0.0.1）。
    [[nodiscard]] virtual Endpoint remote() const noexcept = 0;

    /// 真实客户端 IP（同 `HttpRequest::real_ip()`，握手时算好存下来）。
    /// 前提是配了 `ServerConfig::trusted_proxy_cidrs`，否则为空。
    [[nodiscard]] virtual std::string_view real_ip() const noexcept = 0;

    /// 握手时的请求路径（路由匹配用的那个）。
    [[nodiscard]] virtual std::string_view path() const noexcept = 0;

    /// 握手时的完整请求头（只在 on_open 与 on_message 期间有效，之后可能被回收）。
    [[nodiscard]] virtual std::string_view handshake_header(std::string_view name) const noexcept = 0;

    /// 待发送队列的字节数（监控/背压用）。
    [[nodiscard]] virtual std::size_t pending_send_bytes() const noexcept = 0;

    /// 累计收到的消息数。
    [[nodiscard]] virtual std::uint64_t messages_received() const noexcept = 0;
};

// =============================================================================
//  握手中的回调
// =============================================================================

/// 升级成功后的回调集合。在 `ResponseWriter::upgrade_websocket()` 里一次性给出，
/// 避免"先拿到 session 再设回调"之间出现竞态。
struct WebSocketHandlers {
    /// 握手完成、连接已切换为 WS 模式。可以在这里 send_text 打个招呼。
    std::function<void(WebSocketSession&)> on_open;

    /// 收到一条**完整**消息（分片已在库内拼好）。
    ///
    /// @param[in] data    消息内容，指向库内部缓冲，**回调返回即失效**。
    /// @param[in] is_text true = 文本帧，false = 二进制帧。
    std::function<void(WebSocketSession&, span<const std::uint8_t> data, bool is_text)> on_message;

    /// 连接关闭（收到 close 帧、超时、协议错误、TCP 断开都会走到这里，恰好一次）。
    std::function<void(WebSocketSession&, std::uint16_t code, std::string_view reason)> on_close;
};

// =============================================================================
//  握手工具
// =============================================================================

/// 由 `Sec-WebSocket-Key` 算出 `Sec-WebSocket-Accept`。
///
/// @note 只是把 RFC 6455 §4.2.2 的那一步（SHA-1 + Base64）暴露出来，
///       便于单元测试与诊断；正常流程由库内部调用，应用不用管。
[[nodiscard]] std::string make_websocket_accept(std::string_view client_key);

} // namespace httpmech
