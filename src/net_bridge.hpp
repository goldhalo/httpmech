// =============================================================================
//  httpmech - 内部：公共 Endpoint ⇄ netmech::Address 的桥
// =============================================================================
//  公共头里只有 httpmech::Endpoint（不引 netmech）；真正调 netmech 的
//  listen/connect 时才换成 netmech::Address。转换只在这两个函数里，改动面小。
// =============================================================================
#pragma once

#include <netmech/common.hpp>

#include <httpmech/endpoint.hpp>

namespace httpmech {
namespace detail {

/// Endpoint → netmech::Address（listen/connect 用）。
[[nodiscard]] inline netmech::Address to_address(const Endpoint& ep)
{
    return netmech::Address(ep.host(), ep.port());
}

/// netmech::Address → Endpoint（对端地址回传给应用时用）。
[[nodiscard]] inline Endpoint from_address(const netmech::Address& a)
{
    return Endpoint{a.address_string(), a.port()};
}

} // namespace detail
} // namespace httpmech
