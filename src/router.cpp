// =============================================================================
//  httpmech - 路由表实现
// =============================================================================
#include <httpmech/router.hpp>

#include <chrono>

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
    const auto t0 = std::chrono::steady_clock::now();

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

    // ---- 后置钩子：记日志。此时响应已写完，能拿到的只有标量 ----
    if (!d.afters.empty()) {
        ResponseSummary sum;
        sum.status     = out.status_code();
        sum.body_bytes = out.body_bytes();
        sum.upgraded   = out.upgraded();
        sum.keep_alive = req.keep_alive;
        sum.elapsed    = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);

        for (auto& hook : d.afters) {
            hook(req, sum);
        }
    }
}

} // namespace httpmech
