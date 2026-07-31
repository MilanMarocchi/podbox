// hash58, the iTunesDB checksum for iPod classic and nano 3G-5G.
//
// The algorithm was reverse-engineered by wtbw and first implemented by
// Christophe Fergeau in libgpod's itdb_hash58.c, which carries this notice:
//
//   Copyright (C) 2007 Christophe Fergeau <teuf@gnome.org>
//   The code in this file is heavily based on the proof-of-concept code
//   written by wtbw
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//     1. Redistributions of source code must retain the above copyright
//        notice, this list of conditions and the following disclaimer.
//     2. Redistributions in binary form must reproduce the above copyright
//        notice, this list of conditions and the following disclaimer in the
//        documentation and/or other materials provided with the distribution.
//     3. The name of the author may not be used to endorse or promote
//        products derived from this software without specific prior written
//        permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
//   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
//   IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
//   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
//   NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
//   THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The shape of it: derive a 64-byte HMAC key from the device's FireWire GUID,
// then HMAC-SHA1 the whole database with three header fields zeroed. The two
// lookup tables the key derivation needs turn out to be the standard AES
// S-box and its inverse, so they are generated here rather than tabulated —
// which also means there is no 512 bytes of magic data to mistype.

#include "itdb/hash58.h"

#include <array>
#include <cctype>
#include <cstring>

namespace podbox {
namespace {

constexpr std::size_t kHashOffset = 0x58;
constexpr std::size_t kMinDbSize = 0x6C;

// The 18 bytes mixed into the key derivation. This one really is magic: it
// comes out of Apple's code and means nothing on its own.
constexpr std::uint8_t kFixed[18] = {
    0x67, 0x23, 0xFE, 0x30, 0x45, 0x33, 0xF8, 0x90, 0x99,
    0x21, 0x07, 0xC1, 0xD0, 0x12, 0xB2, 0xA1, 0x07, 0x81,
};

// --- AES S-box, generated rather than tabulated ---------------------------
//
// Multiplication in GF(2^8) with the Rijndael polynomial 0x11B, used only to
// build the S-box below.
std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
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

SBoxes buildSBoxes() {
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

const SBoxes& sboxes() {
    static const SBoxes s = buildSBoxes();
    return s;
}

// --- SHA-1 ----------------------------------------------------------------

std::uint32_t rol(std::uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

void sha1Block(std::uint32_t h[5], const std::uint8_t* p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) |
               (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::vector<std::uint8_t> sha1(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                          0xC3D2E1F0};
    std::size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1Block(h, data + i);

    // Tail: the remainder, 0x80, zero padding, then the bit length big-endian.
    std::uint8_t tail[128] = {};
    const std::size_t rem = len - i;
    if (rem) std::memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    const std::size_t tailLen = (rem >= 56) ? 128 : 64;
    const std::uint64_t bits = std::uint64_t(len) * 8;
    for (int k = 0; k < 8; ++k)
        tail[tailLen - 1 - k] = std::uint8_t(bits >> (8 * k));
    for (std::size_t off = 0; off < tailLen; off += 64) sha1Block(h, tail + off);

    std::vector<std::uint8_t> out(20);
    for (int k = 0; k < 5; ++k) {
        out[k * 4] = std::uint8_t(h[k] >> 24);
        out[k * 4 + 1] = std::uint8_t(h[k] >> 16);
        out[k * 4 + 2] = std::uint8_t(h[k] >> 8);
        out[k * 4 + 3] = std::uint8_t(h[k]);
    }
    return out;
}

std::vector<std::uint8_t> hmacSha1(const std::uint8_t* key, std::size_t keyLen,
                                   const std::uint8_t* data,
                                   std::size_t dataLen) {
    std::uint8_t k[64] = {};
    if (keyLen > 64) {
        const std::vector<std::uint8_t> d = sha1(key, keyLen);
        std::memcpy(k, d.data(), d.size());
    } else if (keyLen) {
        std::memcpy(k, key, keyLen);
    }

    std::vector<std::uint8_t> inner(64 + dataLen);
    for (int i = 0; i < 64; ++i) inner[i] = std::uint8_t(k[i] ^ 0x36);
    if (dataLen) std::memcpy(inner.data() + 64, data, dataLen);
    const std::vector<std::uint8_t> innerHash = sha1(inner.data(), inner.size());

    std::vector<std::uint8_t> outer(64 + innerHash.size());
    for (int i = 0; i < 64; ++i) outer[i] = std::uint8_t(k[i] ^ 0x5C);
    std::memcpy(outer.data() + 64, innerHash.data(), innerHash.size());
    return sha1(outer.data(), outer.size());
}

std::vector<std::uint8_t> parseFirewireGuid(const std::string& hex) {
    // SysInfoExtended reports it as 16 hex characters, sometimes with an 0x
    // prefix or surrounding whitespace. The prefix has to go before non-hex
    // characters are dropped, or its 'x' vanishes and its '0' is mistaken for
    // a digit.
    std::size_t b = hex.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const std::size_t e = hex.find_last_not_of(" \t\r\n");
    std::string s = hex.substr(b, e - b + 1);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);

    if (s.size() != 16) return {};
    for (const char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};

    std::vector<std::uint8_t> out(8);
    for (int i = 0; i < 8; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        out[i] = std::uint8_t((nib(s[i * 2]) << 4) | nib(s[i * 2 + 1]));
    }
    return out;
}

std::vector<std::uint8_t> hash58Key(const std::vector<std::uint8_t>& fwguid) {
    if (fwguid.size() != 8) return {};
    const SBoxes& sb = sboxes();

    // Each pair of GUID bytes contributes its LCM, split into high and low
    // bytes and pushed through both S-boxes.
    auto gcd = [](int a, int b) {
        while (b) {
            const int t = a % b;
            a = b;
            b = t;
        }
        return a;
    };
    std::uint8_t y[16];
    for (int i = 0; i < 4; ++i) {
        const int a = fwguid[i * 2], b = fwguid[i * 2 + 1];
        const int l = (a == 0 || b == 0) ? 1 : (a * b) / gcd(a, b);
        const std::uint8_t hi = std::uint8_t((l & 0xFF00) >> 8);
        const std::uint8_t lo = std::uint8_t(l & 0xFF);
        y[i * 4] = sb.fwd[hi];
        y[i * 4 + 1] = sb.inv[hi];
        y[i * 4 + 2] = sb.fwd[lo];
        y[i * 4 + 3] = sb.inv[lo];
    }

    std::vector<std::uint8_t> seed;
    seed.insert(seed.end(), std::begin(kFixed), std::end(kFixed));
    seed.insert(seed.end(), std::begin(y), std::end(y));
    const std::vector<std::uint8_t> digest = sha1(seed.data(), seed.size());

    // The key is the 20-byte digest in a 64-byte buffer, zero-padded — which
    // is also what HMAC would do with a 20-byte key, but the derivation is
    // specified in terms of the padded buffer so it is built explicitly.
    std::vector<std::uint8_t> key(64, 0);
    std::memcpy(key.data(), digest.data(), digest.size());
    return key;
}

std::vector<std::uint8_t> storedHash58(const std::vector<std::uint8_t>& db) {
    if (db.size() < kHashOffset + 20) return {};
    return {db.begin() + kHashOffset, db.begin() + kHashOffset + 20};
}

std::vector<std::uint8_t> hash58OfDatabase(
    std::vector<std::uint8_t>& db, const std::vector<std::uint8_t>& fwguid) {
    if (db.size() < kMinDbSize || std::memcmp(db.data(), "mhbd", 4) != 0)
        return {};
    const std::vector<std::uint8_t> key = hash58Key(fwguid);
    if (key.empty()) return {};

    // Three header fields must read as zero while hashing: the database id at
    // 0x18, an unmodelled 20-byte field at 0x32, and the hash slot itself.
    // They are restored before returning, so the caller's image is unchanged.
    std::uint8_t backup18[8], backup32[20], backup58[20];
    std::memcpy(backup18, db.data() + 0x18, 8);
    std::memcpy(backup32, db.data() + 0x32, 20);
    std::memcpy(backup58, db.data() + kHashOffset, 20);
    std::memset(db.data() + 0x18, 0, 8);
    std::memset(db.data() + 0x32, 0, 20);
    std::memset(db.data() + kHashOffset, 0, 20);

    const std::vector<std::uint8_t> mac =
        hmacSha1(key.data(), key.size(), db.data(), db.size());

    std::memcpy(db.data() + 0x18, backup18, 8);
    std::memcpy(db.data() + 0x32, backup32, 20);
    std::memcpy(db.data() + kHashOffset, backup58, 20);
    return mac;
}

bool writeHash58(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& fwguid) {
    const std::vector<std::uint8_t> mac = hash58OfDatabase(db, fwguid);
    if (mac.size() != 20) return false;
    std::memcpy(db.data() + kHashOffset, mac.data(), 20);
    return true;
}

}  // namespace podbox
