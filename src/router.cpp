// =============================================================================
//  httpmech - 路由表实现
// =============================================================================
#include <httpmech/router.hpp>

#include <chrono>
#include <string_view>
#include <vector>
#include <cstddef>
#include <string>
#include <utility>

namespace httpmech {

struct Router::Impl {
    struct Route {
        Method      method = Method::get;
        std::string path;
        Handler     handler;
    };

    struct WsRoute {
        std::string       path;
        WebSocketHandlers handlers;
    };

    std::vector<Route>     routes;
    std::vector<WsRoute>   ws_routes;
    std::vector<BeforeHook> befores;
    std::vector<AfterHook>  afters;
    Handler                not_found;
};

Router::Router() : m_impl(new Impl()) {}

Router::~Router() = default;

void Router::route(Method m, std::string_view path, Handler h)
{
    m_impl->routes.push_back(Impl::Route{m, std::string{path}, std::move(h)});
}

void Router::websocket(std::string_view path, WebSocketHandlers handlers)
{
    m_impl->ws_routes.push_back(Impl::WsRoute{std::string{path}, std::move(handlers)});
}

void Router::set_not_found(Handler h)
{
    m_impl->not_found = std::move(h);
}

void Router::before(BeforeHook h)
{
    m_impl->befores.push_back(std::move(h));
}

void Router::after(AfterHook h)
{
    m_impl->afters.push_back(std::move(h));
}

std::size_t Router::route_count() const noexcept
{
    return m_impl->routes.size();
}

std::size_t Router::websocket_route_count() const noexcept
{
    return m_impl->ws_routes.size();
}

void Router::on_request(HttpRequest& req, ResponseWriter& out)
{
    Impl& d = *m_impl;

    bool short_circuited = false;

    // ---- 前置钩子：鉴权等，返回 false 表示已回响应 ----
    for (auto& hook : d.befores) {
        if (!hook(req, out)) {
            short_circuited = true;
            break;
        }
    }

    if (!short_circuited) {
        // ---- WebSocket 路由优先（同一路径可能既是 GET 又是 WS）----
        bool ws_matched = false;
        for (auto& w : d.ws_routes) {
            if (w.path == req.path) {
                ws_matched = true;
                if (!out.upgrade_websocket(w.handlers)) {
                    // 不是合法升级请求，或本版本尚未实现 WS
                    out.set_status(Status::not_implemented);
                    out.end_status(Status::not_implemented, "websocket not available");
                }
                break;
            }
        }

        if (!ws_matched) {
            // ---- 普通路由：先按 (方法, 路径) 精确匹配 ----
            const Impl::Route* hit = nullptr;
            bool               path_exists = false;
            std::string        allow;

            for (auto& r : d.routes) {
                if (r.path != req.path) {
                    continue;
                }
                path_exists = true;
                if (!allow.empty()) {
                    allow.append(", ");
                }
                allow.append(to_string(r.method));
                if (r.method == req.method) {
                    hit = &r;
                    break;
                }
            }

            if (hit != nullptr) {
                hit->handler(req, out);
            } else if (path_exists) {
                out.add_header("Allow", allow);
                out.end_status(Status::method_not_allowed, "method not allowed");
            } else if (d.not_found) {
                d.not_found(req, out);
            } else {
                out.end_status(Status::not_found, "not found");
            }
        }
    }

    // ---- 后置钩子：注册到「响应完成」事件 ----
    // 同步响应由 finish_writer() 在写出后立刻触发，时机几乎不变；
    // 异步响应体则在最后一块落地时才触发，这样 status / body_bytes /
    // elapsed 才是真实值。请求视图在回调返回后失效，所以先做一份快照。
    if (!d.afters.empty()) {
        struct AfterContext {
            HttpRequest req{};
            std::string method_text, target, path, query;
            Headers     headers;
        };
        auto ctx = std::make_shared<AfterContext>();
        ctx->method_text = std::string{req.method_text};
        ctx->target      = std::string{req.target};
        ctx->path        = std::string{req.path};
        ctx->query       = std::string{req.query};
        for (const auto& f : req.headers.all()) {
            ctx->headers.add(f.name, f.value);
        }
        ctx->req                    = req;
        ctx->req.method_text        = ctx->method_text;
        ctx->req.target             = ctx->target;
        ctx->req.path               = ctx->path;
        ctx->req.query              = ctx->query;
        ctx->req.headers            = ctx->headers.view();
        ctx->req.body               = {};
        ctx->req.user               = req.user;
        ctx->req.from_trusted_proxy = req.from_trusted_proxy;

        std::weak_ptr<Router> weak = weak_from_this();
        out.set_completion_hook_([weak, ctx](const ResponseSummary& res) {
            if (auto self = weak.lock()) {
                for (auto& hook : self->m_impl->afters) {
                    hook(ctx->req, res);
                }
            }
        });
    }
}

} // namespace httpmech
