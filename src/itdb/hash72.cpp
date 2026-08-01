// hash72, the iTunesDB checksum for the iPod nano 5G.
//
// The algorithm was reverse-engineered by Chris Lee and first implemented in
// libgpod's itdb_hash72.c, which carries this notice:
//
//   Copyright (c) 2009 Chris Lee <clee@mg8.org>
//   Copyright (C) 2009 Christophe Fergeau <cfergeau@mandriva.com>
//   Licensed under the GNU Lesser General Public License, version 2.1
//
//   iTunes and iPod are trademarks of Apple
//   This product is not supported/written/published by Apple!
//
// The shape of it: SHA-1 the whole database (with four header fields zeroed),
// then build a 46-byte signature — the marker 0x01 0x00, 12 random bytes, and
// AES-128-CBC of SHA1||random under a fixed key. The device verifies the
// signature with the (UUID, random, IV) in its HashInfo file, so the random
// bytes and IV are recovered from a database the device already accepts and
// written to HashInfo before any new database can be signed.
//
// The AES used here is the standard Rijndael-128: the S-boxes come from
// itdb/aes_sbox.h (generated, not tabulated), and the cipher is exercised by
// the FIPS-197 vectors in hash72_test so the composition is only as
// mysterious as the parts the algorithm itself keeps secret.

#include "itdb/hash72.h"

#include "itdb/aes_sbox.h"
#include "itdb/hash58.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace podbox {
namespace {

constexpr std::size_t kSignatureOffset = 0x72;
constexpr std::size_t kSignatureSize = 46;
constexpr std::size_t kMinDbSize = 0x6C;

// The fixed AES key hash72 uses. It comes out of Apple's code and is part of
// the algorithm; it was published in libgpod's itdb_hash72.c.
constexpr std::uint8_t kKey[16] = {
    0x61, 0x8c, 0xa1, 0x0d, 0xc7, 0xf5, 0x7f, 0xd3,
    0xb4, 0x72, 0x3e, 0x08, 0x15, 0x74, 0x63, 0xd7,
};

constexpr std::uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                    0x20, 0x40, 0x80, 0x1b, 0x36};

// --- AES-128 --------------------------------------------------------------

std::uint8_t xtime(std::uint8_t x) {
    return std::uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

void addRoundKey(std::uint8_t s[16], const std::uint8_t rk[16]) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

void subBytes(std::uint8_t s[16], bool invert) {
    const auto& box = invert ? aes::sboxes().inv : aes::sboxes().fwd;
    for (int i = 0; i < 16; ++i) s[i] = box[s[i]];
}

void shiftRows(std::uint8_t s[16], bool invert) {
    // The state is column-major (FIPS 197): the bytes of a row are strided —
    // row r is s[r], s[r+4], s[r+8], s[r+12]. Encryption rotates row r left
    // by r (decryption: right by r).
    for (int r = 1; r < 4; ++r) {
        std::uint8_t row[4];
        for (int c = 0; c < 4; ++c) row[c] = s[r + c * 4];
        for (int c = 0; c < 4; ++c) {
            const int src = invert ? (c + 4 - r) % 4 : (c + r) % 4;
            s[r + c * 4] = row[src];
        }
    }
}

// Multiply one byte by the InvMixColumns coefficients: 14/11/13/9 are
// 8^4^2, 8^2^1, 8^4^1 and 8^1 in GF(2^8), so each is a few x-times and a
// couple of XORs.
std::uint8_t mul9(std::uint8_t x) { return std::uint8_t(xtime(xtime(xtime(x))) ^ x); }
std::uint8_t mul11(std::uint8_t x) {
    return std::uint8_t(xtime(xtime(xtime(x))) ^ xtime(x) ^ x);
}
std::uint8_t mul13(std::uint8_t x) {
    return std::uint8_t(xtime(xtime(xtime(x))) ^ xtime(xtime(x)) ^ x);
}
std::uint8_t mul14(std::uint8_t x) {
    return std::uint8_t(xtime(xtime(xtime(x))) ^ xtime(xtime(x)) ^ xtime(x));
}

void mixColumns(std::uint8_t s[16], bool invert) {
    // In the column-major state the bytes of a column are contiguous:
    // column c is s[c*4 .. c*4+4).
    for (int c = 0; c < 4; ++c) {
        const std::uint8_t x0 = s[c * 4], x1 = s[c * 4 + 1], x2 = s[c * 4 + 2],
                           x3 = s[c * 4 + 3];
        if (invert) {
            s[c * 4] = std::uint8_t(mul14(x0) ^ mul11(x1) ^ mul13(x2) ^ mul9(x3));
            s[c * 4 + 1] =
                std::uint8_t(mul9(x0) ^ mul14(x1) ^ mul11(x2) ^ mul13(x3));
            s[c * 4 + 2] =
                std::uint8_t(mul13(x0) ^ mul9(x1) ^ mul14(x2) ^ mul11(x3));
            s[c * 4 + 3] =
                std::uint8_t(mul11(x0) ^ mul13(x1) ^ mul9(x2) ^ mul14(x3));
        } else {
            const std::uint8_t t = x0 ^ x1 ^ x2 ^ x3;
            s[c * 4] = std::uint8_t(x0 ^ xtime(x0 ^ x1) ^ t);
            s[c * 4 + 1] = std::uint8_t(x1 ^ xtime(x1 ^ x2) ^ t);
            s[c * 4 + 2] = std::uint8_t(x2 ^ xtime(x2 ^ x3) ^ t);
            s[c * 4 + 3] = std::uint8_t(x3 ^ xtime(x3 ^ x0) ^ t);
        }
    }
}

void expandKey(const std::uint8_t key[16], std::uint8_t rk[176]) {
    std::memcpy(rk, key, 16);
    const auto& box = aes::sboxes().fwd;
    for (int i = 16; i < 176; i += 4) {
        std::uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            const std::uint8_t k = t[0];
            t[0] = std::uint8_t(box[t[1]] ^ kRcon[i / 16 - 1]);
            t[1] = box[t[2]];
            t[2] = box[t[3]];
            t[3] = box[k];
        }
        for (int j = 0; j < 4; ++j) rk[i + j] = std::uint8_t(rk[i + j - 16] ^ t[j]);
    }
}

void blockEncrypt(const std::uint8_t key[16], const std::uint8_t in[16],
                  std::uint8_t out[16]) {
    std::uint8_t rk[176];
    expandKey(key, rk);
    std::uint8_t s[16];
    std::memcpy(s, in, 16);
    addRoundKey(s, rk);
    for (int round = 1; round < 10; ++round) {
        subBytes(s, false);
        shiftRows(s, false);
        mixColumns(s, false);
        addRoundKey(s, rk + round * 16);
    }
    subBytes(s, false);
    shiftRows(s, false);
    addRoundKey(s, rk + 160);
    std::memcpy(out, s, 16);
}

void blockDecrypt(const std::uint8_t key[16], const std::uint8_t in[16],
                  std::uint8_t out[16]) {
    std::uint8_t rk[176];
    expandKey(key, rk);
    std::uint8_t s[16];
    std::memcpy(s, in, 16);
    addRoundKey(s, rk + 160);
    for (int round = 9; round >= 1; --round) {
        shiftRows(s, true);
        subBytes(s, true);
        addRoundKey(s, rk + round * 16);
        mixColumns(s, true);
    }
    shiftRows(s, true);
    subBytes(s, true);
    addRoundKey(s, rk);
    std::memcpy(out, s, 16);
}

bool isMhbd(const std::vector<std::uint8_t>& v) {
    return v.size() >= 4 && std::memcmp(v.data(), "mhbd", 4) == 0;
}

bool readAll(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

}  // namespace

std::vector<std::uint8_t> aes128EcbEncrypt(const std::vector<std::uint8_t>& key,
                                           const std::vector<std::uint8_t>& data) {
    if (key.size() != 16 || data.empty() || data.size() % 16 != 0) return {};
    std::vector<std::uint8_t> out(data.size());
    for (std::size_t off = 0; off < data.size(); off += 16)
        blockEncrypt(key.data(), data.data() + off, out.data() + off);
    return out;
}

std::vector<std::uint8_t> aes128EcbDecrypt(const std::vector<std::uint8_t>& key,
                                           const std::vector<std::uint8_t>& data) {
    if (key.size() != 16 || data.empty() || data.size() % 16 != 0) return {};
    std::vector<std::uint8_t> out(data.size());
    for (std::size_t off = 0; off < data.size(); off += 16)
        blockDecrypt(key.data(), data.data() + off, out.data() + off);
    return out;
}

std::vector<std::uint8_t> hash72Sha1(std::vector<std::uint8_t>& db) {
    if (db.size() < kMinDbSize || !isMhbd(db)) return {};

    // Four header fields must read as zero while hashing: db_id (0x18), the
    // hash58 slot (0x58), the unmodelled field at 0x32, and the hash72
    // signature itself. They are restored before returning, so the caller's
    // image is unchanged — the signature is written afterwards by writeHash72.
    std::uint8_t backup18[8], backup32[20], backup58[20], backup72[46];
    std::memcpy(backup18, db.data() + 0x18, 8);
    std::memcpy(backup32, db.data() + 0x32, 20);
    std::memcpy(backup58, db.data() + 0x58, 20);
    std::memcpy(backup72, db.data() + kSignatureOffset, kSignatureSize);
    std::memset(db.data() + 0x18, 0, 8);
    std::memset(db.data() + 0x32, 0, 20);
    std::memset(db.data() + 0x58, 0, 20);
    std::memset(db.data() + kSignatureOffset, 0, kSignatureSize);

    const std::vector<std::uint8_t> digest = sha1(db.data(), db.size());

    std::memcpy(db.data() + 0x18, backup18, 8);
    std::memcpy(db.data() + 0x32, backup32, 20);
    std::memcpy(db.data() + 0x58, backup58, 20);
    std::memcpy(db.data() + kSignatureOffset, backup72, kSignatureSize);
    return digest;
}

std::vector<std::uint8_t> hash72Signature(const std::vector<std::uint8_t>& sha1,
                                          const std::vector<std::uint8_t>& iv,
                                          const std::vector<std::uint8_t>& rndpart) {
    if (sha1.size() != 20 || iv.size() != 16 || rndpart.size() != 12) return {};

    // AES-128-CBC under the fixed key: the first block is XORed with the IV,
    // each following block with the previous ciphertext block.
    std::uint8_t plain[32];
    std::memcpy(plain, sha1.data(), 20);
    std::memcpy(plain + 20, rndpart.data(), 12);

    std::uint8_t chained[16];
    std::memcpy(chained, iv.data(), 16);

    std::vector<std::uint8_t> sig(kSignatureSize);
    sig[0] = 0x01;
    sig[1] = 0x00;
    std::memcpy(sig.data() + 2, rndpart.data(), 12);
    for (int block = 0; block < 2; ++block) {
        std::uint8_t x[16], out[16];
        for (int i = 0; i < 16; ++i) x[i] = std::uint8_t(plain[block * 16 + i] ^ chained[i]);
        blockEncrypt(kKey, x, out);
        std::memcpy(sig.data() + 14 + block * 16, out, 16);
        std::memcpy(chained, out, 16);
    }
    return sig;
}

std::optional<Hash72Params> hash72Extract(const std::vector<std::uint8_t>& signature,
                                          const std::vector<std::uint8_t>& sha1) {
    if (signature.size() < kSignatureSize || signature[0] != 0x01 ||
        signature[1] != 0x00 || sha1.size() != 20)
        return std::nullopt;

    // The first ciphertext block is E(SHA1[0..16) XOR IV); decrypting it
    // yields SHA1[0..16) XOR IV, and the IV is recovered by XORing the known
    // SHA1 back in. The random bytes ride along in the signature itself.
    Hash72Params params;
    params.rndpart.assign(signature.begin() + 2, signature.begin() + 14);

    std::uint8_t dec[16];
    blockDecrypt(kKey, signature.data() + 14, dec);
    params.iv.resize(16);
    for (int i = 0; i < 16; ++i)
        params.iv[i] = std::uint8_t(sha1[i] ^ dec[i]);

    // Regenerating the whole signature is a real check, not a tautology: the
    // extraction above only pins down the first block, while the second block
    // must also match for the signature to be consistent with this SHA1.
    if (hash72Signature(sha1, params.iv, params.rndpart) != signature)
        return std::nullopt;
    return params;
}

std::vector<std::uint8_t> storedHash72(const std::vector<std::uint8_t>& db) {
    if (db.size() < kSignatureOffset + kSignatureSize) return {};
    return {db.begin() + kSignatureOffset,
            db.begin() + kSignatureOffset + kSignatureSize};
}

bool writeHash72(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& iv,
                 const std::vector<std::uint8_t>& rndpart) {
    if (db.size() < kSignatureOffset + kSignatureSize || !isMhbd(db) ||
        iv.size() != 16 || rndpart.size() != 12)
        return false;
    std::vector<std::uint8_t> dbCopy = db;
    const std::vector<std::uint8_t> digest = hash72Sha1(dbCopy);
    if (digest.size() != 20) return false;
    const std::vector<std::uint8_t> sig = hash72Signature(digest, iv, rndpart);
    if (sig.size() != kSignatureSize) return false;
    std::memcpy(db.data() + kSignatureOffset, sig.data(), kSignatureSize);
    return true;
}

std::vector<std::uint8_t> hash72Uuid(const std::vector<std::uint8_t>& fwguid) {
    if (fwguid.size() != 8) return {};
    std::vector<std::uint8_t> uuid(20, 0);
    std::memcpy(uuid.data(), fwguid.data(), 8);
    return uuid;
}

std::vector<std::uint8_t> hash72Iv(const std::vector<std::uint8_t>& fwguid) {
    if (fwguid.size() != 8) return {};
    std::vector<std::uint8_t> iv(16, 0);
    std::memcpy(iv.data(), fwguid.data(), 8);
    return iv;
}

std::optional<HashInfo> readHashInfo(const std::filesystem::path& path,
                                     const std::vector<std::uint8_t>& fwguid) {
    std::vector<std::uint8_t> raw;
    if (!readAll(path, raw) || raw.size() != 54 ||
        std::memcmp(raw.data(), "HASHv0", 6) != 0)
        return std::nullopt;
    // A HashInfo that belongs to a different device signs databases this iPod
    // rejects. Unlike libgpod we do not delete it — the user might want it —
    // but we refuse to use it, and the caller will overwrite it only after
    // verifying against this device's own database.
    if (hash72Uuid(fwguid).empty() ||
        std::memcmp(raw.data() + 6, hash72Uuid(fwguid).data(), 20) != 0)
        return std::nullopt;
    HashInfo info;
    info.uuid.assign(raw.begin() + 6, raw.begin() + 26);
    info.rndpart.assign(raw.begin() + 26, raw.begin() + 38);
    info.iv.assign(raw.begin() + 38, raw.begin() + 54);
    return info;
}

bool writeHashInfo(const std::filesystem::path& path, const HashInfo& info) {
    if (info.uuid.size() != 20 || info.rndpart.size() != 12 ||
        info.iv.size() != 16)
        return false;
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const char magic[] = "HASHv0";
    out.write(magic, 6);
    out.write(reinterpret_cast<const char*>(info.uuid.data()),
              std::streamsize(info.uuid.size()));
    out.write(reinterpret_cast<const char*>(info.rndpart.data()),
              std::streamsize(info.rndpart.size()));
    out.write(reinterpret_cast<const char*>(info.iv.data()),
              std::streamsize(info.iv.size()));
    return static_cast<bool>(out);
}

}  // namespace podbox
