#include "itdb/hashab.h"

#include "itdb/hash58.h"

extern "C" {
#include "calcHashAB.h"
#include "data/FINAL_PERM.h"
}

#include <algorithm>
#include <array>
#include <cstring>

namespace podbox {
namespace {

constexpr std::size_t kOffset = 0xAB;
constexpr std::size_t kMinSize = kOffset + kHashAbSignatureSize;

struct ZeroRange {
    std::size_t offset;
    std::size_t size;
};

constexpr ZeroRange kZeroed[] = {
    {0x18, 8},   // db_id
    {0x58, 20},  // hash58
    {0x72, 46},  // hash72
    {0xAB, 57},  // hashAB
};

std::uint8_t inverseByte(std::uint8_t value,
                         std::uint8_t (*transform)(std::uint8_t)) {
    for (unsigned int candidate = 0; candidate <= 0xFF; ++candidate)
        if (transform(std::uint8_t(candidate)) == value)
            return std::uint8_t(candidate);
    return 0;
}

std::uint8_t finalTransform(std::uint8_t x) {
    std::uint8_t y = std::uint8_t(93u ^ std::uint8_t(13u * x));
    if (x & 1u) y ^= 0x80u;
    return y;
}

std::uint8_t nonceTransform(std::uint8_t x) {
    return std::uint8_t(69u * x + 118u * (x & 0x5Du) + 17u);
}

std::uint32_t permutation(std::size_t index) {
    const std::size_t off = index * 4;
    return std::uint32_t(FINAL_PERM[off]) |
           (std::uint32_t(FINAL_PERM[off + 1]) << 8) |
           (std::uint32_t(FINAL_PERM[off + 2]) << 16) |
           (std::uint32_t(FINAL_PERM[off + 3]) << 24);
}

}  // namespace

std::vector<std::uint8_t> hashAbSha1(std::vector<std::uint8_t>& db) {
    if (db.size() < kMinSize || std::memcmp(db.data(), "mhbd", 4) != 0)
        return {};

    std::array<std::vector<std::uint8_t>, std::size(kZeroed)> saved;
    for (std::size_t i = 0; i < std::size(kZeroed); ++i) {
        const ZeroRange range = kZeroed[i];
        saved[i].assign(db.begin() + range.offset,
                        db.begin() + range.offset + range.size);
        std::fill_n(db.begin() + range.offset, range.size, 0);
    }
    const std::vector<std::uint8_t> digest = sha1(db.data(), db.size());
    for (std::size_t i = 0; i < std::size(kZeroed); ++i)
        std::copy(saved[i].begin(), saved[i].end(),
                  db.begin() + kZeroed[i].offset);
    return digest;
}

std::vector<std::uint8_t> hashAbSignature(
    const std::vector<std::uint8_t>& digest,
    const std::vector<std::uint8_t>& uuid,
    const std::vector<std::uint8_t>& nonce) {
    if (digest.size() != 20 || uuid.size() != 8 || nonce.size() != 23)
        return {};
    std::vector<std::uint8_t> out(kHashAbSignatureSize);
    // The upstream C declaration predates const-correct input parameters.
    calcHashAB(out.data(), const_cast<std::uint8_t*>(digest.data()),
               const_cast<std::uint8_t*>(uuid.data()), nonce.data());
    return out;
}

std::optional<std::vector<std::uint8_t>> hashAbExtractNonce(
    const std::vector<std::uint8_t>& signature,
    const std::vector<std::uint8_t>& digest,
    const std::vector<std::uint8_t>& uuid) {
    if (signature.size() != kHashAbSignatureSize || digest.size() != 20 ||
        uuid.size() != 8 || signature[0] != 3 || signature[1] != 0)
        return std::nullopt;

    std::array<std::uint8_t, kHashAbSignatureSize> work{};
    std::copy(signature.begin(), signature.end(), work.begin());
    for (std::size_t i = 2; i < work.size(); ++i)
        work[i] = inverseByte(work[i], finalTransform);

    // calcHashAB swaps in ascending order, so undo the swaps in reverse.
    for (int i = 54; i >= 0; --i)
        std::swap(work[std::size_t(i) + 2],
                  work[std::size_t(permutation(i)) + 2]);

    std::vector<std::uint8_t> nonce(23);
    for (std::size_t i = 0; i < nonce.size(); ++i)
        nonce[i] = inverseByte(work[i + 2], nonceTransform);
    if (hashAbSignature(digest, uuid, nonce) != signature)
        return std::nullopt;
    return nonce;
}

std::vector<std::uint8_t> hashAbDefaultNonce() {
    static constexpr char kNonce[] = "ABCDEFGHIJKLMNOPQRSTUVW";
    return {kNonce, kNonce + 23};
}

bool writeHashAb(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& uuid,
                 const std::vector<std::uint8_t>& nonce) {
    if (uuid.size() != 8 || nonce.size() != 23) return false;
    const std::vector<std::uint8_t> digest = hashAbSha1(db);
    if (digest.empty()) return false;
    const std::vector<std::uint8_t> signature =
        hashAbSignature(digest, uuid, nonce);
    if (signature.empty()) return false;
    std::copy(signature.begin(), signature.end(), db.begin() + kOffset);
    return true;
}

std::vector<std::uint8_t> storedHashAb(const std::vector<std::uint8_t>& db) {
    if (db.size() < kMinSize) return {};
    return {db.begin() + kOffset,
            db.begin() + kOffset + kHashAbSignatureSize};
}

}  // namespace podbox
