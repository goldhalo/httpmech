// =============================================================================
//  httpmech - 头部字段的实现
// =============================================================================
#include <httpmech/headers.hpp>

#include <string_view>
#include <cstddef>
#include <string>

namespace httpmech {

static inline unsigned char lower(unsigned char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view s) noexcept
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
        --e;
    }
    return s.substr(b, e - b);
}

// =============================================================================
//  HeadersView
// =============================================================================

std::string_view HeadersView::get(std::string_view name) const noexcept
{
    for (const auto& f : m_fields) {
        // picohttpparser 对续行（obs-fold）会给出 name == nullptr 的条目，
        // 那些是上一个字段的延续，跳过。
        if (f.name.empty() && f.value.empty()) {
            continue;
        }
        if (iequals(f.name, name)) {
            return f.value;
        }
    }
    return {};
}

std::size_t HeadersView::count(std::string_view name) const noexcept
{
    std::size_t n = 0;
    for (const auto& f : m_fields) {
        if (iequals(f.name, name)) {
            ++n;
        }
    }
    return n;
}

// =============================================================================
//  Headers
// =============================================================================

void Headers::set(std::string_view name, std::string_view value)
{
    remove(name);
    add(name, value);
}

void Headers::add(std::string_view name, std::string_view value)
{
    m_items.emplace_back(std::string{name}, std::string{value});
}

std::size_t Headers::remove(std::string_view name)
{
    std::size_t removed = 0;
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (iequals(it->first, name)) {
            it = m_items.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::string_view Headers::get(std::string_view name) const noexcept
{
    for (const auto& kv : m_items) {
        if (iequals(kv.first, name)) {
            return kv.second;
        }
    }
    return {};
}

std::size_t Headers::count(std::string_view name) const noexcept
{
    std::size_t n = 0;
    for (const auto& kv : m_items) {
        if (iequals(kv.first, name)) {
            ++n;
        }
    }
    return n;
}

void Headers::clear() noexcept
{
    m_items.clear();
    m_view_cache.clear();
}

HeadersView Headers::view() const noexcept
{
    m_view_cache.clear();
    m_view_cache.reserve(m_items.size());
    for (const auto& kv : m_items) {
        m_view_cache.push_back(HeaderField{std::string_view{kv.first}, std::string_view{kv.second}});
    }
    return HeadersView{span<const HeaderField>{m_view_cache.data(), m_view_cache.size()}};
}

} // namespace httpmech
