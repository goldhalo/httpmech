// =============================================================================
//  httpmech - 路由表
// =============================================================================
//  文件    : httpmech/router.hpp
//  用途    : 一个开箱即用的 `RequestHandler` 实现：按 (方法, 路径) 精确匹配，
//            带 before / after 钩子。不用它也行 —— 直接实现 RequestHandler
//            接管所有请求。
//  依赖    : C++17 标准库。
//  编码    : UTF-8
//
//  ★ 为什么库要自带路由（此前设计是不做的）
//    参考公司现有实现（wtoe::HttpProtocol 的 IHttpServer::Attr::setRoute），
//    业务侧的用法就是"注册一批 (方法, URL) → 回调"，另加 before（鉴权）/
//    after（操作日志）两个钩子。这层薄封装不到 200 行，让接入方不用自己写；
//    但**不做**路径参数解析（`/dev/:id`）、通配符、正则 —— 那些才是复杂度所在。
//
//  ★ 与参考实现的差异：路径大小写
//    参考实现用 ci_map（大小写不敏感）存路由。HTTP 的 path 按 RFC 9110 是
//    **大小写敏感**的，本库按规范实现精确匹配。若接入方现有路由存在
//    "/API/xxx" 这类写法，迁移时需要统一大小写 —— 这属于一次性成本，
//    比"默默接受错误大小写"更可控。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httpmech/common.hpp>
#include <httpmech/message.hpp>
#include <httpmech/server.hpp>
#include <httpmech/websocket.hpp>

namespace httpmech {

/// 精确匹配的 HTTP 路由表。
///
/// @par 用法
/// @code
/// auto router = std::make_shared<httpmech::Router>();
///
/// router->before([](httpmech::HttpRequest& req, httpmech::ResponseWriter& out) {
///     if (!check_token(req.header("Authorization"))) {
///         out.end_status(httpmech::Status::unauthorized, "no token");
///         return false;              // 短路，不再走路由
///     }
///     return true;
/// });
///
/// router->get("/api/status", [](const httpmech::HttpRequest&, httpmech::ResponseWriter& out) {
///     out.set_content_type("application/json");
///     out.write(R"({"ok":true})");
///     out.end();
/// });
///
/// router->after([](const httpmech::HttpRequest& req,
///                  const httpmech::ResponseSummary& res) {
///     log("%s %s -> %u (%zu B, %lld ms)", ..., res.status, res.body_bytes,
///         static_cast<long long>(res.elapsed.count()));
/// });
///
/// router->websocket("/ws/events", handlers);
///
/// auto srv = httpmech::HttpServer::create(mech, cfg, router);
/// @endcode
///
/// @par 线程
/// 注册接口（`get` / `post` / `before` …）**必须在 `HttpServer::start()` 之前**
/// 调完，之后只读。运行期不再加锁 —— 路由表是启动期配置，不是运行期状态。
class HTTPMECH_API Router : public RequestHandler,
               public std::enable_shared_from_this<Router> {
public:
    /// 处理函数。`out` 必须以 `end()` 或 `upgrade_websocket()` 收尾。
    ///
    /// @note 请求按 **const 引用**传入：路由只读。要挂上下文（token、用户、
    ///       计时起点）请在 `before` 钩子里写 `req.user`，这里读出来用。
    using Handler = std::function<void(const HttpRequest&, ResponseWriter&)>;

    /// 前置钩子（鉴权）。返回 false 表示已自行回响应，不再往下走。
    ///
    /// @note 请求按**非 const 引用**传入 —— 这是唯一能写 `req.user` 的地方，
    ///       写进去的对象会一路传到路由与 `after`。
    using BeforeHook = std::function<bool(HttpRequest&, ResponseWriter&)>;

    /// 后置钩子（记访问/操作日志）。
    ///
    /// @param[in] req 这条请求；`req.user` 仍可用（before 里挂的上下文还在）。
    /// @param[in] res 响应摘要（状态码、body 字节数、耗时、是否升级、是否复用）。
    using AfterHook = std::function<void(const HttpRequest&, const ResponseSummary&)>;

    Router();
    ~Router() override;

    // --- 注册（启动前）---

    void route(Method m, std::string_view path, Handler h);

    void get(std::string_view path, Handler h)     { route(Method::get, path, std::move(h)); }
    void post(std::string_view path, Handler h)    { route(Method::post, path, std::move(h)); }
    void put(std::string_view path, Handler h)     { route(Method::put, path, std::move(h)); }
    void del(std::string_view path, Handler h)     { route(Method::delete_, path, std::move(h)); }
    void patch(std::string_view path, Handler h)   { route(Method::patch, path, std::move(h)); }
    void head(std::string_view path, Handler h)    { route(Method::head, path, std::move(h)); }

    /// 注册一条 WebSocket 路由。库内部完成握手，业务只处理三条回调。
    void websocket(std::string_view path, WebSocketHandlers handlers);

    /// 未匹配到任何路由时的处理（默认回 404）。
    void set_not_found(Handler h);

    void before(BeforeHook h);
    void after(AfterHook h);

    // --- RequestHandler ---

    void on_request(HttpRequest& req, ResponseWriter& out) override;

    [[nodiscard]] std::size_t route_count() const noexcept;
    [[nodiscard]] std::size_t websocket_route_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace httpmech
