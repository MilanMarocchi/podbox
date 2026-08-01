#pragma once

// The AES S-box and its inverse, generated rather than tabulated. Building
// them from the GF(2^8) definition takes microseconds once and means there is
// no 512 bytes of magic constants to mistype — the same trick hash58's key
// derivation used. Both checksums need these, so they live here instead of
// being regenerated twice.

#include <array>
#include <cstdint>

namespace podbox {
namespace aes {

// Multiplication in GF(2^8) with the Rijndael polynomial 0x11B, used only to
// build the S-box below.
inline std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        const bool hi = a & 0x80;
        a = std::uint8_t(a << 1);
        if (hi) a ^= 0x1B;
        b = std::uint8_t(b >> 1);
    }
    return p;
}

struct SBoxes {
    std::array<std::uint8_t, 256> fwd{};
    std::array<std::uint8_t, 256> inv{};
};

inline SBoxes buildSBoxes() {
    SBoxes s;
    // Multiplicative inverse in GF(2^8), then the affine transform.
    std::array<std::uint8_t, 256> inverse{};
    inverse[0] = 0;
    for (int a = 1; a < 256; ++a)
        for (int b = 1; b < 256; ++b)
            if (gmul(std::uint8_t(a), std::uint8_t(b)) == 1) {
                inverse[a] = std::uint8_t(b);
                break;
            }
    for (int i = 0; i < 256; ++i) {
        // The affine step: x ^ rotl(x,1) ^ rotl(x,2) ^ rotl(x,3) ^ rotl(x,4),
        // then ^ 0x63.
        const std::uint8_t x = inverse[i];
        std::uint8_t v = x, acc = x;
        for (int r = 0; r < 4; ++r) {
            acc = std::uint8_t((acc << 1) | (acc >> 7));
            v ^= acc;
        }
        s.fwd[i] = std::uint8_t(v ^ 0x63);
    }
    for (int i = 0; i < 256; ++i) s.inv[s.fwd[i]] = std::uint8_t(i);
    return s;
}

inline const SBoxes& sboxes() {
    static const SBoxes s = buildSBoxes();
    return s;
}

}  // namespace aes
}  // namespace podbox
