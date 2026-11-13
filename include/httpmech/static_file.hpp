// =============================================================================
//  httpmech - 静态文件服务
// =============================================================================
//  文件    : httpmech/static_file.hpp
//  用途    : 一个可选的 RequestHandler 实现：把 URL 前缀映射成本地目录，
//            支持 MIME、ETag/Last-Modified/304、单段 Range、HEAD、目录索引。
//
//  依赖    : C++17 标准库 + 本库 server/router（不引 netmech）。
//  编码    : UTF-8
//
//  ★ 定位：库核心仍然是"只做协议"，这里是**独立可选组件**
//    - 不往 ServerConfig 里加字段，保持公开配置结构体的 ABI 稳定；
//    - 不修改 ResponseWriter 的发送链路，因此只适用于「缓冲模式」的小文件；
//    - 想接管全部请求：直接把 StaticFileHandler 交给 HttpServer::create()；
//    - 想在已有路由表里做兜底：用 mount_static(router, cfg)，它挂的是
//      Router::set_not_found()，所以 before 钩子（鉴权）依然先于文件访问。
//
//  ★ 发送链路（阶段 3）
//    文件通过 `ResponseWriter::set_body_provider()` 分块投递：由
//    `ServerSession::on_write()` 驱动下一块，每连接最多 1 块（256 KiB）在途。
//    因此内存占用与文件大小无关，也不再受
//    `ServerConfig::max_buffered_response` 约束（那个阈值只管老的 write()
//    自动切 chunked 路径）。慢客户端靠 `max_send_queue_bytes` 背压。
//
//    @note provider 目前在该连接的 io 线程上同步 read() 一块。本地磁盘下
//          代价可控；若根目录在慢速网络盘上，接入方应自行把文件 IO 挪到
//          业务线程池再经 `Conn::post()` 回投（阶段 4 的零拷贝优化点）。
//
//  ★ 线程
//    on_request() 只读取配置（启动后不再修改），无共享可变状态，
//    多条连接并发调用是安全的。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <httpmech/headers.hpp>
#include <httpmech/router.hpp>
#include <httpmech/server.hpp>

namespace httpmech {

// =============================================================================
//  小工具（公开出来便于接入方复用与单元测试）
// =============================================================================

/// 解码 URL 路径里的 `%XX`。非法转义（`%`、`%G1`）一律失败。
/// 解码结果可能包含 NUL / `\` / 新的 `/`，请接着调用 `is_safe_url_path()`。
[[nodiscard]] HTTPMECH_API bool percent_decode_path(std::string_view in, std::string& out);

/// 静态文件服务允许的路径形态（与 cpp-httplib 的 is_valid_path 同口径）：
/// 必须以 `/` 开头；拒绝 NUL、反斜杠、控制字符；Windows 额外拒绝 `:`
/// （NTFS 流 `a.txt::$DATA`）；拒绝空段（尾部一个 `/` 除外）；
/// 拒绝以 `.` 或空格结尾的段（覆盖 `..` 和 Windows 尾部裁剪）。
///
/// @note 传入的应是**解码后**的路径。
[[nodiscard]] HTTPMECH_API bool is_safe_url_path(std::string_view path) noexcept;

/// 扩展名 → MIME。可带前导 `.`，大小写不敏感；未收录返回空视图。
/// 返回值指向静态表，生命周期与程序相同。
[[nodiscard]] HTTPMECH_API std::string_view mime_type_for_extension(std::string_view ext) noexcept;

// =============================================================================
//  配置
// =============================================================================

struct StaticFileConfig {
    /// URL 前缀，必须以 `/` 开头。`/` 表示整个站点。
    /// 匹配按**路径段边界**进行：`/public` 不会吞掉 `/publicity/x`。
    std::string mount_point = "/";

    /// 本地根目录（UTF-8）。不存在或不是目录时 `valid()` 返回 false。
    std::string root;

    /// 以 `/` 结尾的路径自动追加这个文件名。
    std::string index_file = "index.html";

    /// 扩展名未收录时使用的类型。
    std::string default_content_type = "application/octet-stream";

    /// 自定义/覆盖 MIME 映射。key 小写、不带前导 `.`，如 `{"mjs","text/javascript"}`。
    std::map<std::string, std::string> mime_types;

    /// 每个静态响应追加的头（如 `Cache-Control`）。
    /// @note 不要在这里设置 `Content-Length` / `Transfer-Encoding` / `Content-Range`，
    ///       这些由处理器按实际文件长度生成。
    Headers extra_headers;

    /// 可选的单个文件大小上限（字节）。0 = 不限制（默认）。
    /// 现在走异步响应体通道，这个上限只表达业务策略（超过回 403），
    /// 不再是「内存装不下」的技术限制。
    std::size_t max_file_bytes = 0;

    /// 是否处理 `Range`（单段）。多段 `bytes=a-b,c-d` 按 RFC 9110 允许忽略。
    bool enable_range = true;

    /// 是否生成 `ETag` / `Last-Modified` 并处理条件请求。
    bool enable_etag = true;

    /// 目录请求（不带尾 `/`）是否回 301 到 `path/`。
    bool redirect_directory = true;
};

// =============================================================================
//  处理器
// =============================================================================

/// 静态文件处理器。可以直接作为 `HttpServer::create()` 的 handler，
/// 也可以用 `mount_static()` 挂成路由表的 404 兜底。
///
/// @par 路径安全
///   URL → percent-decode → `is_safe_url_path()` → 拼进 root →
///   `weakly_canonical()` → 与规范化后的 root 做前缀比较。
///   符号链接若解析到 root 之外一律回 403；Windows 上比较大小写不敏感。
///
/// @par 支持的语义
///   GET/HEAD（其余 405 + `Allow: GET, HEAD`）、目录索引、301 目录重定向、
///   MIME、`ETag`/`Last-Modified`、`If-None-Match`/`If-Modified-Since`/304、
///   `If-Range`、单段 `Range`/206/416、`Accept-Ranges`、404/403/500。
///   不做多段 multipart/byteranges、gzip、目录列表、sendfile。
class HTTPMECH_API StaticFileHandler : public RequestHandler {
public:
    explicit StaticFileHandler(StaticFileConfig cfg);
    ~StaticFileHandler() override;

    StaticFileHandler(const StaticFileHandler&)            = delete;
    StaticFileHandler& operator=(const StaticFileHandler&) = delete;

    /// 配置是否可用（root 存在且是目录、mount_point 合法）。
    [[nodiscard]] bool valid() const noexcept;

    /// 配置无效时的原因（有效时为空）。
    [[nodiscard]] const std::string& error_message() const noexcept;

    /// 规范化后的根目录（诊断/测试用）。
    [[nodiscard]] const std::string& resolved_root() const noexcept;

    [[nodiscard]] const StaticFileConfig& config() const noexcept;

    // --- RequestHandler ---
    void on_request(HttpRequest& req, ResponseWriter& out) override;

    /// 与 `on_request` 等价，但按 const 请求传入，
    /// 便于挂到 `Router::set_not_found()`（它的回调是 const 引用）。
    void serve(const HttpRequest& req, ResponseWriter& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// 便捷：创建处理器并挂到 `router->set_not_found()`。
///
/// @note `before` 钩子在 not_found 之前执行，所以鉴权仍然生效
///       （cpp-httplib 的 mount point 会跳过 pre_request_handler，此处不会）。
/// @return 处理器；`router` 为空时也返回它（只是不挂载）。
[[nodiscard]] HTTPMECH_API std::shared_ptr<StaticFileHandler> mount_static(
    const std::shared_ptr<Router>& router, StaticFileConfig cfg);

} // namespace httpmech
