// =============================================================================
//  httpmech - 端点（IP + 端口）
// =============================================================================
//  文件    : httpmech/endpoint.hpp
//  用途    : 配置与查询里用到的地址值类型。**不依赖 netmech** —— 本库的公共头
//            一行 netmech 都不出现，装出去的包也不要求使用者准备 netmech 的头。
//  编码    : UTF-8
//
//  ★ 为什么不用 netmech::Address
//    那会把 netmech 的类型钉进本库的公共 API：使用者哪怕只想用 httpmech，
//    也得把 netmech 的头挂在 include 路径上。这里是纯值语义（host + port），
//    自己给一个更省事。实现里再和 netmech 的地址互转（见 src/net_bridge.hpp）。
// =============================================================================
#pragma once

#include "httpmech/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace httpmech {

/// 一个 IP 端点（IP 字面量 + 端口）。**不做域名解析** —— 设备直连场景地址
/// 基本都来自配置里的 IP 字面量。
///
/// @par 用法
/// @code
/// httpmech::Endpoint ep{"127.0.0.1", 8080};
/// if (auto p = httpmech::Endpoint::parse("192.168.3.10:9898")) { ... }
/// @endcode
class HTTPMECH_API Endpoint {
public:
    // 不写 noexcept：std::string 成员的默认构造理论上可抛 bad_alloc，
    // 显式 noexcept 与隐式规格不符会被 GCC 9 判为 deleted（README 承诺 GCC 9+）。
    Endpoint() = default;

    /// 由 host 字面量与端口构造。不校验 host（拼接时才用得到合法性）。
    Endpoint(std::string host, std::uint16_t port) noexcept
        : m_host(std::move(host)), m_port(port) {}

    /// 由 `"1.2.3.4:502"` / `"[::1]:9000"` / `":9000"`（等价 0.0.0.0:9000）解析。
    /// @return 解析失败返回 std::nullopt。
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view text) noexcept;

    /// 通配地址（0.0.0.0:port）。
    [[nodiscard]] static Endpoint any(std::uint16_t port) { return Endpoint{"0.0.0.0", port}; }

    [[nodiscard]] const std::string& host() const noexcept { return m_host; }
    [[nodiscard]] std::uint16_t      port() const noexcept { return m_port; }

    /// 是否通配（0.0.0.0:0）。
    [[nodiscard]] bool is_unspecified() const noexcept;

    /// `"1.2.3.4:502"` / `"[::1]:502"`。
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Endpoint& a, const Endpoint& b) noexcept
    {
        return a.m_port == b.m_port && a.m_host == b.m_host;
    }
    friend bool operator!=(const Endpoint& a, const Endpoint& b) noexcept { return !(a == b); }

private:
    std::string   m_host = "0.0.0.0";
    std::uint16_t m_port = 0;
};

} // namespace httpmech
