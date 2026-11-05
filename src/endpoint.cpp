// =============================================================================
//  httpmech - Endpoint 的实现
// =============================================================================
//  这里的解析/格式化**委托给 netmech::Address**，而不是自己再写一份：
//  地址字面量的解析（IPv6 方括号、通配 ":9000" 之类）细节不少，复制一份
//  只会多出一处需要同步维护的实现。公共头仍然不引 netmech —— 依赖只落在
//  这个 .cpp 里。
// =============================================================================
#include <httpmech/endpoint.hpp>

#include <netmech/common.hpp>

#include <optional>
#include <string_view>
#include <string>

namespace httpmech {

std::optional<Endpoint> Endpoint::parse(std::string_view text) noexcept
{
    const auto a = netmech::Address::parse(text);
    if (!a) {
        return std::nullopt;
    }
    return Endpoint{a->address_string(), a->port()};
}

bool Endpoint::is_unspecified() const noexcept
{
    return netmech::Address(m_host, m_port).is_unspecified();
}

std::string Endpoint::to_string() const
{
    return netmech::Address(m_host, m_port).to_string();
}

} // namespace httpmech
