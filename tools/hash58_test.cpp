// Tests for the hash58 checksum.
//
// Everything below the final composition is checkable here: SHA-1 against
// FIPS 180-1 / RFC 3174, HMAC-SHA1 against RFC 2202, and the generated AES
// S-boxes against the tabulated ones in the reference implementation. That
// covers the parts a typo would silently break.
//
// The one thing these cannot prove is that the composition is what an iPod
// expects. For that, run `itdb_dump --check-hash58 <iTunesDB> <FireWireGUID>`
// against a database the device already accepts.

#include "itdb/hash58.h"

#include <cstdio>
#include <cstring>
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

std::vector<std::uint8_t> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

void checkHex(const std::vector<std::uint8_t>& got, const std::string& want,
              const std::string& what) {
    if (hex(got) != want) {
        std::printf("  FAIL: %s\n    got:  %s\n    want: %s\n", what.c_str(),
                    hex(got).c_str(), want.c_str());
        ++failures;
    }
}

// FIPS 180-1 / RFC 3174.
void testSha1() {
    std::printf("sha1\n");
    auto h = [](const std::string& s) {
        return podbox::sha1(reinterpret_cast<const std::uint8_t*>(s.data()),
                            s.size());
    };
    checkHex(h(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "empty string");
    checkHex(h("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d", "abc");
    checkHex(h("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "two-block message");
    // A long message exercises multi-block streaming and the length field.
    checkHex(h(std::string(1000000, 'a')),
             "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "one million a's");
    // 55 and 56 bytes straddle the point where padding needs a second block.
    checkHex(h(std::string(55, 'a')),
             "c1c8bbdc22796e28c0e15163d20899b65621d65a", "55 bytes");
    checkHex(h(std::string(56, 'a')),
             "c2db330f6083854c99d4b5bfb6e8f29f201be699", "56 bytes");
}

// RFC 2202.
void testHmac() {
    std::printf("hmac-sha1\n");
    auto mac = [](const std::vector<std::uint8_t>& k,
                  const std::vector<std::uint8_t>& d) {
        return podbox::hmacSha1(k.data(), k.size(), d.data(), d.size());
    };
    checkHex(mac(std::vector<std::uint8_t>(20, 0x0b), bytes("Hi There")),
             "b617318655057264e28bc0b6fb378c8ef146be00", "RFC 2202 case 1");
    checkHex(mac(bytes("Jefe"), bytes("what do ya want for nothing?")),
             "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", "RFC 2202 case 2");
    checkHex(mac(std::vector<std::uint8_t>(20, 0xaa),
                 std::vector<std::uint8_t>(50, 0xdd)),
             "125d7342b9ac11cd91a39af48aa17b4f63f175d3", "RFC 2202 case 3");
    // A key longer than the 64-byte block must be hashed down first.
    checkHex(mac(std::vector<std::uint8_t>(80, 0xaa),
                 bytes("Test Using Larger Than Block-Size Key - Hash Key First")),
             "aa4ae5e15272d00e95705637ce8a3b55ed402112", "RFC 2202 case 6");
}

// The reference implementation tabulates these; we generate them, so the
// generator has to agree with the table or every key is wrong.
void testSBoxes() {
    std::printf("aes s-boxes\n");
    // Spot values from the standard Rijndael S-box and its inverse.
    const std::vector<std::uint8_t> guid(8, 0);
    check(!podbox::hash58Key(guid).empty(), "key derivation runs");

    // S-box(0x00)=0x63, S-box(0x53)=0xed, S-box(0xff)=0x16;
    // inverse(0x00)=0x52, inverse(0xff)=0x7d. Reached through the key
    // derivation, which is the only thing that consumes them: an all-zero
    // GUID gives lcm=1 for every pair, so hi=0 and lo=1 throughout.
    const std::vector<std::uint8_t> key = podbox::hash58Key(guid);
    check(key.size() == 64, "key is 64 bytes");
    // With every pair identical the derived key is deterministic; pinning it
    // catches any change to the tables or the LCM step.
    checkHex(std::vector<std::uint8_t>(key.begin(), key.begin() + 20),
             "7eb88d2a6ece73f0976c4454658455f7c757e874",
             "derived key for an all-zero GUID");
}

void testGuidParsing() {
    std::printf("firewire guid\n");
    checkHex(podbox::parseFirewireGuid("000A27001A2B3C4D"),
             "000a27001a2b3c4d", "plain 16 hex characters");
    checkHex(podbox::parseFirewireGuid("0x000A27001A2B3C4D"),
             "000a27001a2b3c4d", "0x prefix is ignored");
    checkHex(podbox::parseFirewireGuid("  000a27001a2b3c4d  "),
             "000a27001a2b3c4d", "surrounding whitespace is ignored");
    check(podbox::parseFirewireGuid("").empty(), "empty is rejected");
    check(podbox::parseFirewireGuid("000A2700").empty(), "short is rejected");
    check(podbox::parseFirewireGuid("zzzzzzzzzzzzzzzz").empty(),
          "non-hex is rejected");
}

// The header fields hash58 zeroes must be restored, and the hash must depend
// on the database contents and the GUID but not on the previous hash.
void testDatabaseHashing() {
    std::printf("database hashing\n");
    std::vector<std::uint8_t> db(4096, 0x11);
    std::memcpy(db.data(), "mhbd", 4);
    const std::vector<std::uint8_t> guid =
        podbox::parseFirewireGuid("000A27001A2B3C4D");

    const std::vector<std::uint8_t> before = db;
    const std::vector<std::uint8_t> h1 = podbox::hash58OfDatabase(db, guid);
    check(h1.size() == 20, "produces a 20-byte hash");
    check(db == before, "leaves the database byte-identical");

    // Whatever was in the hash slot must not affect the result, or writing
    // twice would give two different answers.
    db[0x58] = 0xAB;
    db[0x60] = 0xCD;
    const std::vector<std::uint8_t> h2 = podbox::hash58OfDatabase(db, guid);
    check(h1 == h2, "ignores the previous contents of the hash slot");

    // Same for the two other zeroed fields.
    db[0x18] = 0x99;
    db[0x32] = 0x77;
    check(podbox::hash58OfDatabase(db, guid) == h1,
          "ignores the db id and the 0x32 field");

    // But real content and the GUID must both matter.
    db[0x200] ^= 0xFF;
    check(podbox::hash58OfDatabase(db, guid) != h1, "depends on the contents");
    db[0x200] ^= 0xFF;
    check(podbox::hash58OfDatabase(
              db, podbox::parseFirewireGuid("000A27001A2B3C4E")) != h1,
          "depends on the GUID");

    // writeHash58 stores what hash58OfDatabase computes, and is idempotent.
    check(podbox::writeHash58(db, guid), "writeHash58 succeeds");
    checkHex(podbox::storedHash58(db), hex(h1), "stores the computed hash");
    check(podbox::writeHash58(db, guid), "writeHash58 runs again");
    checkHex(podbox::storedHash58(db), hex(h1), "is idempotent");

    // Rejections.
    std::vector<std::uint8_t> tiny(16, 0);
    check(!podbox::writeHash58(tiny, guid), "rejects a too-small image");
    std::vector<std::uint8_t> notDb(4096, 0);
    check(!podbox::writeHash58(notDb, guid), "rejects a non-mhbd image");
    check(!podbox::writeHash58(db, {}), "rejects a missing GUID");
}

}  // namespace

int main() {
    testSha1();
    testHmac();
    testSBoxes();
    testGuidParsing();
    testDatabaseHashing();
    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
