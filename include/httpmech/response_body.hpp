// =============================================================================
//  httpmech - 异步响应体通道
// =============================================================================
//  文件    : httpmech/response_body.hpp
//  用途    : 让应用以「分块提供」的方式写大响应，而不必把整个 body 攒在内存里。
//            接口形状参考 cpp-httplib 的 ContentProvider / DataSink，但**语义
//            完全不同**：对方的 write() 是同步阻塞写，这里的 write() 只是
//            「投递一个 ConstBuf」，真正的完成由 ServerSession::on_write() 通知。
//
//  依赖    : 仅 C++17 标准库 + span.hpp（公共头不引 netmech）。
//  编码    : UTF-8
//
//  ★ 用法
//      out.set_content_type("application/octet-stream");
//      out.set_header("Content-Length", std::to_string(size));
//      out.set_body_provider({size,
//          [f](std::uint64_t off, std::size_t len, DataSink& sink) {
//              std::string buf = read_at(off, len);
//              return sink.write(span<const std::uint8_t>{...});
//          }});
//      out.end();
//
//  ★ 约束
//    1. 所有响应头必须在 set_body_provider() / 第一次 write() 之前设置完毕；
//       头一旦发出，set_status() / set_header() 会被忽略（a00718d）。
//    2. length > 0 用 Content-Length 定帧；length == 0 用 chunked，
//       此时 provider 必须调用 done() 结束，否则连接无法复用。
//    3. provider 在**连接所属的 io 线程**上被调用（由 on_write 经 Conn::post
//       投回）。阻塞的文件 IO 请自己丢到 WorkerPool，再 post 回来。
//    4. 每次调用最多投递 length 字节；不投递任何数据会被判为「无进展」，
//       终止响应，防止死循环。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include <httpmech/span.hpp>

namespace httpmech {

namespace detail {
class ServerSession;   ///< 内部：见 src/server.cpp
}

/// 体提供者投递一块数据的句柄。**只在 provider 回调期间有效**，别存下来。
///
/// 不暴露 netmech::Conn，也不暴露 ServerSession，保持公共头的依赖边界。
class HTTPMECH_API DataSink {
public:
    DataSink(const DataSink&)            = delete;
    DataSink& operator=(const DataSink&) = delete;
    DataSink(DataSink&&)                 = delete;
    DataSink& operator=(DataSink&&)      = delete;

    /// 投递一块。返回 false 表示连接已不可写（对端断开 / 已 abort）。
    /// @note data 指向的内存在调用返回前必须有效；库内部会拷贝进在途缓冲，
    ///       provider 返回后可以立即释放。
    bool write(span<const std::uint8_t> data);

    /// 便捷重载：文本 body。
    bool write(std::string_view text);

    /// 是否还能继续投递（看发送队列水位）。
    [[nodiscard]] bool writable() const noexcept;

    /// 提前结束体（仅 length == 0 的 chunked 模式需要；length > 0 时
    /// 提前 done 会因短于 Content-Length 而断连）。
    void done() noexcept;

private:
    friend class detail::ServerSession;
    explicit DataSink(detail::ServerSession* s) noexcept : m_session(s) {}

    detail::ServerSession* m_session = nullptr;
};

/// 体提供者。
/// @param offset 期望的起始偏移（累计已投递字节数）
/// @param length 本次最多可投递的字节数
/// @param sink   投递句柄
/// @return false 表示取数失败，库会终止响应并断连
using ContentProvider =
    std::function<bool(std::uint64_t offset, std::size_t length, DataSink&)>;

/// 注册到 `ResponseWriter::set_body_provider()` 的描述。
struct BodyProviderSpec {
    BodyProviderSpec() = default;

    /// 便捷构造：`BodyProviderSpec{length, provider}` 即可。
    /// @note 显式构造函数而非聚合体，避免调用方漏写 on_complete 时
    ///       触发 -Wmissing-field-initializers（CI 开了 -Werror）。
    BodyProviderSpec(std::uint64_t len, ContentProvider p,
                     std::function<void(bool ok)> cb = {})
        : length(len), provider(std::move(p)), on_complete(std::move(cb)) {}

    /// 响应体总字节数。0 = 未知长度，走 `Transfer-Encoding: chunked`。
    std::uint64_t length = 0;

    ContentProvider provider;

    /// 响应完全结束（或失败）后回调一次。ok=false 表示没发完。
    /// @note 在连接 io 线程上调用，回调里别做阻塞操作。
    std::function<void(bool ok)> on_complete;
};

} // namespace httpmech
