// =============================================================================
//  httpmech - HTTP/1.1 增量解析器
// =============================================================================
//  文件    : httpmech/parser.hpp
//  用途    : 把 TCP 字节流切成一条条 HTTP 消息。服务端与客户端共用同一份实现，
//            靠 `Role` 区分解析请求行还是状态行。
//  依赖    : C++17 标准库 + picohttpparser（vendored，thirdparty/picohttpparser）
//  编码    : UTF-8
//
//  ★ 为什么自带拼包缓冲，而不是"调用方给缓冲、解析器只做视图"
//    一条头部的边界会跨 read：这次读到 "Content-Ty"，下次才读到 "pe: ...
//    \r\n\r\n"。如果视图指向调用方的两块不同内存，就得在消息层做拼接 ——
//    等于把复杂度推给每个使用者。这里由解析器持有一块可增长的缓冲，
//    视图永远指向**同一块**内存，应用拿到的 string_view 天然连续。
//    代价是一次内存搬移（缓冲满时 grow），换来所有使用者的代码简单。
//
//  ★ 两种 body 模式，**二选一，不自动切换**
//    aggregate：攒够整条 body 才回调。适合配置接口、JSON 这类小消息。
//    stream   ：头一到就回调，body 分块给。适合文件上传、日志回灌。
//    不做"超过 N 就自动转流式" —— 那会让应用的处理路径取决于报文大小，
//    是最难查的一类 bug。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstdint>
#include <memory>
#include <system_error>

#include <httpmech/common.hpp>
#include <httpmech/message.hpp>
#include <httpmech/span.hpp>

namespace httpmech {

/// HTTP/1.1 增量解析器。
///
/// @par 用法（聚合模式，服务端）
/// @code
/// httpmech::Parser p{httpmech::Parser::Role::server, cfg};
/// std::error_code ec;
/// auto r = p.feed(span{buf, n}, ec);
/// if (r == httpmech::Parser::Result::message) {
///     handler.on_request(p.request());   // p.request().body 是完整的
///     p.reset();                         // 准备下一条（keep-alive）
/// }
/// @endcode
///
/// @par 线程模型
/// 非线程安全。一个连接一个 Parser，只在该连接的 io 线程上用 —— 这与
/// netmech "同一连接的回调串行" 的契约天然吻合，不需要加锁。
class HTTPMECH_API Parser {
public:
    enum class Role : std::uint8_t {
        server,   ///< 解析请求（server → 收请求行）
        client,   ///< 解析响应（client → 收状态行）
    };

    enum class BodyMode : std::uint8_t {
        aggregate,   ///< 攒完整个 body 再回调（默认）
        stream,      ///< 头到了就回调，body 分块给
    };

    struct Config {
        Limits   limits{};
        BodyMode body_mode = BodyMode::aggregate;

        /// 初始接收缓冲大小（默认 8 KiB）。会按需增长到 max_header_bytes 量级。
        std::size_t initial_buffer_size = 8 * 1024;
    };

    /// `feed()` 的返回。
    enum class Result : std::uint8_t {
        need_more = 0,   ///< 数据不够，继续收
        message,         ///< [aggregate] 整条消息就绪（含 body）
        headers,         ///< [stream] 头部就绪，`request()` / `response()` 可用
        body_chunk,      ///< [stream] 一块 body，见 `body_chunk()`
        message_end,     ///< [stream] body 结束，本条消息处理完
    };

    explicit Parser(Role role);
    Parser(Role role, Config cfg);
    ~Parser();

    Parser(const Parser&)            = delete;
    Parser& operator=(const Parser&) = delete;

    /// 喂入一段字节（可以是任意长度，包括 1 字节）。
    ///
    /// @param[in]  data 新收到的数据。
    /// @param[out] ec   出错时填 `httpmech::Errc::*`；成功时被清空。
    /// @return 见 `Result`。出错时返回 `need_more`，请以 `ec` 为准。
    ///
    /// @par 一次 feed 可能产出多次事件
    /// 内部循环解析，直到数据用尽或需要更多数据。所以一次 feed 里
    /// 可能出现"headers → body_chunk → ... → message_end"多个事件 ——
    /// 调用方应写成循环：反复调用 `next(ec)` 取事件，直到它返回
    /// `need_more` 或本条消息结束。
    Result feed(span<const std::uint8_t> data, std::error_code& ec);

    /// 从已缓存的字节里继续取下一个事件（不喂新数据）。
    ///
    /// @note `feed()` 内部已经调用过，一般不需要直接用；提供出来是为了
    ///       让"一次 feed 多个事件"的循环写法更清楚。
    Result next(std::error_code& ec);

    /// 告诉解析器"连接已经结束（读到 FIN 了）"。
    ///
    /// @note 只有一种情况需要它：响应既没有 Content-Length 也没有 chunked
    ///       （RFC 9112 §6.3），此时 body 的结束位置**只能**由连接关闭确定。
    ///       客户端在 `on_close` 里调一次，随后 `next()` 就会把这条消息收尾。
    void feed_eof();

    /// 是否正在等"连接结束"来定界（见 `feed_eof()`）。
    [[nodiscard]] bool awaiting_eof() const noexcept;

    /// 当前消息的头部是否已解析完成（不论 body 是否收齐）。
    ///
    /// 服务端用它切换超时判据：聚合模式下 body 还在途时，头部已收齐
    /// 就应进入 body 阶段（body_idle_timeout），而不是继续用
    /// header_timeout 从首字节计时误杀慢上传。
    [[nodiscard]] bool headers_complete() const noexcept;

    /// 当前消息。仅当结果为 `message` / `headers` / `body_chunk` / `message_end`
    /// 时有效。
    [[nodiscard]] const HttpRequest&  request() const noexcept;
    [[nodiscard]] const HttpResponse& response() const noexcept;

    /// 可写引用。**只给 server 层用**：它要在派发前挂 `req.user` /
    /// `req.from_trusted_proxy` 这两个应用可见的字段。
    ///
    /// @warning 其它字段一律视同只读 —— 改了会让下一跳看到不一致的报文语义。
    [[nodiscard]] HttpRequest& request_mut() noexcept;

    /// 流式模式下的一块 body。仅当结果为 `body_chunk` 时有效。
    [[nodiscard]] span<const std::uint8_t> body_chunk() const noexcept;

    /// 一条消息处理完毕，准备解析下一条（keep-alive）。
    ///
    /// @par 为什么必须显式调用
    /// 解析器不知道自己什么时候"消费完"上一条 —— 应用可能在回调里还要用
    /// 那些视图。`reset()` 明确表示"上一条的视图我不要了"，此时解析器才会
    /// 回收缓冲、切到下一条。
    ///
    /// @note reset 之后如果缓冲里还留着下一条的字节（pipelining），
    ///       下一次 `next()` 会直接开始解析它，不需要重新 feed。
    void reset() noexcept;

    /// 缓冲里未被消费的字节（只读视图，下一次 feed / reset 前有效）。
    ///
    /// 用途：客户端收到 `101 Switching Protocols` 之后，对端可能已经
    /// 把第一批 WS 帧紧跟在响应头后面发过来了 —— 那些字节还在解析器缓冲里，
    /// 必须原样交给帧层，否则会丢帧。
    [[nodiscard]] span<const std::uint8_t> pending_bytes() const noexcept;

    /// 缓冲里还有多少未被消费的字节。
    /// 典型用途：`while (p.pending() > 0) { p.reset(); p.next(ec); ... }`
    [[nodiscard]] std::size_t pending() const noexcept;

    /// 已成功解析的消息数（诊断 / 连接级限流用）。
    [[nodiscard]] std::uint64_t messages() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace httpmech
