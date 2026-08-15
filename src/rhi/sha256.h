#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace vnm::msdf_text::rhi::detail {

/**
 * @brief Incremental SHA-256 (FIPS 180-4).
 *
 * Internal to the shared text component, which uses it only to derive the
 * content identity of a built font snapshot from the inputs that produced it.
 * A standard digest is used so the identity of the same font is stable across
 * rebuilds and processes without a collision caveat attached to the contract.
 */
class Sha256
{
public:
    static constexpr std::size_t k_digest_bytes = 32;

    using digest_t = std::array<std::uint8_t, k_digest_bytes>;

    void update(std::span<const std::uint8_t> data)
    {
        m_total_bytes += data.size();

        std::size_t offset = 0;
        if (m_block_used > 0) {
            const std::size_t fill = std::min(k_block_bytes - m_block_used, data.size());
            std::memcpy(m_block.data() + m_block_used, data.data(), fill);
            m_block_used += fill;
            offset        = fill;
            if (m_block_used < k_block_bytes) {
                return;
            }
            compress(m_block.data());
            m_block_used = 0;
        }

        while (data.size() - offset >= k_block_bytes) {
            compress(data.data() + offset);
            offset += k_block_bytes;
        }

        const std::size_t tail = data.size() - offset;
        if (tail > 0) {
            std::memcpy(m_block.data(), data.data() + offset, tail);
            m_block_used = tail;
        }
    }

    void update_u32(std::uint32_t value)
    {
        const std::array<std::uint8_t, 4> bytes = {
            static_cast<std::uint8_t>((value >> 24) & 0xFFu),
            static_cast<std::uint8_t>((value >> 16) & 0xFFu),
            static_cast<std::uint8_t>((value >>  8) & 0xFFu),
            static_cast<std::uint8_t>( value        & 0xFFu),
        };
        update(bytes);
    }

    void update_f32(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        update_u32(bits);
    }

    void update_f64(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        update_u32(static_cast<std::uint32_t>(bits >> 32));
        update_u32(static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
    }

    [[nodiscard]] digest_t finish()
    {
        const std::uint64_t bit_length = m_total_bytes * 8u;

        constexpr std::uint8_t k_pad_start = 0x80;
        update(std::span<const std::uint8_t>(&k_pad_start, 1));

        constexpr std::uint8_t k_pad_zero = 0x00;
        while (m_block_used != k_block_bytes - 8u) {
            update(std::span<const std::uint8_t>(&k_pad_zero, 1));
        }

        for (int shift = 56; shift >= 0; shift -= 8) {
            m_block[m_block_used++] =
                static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu);
        }
        compress(m_block.data());
        m_block_used = 0;

        digest_t digest{};
        for (std::size_t i = 0; i < m_state.size(); ++i) {
            digest[i * 4u + 0u] = static_cast<std::uint8_t>((m_state[i] >> 24) & 0xFFu);
            digest[i * 4u + 1u] = static_cast<std::uint8_t>((m_state[i] >> 16) & 0xFFu);
            digest[i * 4u + 2u] = static_cast<std::uint8_t>((m_state[i] >>  8) & 0xFFu);
            digest[i * 4u + 3u] = static_cast<std::uint8_t>( m_state[i]        & 0xFFu);
        }
        return digest;
    }

private:
    static constexpr std::size_t k_block_bytes = 64;

    static constexpr std::uint32_t rotr(std::uint32_t value, int bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    void compress(const std::uint8_t* block)
    {
        static constexpr std::array<std::uint32_t, 64> k_round = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::array<std::uint32_t, 64> w{};
        for (std::size_t t = 0; t < 16; ++t) {
            w[t] =
                (static_cast<std::uint32_t>(block[t * 4u + 0u]) << 24) |
                (static_cast<std::uint32_t>(block[t * 4u + 1u]) << 16) |
                (static_cast<std::uint32_t>(block[t * 4u + 2u]) <<  8) |
                 static_cast<std::uint32_t>(block[t * 4u + 3u]);
        }
        for (std::size_t t = 16; t < 64; ++t) {
            const std::uint32_t s0 =
                rotr(w[t - 15],  7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >>  3);
            const std::uint32_t s1 =
                rotr(w[t -  2], 17) ^ rotr(w[t -  2], 19) ^ (w[t -  2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }

        std::uint32_t a = m_state[0];
        std::uint32_t b = m_state[1];
        std::uint32_t c = m_state[2];
        std::uint32_t d = m_state[3];
        std::uint32_t e = m_state[4];
        std::uint32_t f = m_state[5];
        std::uint32_t g = m_state[6];
        std::uint32_t h = m_state[7];

        for (std::size_t t = 0; t < 64; ++t) {
            const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp_1 = h + big_s1 + choice + k_round[t] + w[t];
            const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t major  = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp_2 = big_s0 + major;

            h = g;
            g = f;
            f = e;
            e = d + temp_1;
            d = c;
            c = b;
            b = a;
            a = temp_1 + temp_2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<std::uint32_t, 8> m_state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<std::uint8_t, k_block_bytes> m_block{};
    std::size_t                            m_block_used  = 0;
    std::uint64_t                          m_total_bytes = 0;
};

} // namespace vnm::msdf_text::rhi::detail
