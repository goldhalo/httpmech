// =============================================================================
//  httpmech - SHA-1 / Base64 实现
// =============================================================================
//  SHA-1 按 RFC 3174 实现；Base64 按 RFC 4648 §4（标准字母表 + '=' 填充）。
//  只服务于 WebSocket 握手，追求"短、可读、无依赖"，不追求极限速度。
// =============================================================================
#include "sha1_base64.hpp"

#include <cstring>
#include <array>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <string>

namespace httpmech {
namespace detail {

namespace {

inline std::uint32_t rotl(std::uint32_t v, int n) noexcept
{
    return (v << n) | (v >> (32 - n));
}

} // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) noexcept
{
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    // 消息 + 0x80 + 若干 0 + 64 位长度（大端）：分块处理
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;

    std::uint8_t block[64];
    std::size_t  filled = 0;
    std::array<std::uint8_t, 8> len_be{};
    for (int i = 0; i < 8; ++i) {
        len_be[7 - i] = static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF);
    }

    auto process = [&](const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(p[i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };

    const auto* p   = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t left = data.size();

    while (left >= 64) {
        process(p);
        p += 64;
        left -= 64;
    }

    // 尾巴：剩余字节 + 0x80 + 补零
    filled = 0;
    std::memcpy(block, p, left);
    filled = left;
    block[filled++] = 0x80;

    if (filled > 56) {
        std::memset(block + filled, 0, 64 - filled);
        process(block);
        filled = 0;
    }
    std::memset(block + filled, 0, 56 - filled);
    std::memcpy(block + 56, len_be.data(), 8);
    process(block);

    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return out;
}

std::string base64_encode(const std::uint8_t* data, std::size_t len)
{
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[i + 2]));
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

} // namespace detail
} // namespace httpmech
