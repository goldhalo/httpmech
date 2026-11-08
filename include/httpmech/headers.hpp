// =============================================================================
//  httpmech - 头部字段
// =============================================================================
//  文件    : httpmech/headers.hpp
//  用途    : 两套东西，别混用 ——
//              HeadersView  借用视图，解析结果用它（零拷贝，不拥有内存）
//              Headers      拥有容器，**构造**请求/响应用它
//  依赖    : 仅 C++17 标准库。
//  编码    : UTF-8
//
//  ★ 大小写
//    HTTP 头部字段名**大小写不敏感**（RFC 9110 §5.1），值不做任何规范化。
//    本库一律用 ASCII 大小写不敏感比较查名字；`get("content-type")` 与
//    `get("Content-Type")` 等价。重复字段（如多个 `Set-Cookie`）按出现顺序
//    全部保留，`get()` 只返回第一个，要全拿请遍历 `all()`。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httpmech/span.hpp>

namespace httpmech {

/// 一个头部字段。`name` / `value` 都是借用视图，生命周期见各持有者说明。
struct HeaderField {
    std::string_view name;
    std::string_view value;
};

/// ASCII 大小写不敏感比较。
[[nodiscard]] HTTPMECH_API bool iequals(std::string_view a, std::string_view b) noexcept;

/// 去掉首尾的 OWS（空格 / 制表符），用于解析出来的字段值。
[[nodiscard]] HTTPMECH_API std::string_view trim_ows(std::string_view s) noexcept;

// =============================================================================
//  借用视图（解析结果用）
// =============================================================================

/// 头部字段的只读视图。
///
/// @warning 字段内容指向**解析器内部的接收缓冲**。它只在"当前这条消息还在处理"
///          期间有效 —— 即从回调进入，到回调返回为止。要跨回调保存请自己拷。
class HTTPMECH_API HeadersView {
public:
    constexpr HeadersView() noexcept = default;
    constexpr explicit HeadersView(span<const HeaderField> fields) noexcept : m_fields(fields) {}

    [[nodiscard]] constexpr span<const HeaderField> all() const noexcept { return m_fields; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return m_fields.size(); }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_fields.empty(); }

    /// 按名字查第一个值（大小写不敏感）。没有返回空 view。
    [[nodiscard]] std::string_view get(std::string_view name) const noexcept;

    /// 是否存在该字段（字段存在但值为空也算存在）。
    [[nodiscard]] bool has(std::string_view name) const noexcept { return count(name) > 0; }

    /// 同名字段出现几次（例如多个 Set-Cookie）。
    [[nodiscard]] std::size_t count(std::string_view name) const noexcept;

private:
    span<const HeaderField> m_fields;
};

// =============================================================================
//  拥有容器（构造请求 / 响应用）
// =============================================================================

/// 头部字段的拥有容器。保持插入顺序，允许同名多次。
///
/// @par 典型用法（服务端写响应）
/// @code
/// out.set_header("Content-Type", "application/json");
/// out.add_header("Set-Cookie", "a=1");
/// out.add_header("Set-Cookie", "b=2");
/// @endcode
class HTTPMECH_API Headers {
public:
    Headers() = default;
    Headers(const Headers&) = default;
    Headers& operator=(const Headers&) = default;

    /// 覆盖：先删掉所有同名，再追加一个。
    void set(std::string_view name, std::string_view value);

    /// 追加：保留已有的同名。
    void add(std::string_view name, std::string_view value);

    /// 删除所有同名。返回删除条数。
    std::size_t remove(std::string_view name);

    [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
    [[nodiscard]] bool has(std::string_view name) const noexcept { return count(name) > 0; }
    [[nodiscard]] std::size_t count(std::string_view name) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }

    [[nodiscard]] const std::string& name_at(std::size_t i) const { return m_items[i].first; }
    [[nodiscard]] const std::string& value_at(std::size_t i) const { return m_items[i].second; }

    void clear() noexcept;

    /// 转成借用视图。**每次调用都会刷新内部缓存**，
    /// 返回的视图在下一次修改本对象（或再次调用 view()）前有效。
    [[nodiscard]] HeadersView view() const noexcept;

private:
    std::vector<std::pair<std::string, std::string>> m_items;
    mutable std::vector<HeaderField>                 m_view_cache;
};

} // namespace httpmech
