// Tests for the hash72 checksum and the iTunesCDB container.
//
// Everything below the final composition is checkable here: AES-128 against
// FIPS-197, SHA-1 against FIPS 180-1 / RFC 3174 (via hash58_test), the
// signature build/extract round trip, and the HashInfo file. The iTunesCDB
// zlib round trip is tested too: a compressed image must inflate back to
// exactly the image that was deflated.
//
// The one thing these cannot prove is that the composition is what an iPod
// expects. For that, run `itdb_dump --check-hash72 <iTunesCDB> <FireWireGUID>`
// against a database the device already accepts.

#include "itdb/hash72.h"
#include "itdb/itunescdb.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string hex(const std::vector<std::uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

std::vector<std::uint8_t> fromHex(const std::string& s) {
    auto nib = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return std::uint8_t(c - '0');
        if (c >= 'a' && c <= 'f') return std::uint8_t(c - 'a' + 10);
        return std::uint8_t(c - 'A' + 10);
    };
    std::vector<std::uint8_t> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = std::uint8_t((nib(s[i * 2]) << 4) | nib(s[i * 2 + 1]));
    return out;
}

void checkHex(const std::vector<std::uint8_t>& got, const std::string& want,
              const std::string& what) {
    if (hex(got) != want) {
        std::printf("  FAIL: %s\n    got:  %s\n    want: %s\n", what.c_str(),
                    hex(got).c_str(), want.c_str());
        ++failures;
    }
}

// FIPS 197 Appendix C.1/C.3, and NIST SP 800-38A ECB-AES128.
void testAes() {
    std::printf("aes-128\n");
    const std::vector<std::uint8_t> key =
        fromHex("000102030405060708090a0b0c0d0e0f");
    const std::vector<std::uint8_t> plain =
        fromHex("00112233445566778899aabbccddeeff");
    checkHex(podbox::aes128EcbEncrypt(key, plain),
             "69c4e0d86a7b0430d8cdb78070b4c55a", "FIPS 197 C.1/C.3 encrypt");
    checkHex(podbox::aes128EcbDecrypt(
                 key, fromHex("69c4e0d86a7b0430d8cdb78070b4c55a")),
             hex(plain), "FIPS 197 decrypt");

    const std::vector<std::uint8_t> key2 =
        fromHex("2b7e151628aed2a6abf7158809cf4f3c");
    checkHex(podbox::aes128EcbEncrypt(
                 key2, fromHex("6bc1bee22e409f96e93d7e117393172a")),
             "3ad77bb40d7a3660a89ecaf32466ef97", "SP 800-38A encrypt");

    // Two blocks round trip through encrypt+decrypt.
    const std::vector<std::uint8_t> two = plain;
    checkHex(podbox::aes128EcbDecrypt(key, podbox::aes128EcbEncrypt(key, two)),
             hex(two), "two-block round trip");

    check(podbox::aes128EcbEncrypt(key, {1, 2, 3}).empty(),
          "rejects a non-block-aligned input");
    check(podbox::aes128EcbEncrypt({1}, plain).empty(), "rejects a bad key");
}

void testSignature() {
    std::printf("signature build and extract\n");
    const std::vector<std::uint8_t> sha1 =
        fromHex("a9993e364706816aba3e25717850c26c9cd0d89d");
    const std::vector<std::uint8_t> iv = fromHex("000102030405060708090a0b0c0d0e0f");
    const std::vector<std::uint8_t> rnd = fromHex("aabbccddeeff001122334455");

    const std::vector<std::uint8_t> sig =
        podbox::hash72Signature(sha1, iv, rnd);
    check(sig.size() == 46, "signature is 46 bytes");
    check(sig[0] == 0x01 && sig[1] == 0x00, "signature starts with 0x01 0x00");
    checkHex(std::vector<std::uint8_t>(sig.begin() + 2, sig.begin() + 14),
             hex(rnd), "random bytes ride along at offset 2");
    check(sig[14] != 0, "ciphertext is not all zeros");

    // Extraction must recover exactly what was signed with.
    const auto params = podbox::hash72Extract(sig, sha1);
    check(params.has_value(), "extraction succeeds");
    checkHex(params->iv, hex(iv), "recovers the IV");
    checkHex(params->rndpart, hex(rnd), "recovers the random bytes");

    // Same signature cannot belong to different content.
    check(!podbox::hash72Extract(sig, fromHex("00112233445566778899aabbccddeeffaabbccdd"))
              .has_value(),
          "rejects a signature for different content");

    // Wrong marker: anything without the 0x01 0x00 prefix is not hash72.
    std::vector<std::uint8_t> bad = sig;
    bad[0] = 0x02;
    check(!podbox::hash72Extract(bad, sha1).has_value(),
          "rejects a wrong marker");
    check(!podbox::hash72Extract({1, 2, 3}, sha1).has_value(),
          "rejects a short signature");
}

std::vector<std::uint8_t> makeDb() {
    // A plausible mhbd: zeroed like the real writer does, header to 0xF4
    // plus a little payload.
    std::vector<std::uint8_t> db(0x300, 0);
    std::memcpy(db.data(), "mhbd", 4);
    db[4] = 0xF4;  // header length, so the iTunesCDB helpers find a payload
    return db;
}

void testDatabaseHashing() {
    std::printf("database hashing\n");
    std::vector<std::uint8_t> db = makeDb();
    const std::vector<std::uint8_t> iv(16, 0x42);
    const std::vector<std::uint8_t> rnd(12, 0x24);

    const std::vector<std::uint8_t> before = db;
    const std::vector<std::uint8_t> h1 = podbox::hash72Sha1(db);
    check(h1.size() == 20, "produces a 20-byte SHA1");
    check(db == before, "leaves the database byte-identical");

    // The four zeroed fields must not affect the result.
    std::vector<std::uint8_t> tampered = db;
    tampered[0x18] = 0x99;
    tampered[0x32] = 0x77;
    tampered[0x58] = 0xAB;
    tampered[0x72] = 0xCD;
    const std::vector<std::uint8_t> tamperedState = tampered;
    check(podbox::hash72Sha1(tampered) == h1,
          "ignores db id, 0x32, hash58 and hash72 slots");
    check(tampered == tamperedState, "restores the caller's bytes");

    // But real content must matter.
    tampered[0x200] ^= 0xFF;
    check(podbox::hash72Sha1(tampered) != h1, "depends on the contents");
    tampered[0x200] ^= 0xFF;
    db = tampered;

    // writeHash72 stores the signature and is idempotent; the stored value
    // extracts back to the (IV, random) the database was signed with.
    check(podbox::writeHash72(db, iv, rnd), "writeHash72 succeeds");
    const std::vector<std::uint8_t> sig = podbox::storedHash72(db);
    check(sig.size() == 46, "stores a 46-byte signature");
    const std::vector<std::uint8_t> sha1 = podbox::hash72Sha1(db);
    const auto params = podbox::hash72Extract(sig, sha1);
    check(params.has_value(), "extracts from a written database");
    checkHex(params->iv, hex(iv), "extracted IV matches");
    checkHex(params->rndpart, hex(rnd), "extracted random matches");
    check(podbox::writeHash72(db, iv, rnd), "writeHash72 runs again");
    check(podbox::storedHash72(db) == sig, "is idempotent");

    // Rejections.
    std::vector<std::uint8_t> tiny(16, 0);
    check(!podbox::writeHash72(tiny, iv, rnd), "rejects a too-small image");
    std::vector<std::uint8_t> notDb(0x100, 0);
    check(!podbox::writeHash72(notDb, iv, rnd), "rejects a non-mhbd image");
    check(!podbox::writeHash72(db, {}, rnd), "rejects a missing IV");
    check(!podbox::writeHash72(db, iv, {}), "rejects missing random bytes");
}

void testGuidDerivation() {
    std::printf("guid derivation\n");
    checkHex(podbox::hash72Uuid(fromHex("000a27001af8597d")),
             "000a27001af8597d000000000000000000000000",
             "UUID is the GUID zero-padded to 20 bytes");
    checkHex(podbox::hash72Iv(fromHex("000a27001af8597d")),
             "000a27001af8597d0000000000000000",
             "IV is the GUID with zero padding");
    check(podbox::hash72Uuid({1, 2, 3}).empty(), "rejects a short GUID");
}

void testHashInfo() {
    std::printf("hashinfo file\n");
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / "podbox-hashinfo-test.tmp";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const std::vector<std::uint8_t> guid = fromHex("000a27001af8597d");
    podbox::HashInfo info;
    info.uuid = podbox::hash72Uuid(guid);
    info.rndpart = fromHex("aabbccddeeff001122334455");
    info.iv = fromHex("000102030405060708090a0b0c0d0e0f");

    check(!podbox::readHashInfo(path, guid).has_value(),
          "missing file reads as nothing");
    check(podbox::writeHashInfo(path, info), "writeHashInfo succeeds");
    const auto read = podbox::readHashInfo(path, guid);
    check(read.has_value(), "reads back");
    checkHex(read->uuid, hex(info.uuid), "uuid round trips");
    checkHex(read->rndpart, hex(info.rndpart), "rndpart round trips");
    checkHex(read->iv, hex(info.iv), "iv round trips");

    // A file whose UUID belongs to a different device is ignored.
    check(!podbox::readHashInfo(path, fromHex("deadbeef00000000")).has_value(),
          "a different device's HashInfo is refused");

    // Corrupt content is refused.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "NOPE!!";
    }
    check(!podbox::readHashInfo(path, guid).has_value(),
          "bad magic is refused");
    std::filesystem::remove(path, ec);
}

void testItunesCdb() {
    std::printf("itunescdb\n");
    std::vector<std::uint8_t> image = makeDb();
    // Enough payload that compression is exercised meaningfully, and a
    // total_length field that matches — as the writer always emits.
    image.resize(0xF4 + 4096);
    image[8] = std::uint8_t(image.size());
    image[9] = std::uint8_t(image.size() >> 8);
    image[10] = std::uint8_t(image.size() >> 16);
    image[11] = std::uint8_t(image.size() >> 24);
    for (std::size_t i = 0xF4; i < image.size(); ++i)
        image[i] = std::uint8_t((i * 31) & 0xFF);

    check(!podbox::isCompressedCdb(image), "plain image is not compressed");
    const std::vector<std::uint8_t> cdb = podbox::compressItunesCdb(image);
    check(!cdb.empty(), "compression succeeds");
    check(cdb.size() < image.size(), "compression shrinks it");
    check(podbox::isCompressedCdb(cdb), "compressed image is flagged");
    const auto back = podbox::decompressItunesCdb(cdb);
    check(back.has_value(), "decompression succeeds");
    check(*back == image, "round trips to the original image");

    // The flag must clear the reader's suspicion of a plain image.
    check(!podbox::decompressItunesCdb(image).has_value(),
          "a plain image is not decompressed");

    // A truncated or garbage stream is refused, not half-parsed.
    std::vector<std::uint8_t> cut = cdb;
    cut.resize(cut.size() / 2);
    check(!podbox::decompressItunesCdb(cut).has_value(),
          "a truncated stream is refused");
    std::vector<std::uint8_t> garbage = cdb;
    for (std::size_t i = 0xF4; i < garbage.size(); ++i) garbage[i] ^= 0xFF;
    check(!podbox::decompressItunesCdb(garbage).has_value(),
          "a corrupt stream is refused");

    check(podbox::compressItunesCdb({1, 2, 3}).empty(),
          "compressing a non-mhbd image fails");
}

}  // namespace

int main() {
    testAes();
    testSignature();
    testDatabaseHashing();
    testGuidDerivation();
    testHashInfo();
    testItunesCdb();
    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
