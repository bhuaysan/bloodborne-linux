#include "common/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace bb::common {
namespace {

// FIPS 180-4 section 4.2.2.
constexpr std::array<std::uint32_t, 64> k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t load_be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

}  // namespace

// FIPS 180-4 section 6.2.2.
void Sha256::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w;
    for (int t = 0; t < 16; ++t) {
        w[t] = load_be32(block + 4 * t);
    }
    for (int t = 16; t < 64; ++t) {
        std::uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        std::uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = s1 + w[t - 7] + s0 + w[t - 16];
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (int t = 0; t < 64; ++t) {
        std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + sum1 + ch + k[t] + w[t];
        std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = sum0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    total_bytes_ += n;
    while (n > 0) {
        if (buffered_ == 0 && n >= buffer_.size()) {
            compress(p);
            p += buffer_.size();
            n -= buffer_.size();
            continue;
        }
        std::size_t take = std::min(n, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, p, take);
        buffered_ += take;
        p += take;
        n -= take;
        if (buffered_ == buffer_.size()) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }
}

// Padding per FIPS 180-4 section 5.1.1: a 1 bit, zeros, then the message length in bits.
Sha256Digest Sha256::finish() {
    std::uint64_t bits = total_bytes_ * 8;
    std::array<std::uint8_t, 72> pad{0x80};
    std::size_t zeros = (buffered_ < 56 ? 56 : 120) - buffered_;
    for (int i = 0; i < 8; ++i) {
        pad[zeros + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    update(std::as_bytes(std::span(pad.data(), zeros + 8)));

    Sha256Digest out;
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (int j = 0; j < 4; ++j) {
            out[4 * i + j] = static_cast<std::uint8_t>(state_[i] >> (24 - 8 * j));
        }
    }
    return out;
}

Sha256Digest sha256(std::span<const std::byte> data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

std::string to_hex(const Sha256Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : digest) {
        out += digits[b >> 4];
        out += digits[b & 0xf];
    }
    return out;
}

}  // namespace bb::common
