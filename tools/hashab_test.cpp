#include "itdb/hashab.h"
#include "itdb/itunesdb.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::uint8_t> fromHex(const std::string& value) {
    auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
        return unsigned(c - 'A' + 10);
    };
    std::vector<std::uint8_t> out(value.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = std::uint8_t((nibble(value[i * 2]) << 4) |
                              nibble(value[i * 2 + 1]));
    return out;
}

struct Vector {
    const char* sha1;
    const char* uuid;
    const char* signature;
};

// A subset of the upstream repository's 100 published vectors. Its own test
// suite also compares 10,000 random cases against the historical binary.
constexpr Vector kVectors[] = {
    {"915dd7203086aeea302740e97febb7aefa9ca9f1",
     "f832c65917da6785",
     "030056d7474d4843465449bd444266eb516bf4552726b94169d1424c4f433a8208fc5345e34507c3fe4a8f4e8a5052664515aad30e4b195c57"},
    {"6a1e089e993ea643ac8a03732d65d70fb49ab2be",
     "0f78bc1413025880",
     "03005643474d48e7465449104442732251aebf554cbd08415bf5a14c4f43d6ad37e853c84145b182034a724e5b505226158c73bf284bdeb757"},
    {"91e301635017bee75e2c603ddf6513dc8d3230f5",
     "a9a79ad5c49c8e17",
     "030056e0474d4865465449da4442081551880e5512e05341db5db54c4f431cd9e18a538806458afbb74ac94eac505290473f53250f4be0be57"},
    {"b0701b04c0b46dcbc8f488b0e075d54ecfa73277",
     "397a06be88415bd9",
     "03005620474d48af465449914442f6225177fa55330d0a410d2d254c4f437b1d1a3253be5845be83274af94e6c505225768bdbfffd4b0f7057"},
    {"1a98e53fd5dd47a9c84af12d091ecf76c1f270f9",
     "05249e5de1935cc1",
     "03005627474d4883465449bd44421ed1513fac551dd858418e59794c4f4331ce035453cdca45be50244a704efe505286c2209121c64b802d57"},
};

void testVectorsAndExtraction() {
    std::printf("hashAB vectors\n");
    const auto nonce = podbox::hashAbDefaultNonce();
    check(nonce.size() == 23, "default nonce is 23 bytes");
    for (const Vector& vector : kVectors) {
        const auto digest = fromHex(vector.sha1);
        const auto uuid = fromHex(vector.uuid);
        const auto expected = fromHex(vector.signature);
        const auto signature =
            podbox::hashAbSignature(digest, uuid, nonce);
        check(signature == expected, "matches published signature");
        const auto extracted =
            podbox::hashAbExtractNonce(signature, digest, uuid);
        check(extracted && *extracted == nonce,
              "recovers and verifies embedded nonce");
    }

    auto corrupt = fromHex(kVectors[0].signature);
    corrupt[40] ^= 1;
    check(!podbox::hashAbExtractNonce(
               corrupt, fromHex(kVectors[0].sha1), fromHex(kVectors[0].uuid)),
          "rejects a corrupt signature");
    check(podbox::hashAbSignature({}, {}, {}).empty(),
          "rejects wrongly sized inputs");
}

void testDatabaseHashing() {
    std::printf("hashAB database fields\n");
    std::vector<std::uint8_t> db(0x400, 0xA5);
    std::memcpy(db.data(), "mhbd", 4);
    const auto before = db;
    const auto digest = podbox::hashAbSha1(db);
    check(digest.size() == 20, "produces a 20-byte SHA1");
    check(db == before, "restores all temporarily zeroed fields");

    std::vector<std::uint8_t> ignored = db;
    ignored[0x18] ^= 1;
    ignored[0x58] ^= 2;
    ignored[0x72] ^= 4;
    ignored[0xAB] ^= 8;
    check(podbox::hashAbSha1(ignored) == digest,
          "ignores db id and every hash slot");
    ignored[0x300] ^= 1;
    check(podbox::hashAbSha1(ignored) != digest,
          "depends on database content");

    const auto uuid = fromHex("f832c65917da6785");
    const auto nonce = podbox::hashAbDefaultNonce();
    check(podbox::writeHashAb(db, uuid, nonce), "writes signature");
    const auto stored = podbox::storedHashAb(db);
    check(stored.size() == podbox::kHashAbSignatureSize,
          "stores 57 bytes at offset 0xAB");
    const auto params =
        podbox::hashAbExtractNonce(stored, podbox::hashAbSha1(db), uuid);
    check(params && *params == nonce, "written signature verifies");
    check(podbox::writeHashAb(db, uuid, nonce), "second write succeeds");
    check(podbox::storedHashAb(db) == stored, "write is idempotent");

    std::vector<std::uint8_t> tiny(32, 0);
    check(!podbox::writeHashAb(tiny, uuid, nonce), "rejects a tiny image");
    std::vector<std::uint8_t> notDb(0x200, 0);
    check(!podbox::writeHashAb(notDb, uuid, nonce), "rejects a non-mhbd image");
}

void testWriterIntegration() {
    std::printf("hashAB iTunesCDB writer\n");
    podbox::Library library;
    library.version = 0x2A;
    library.databaseDbid = 0x1020304050607080ULL;
    library.masterDbid = 0x0123456789ABCDEFULL;
    library.masterName = "nano";
    podbox::WriteOptions options;
    options.compressed = true;
    options.hashAbUuid = fromHex("f832c65917da6785");
    options.hashAbNonce = podbox::hashAbDefaultNonce();
    const auto path = std::filesystem::temp_directory_path() /
                      "podbox-hashab-writer-test.tmp";
    std::string error;
    check(podbox::writeItunesDb(library, path, &error, options),
          error.empty() ? "writes compressed hashAB database" : error.c_str());
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> image(std::istreambuf_iterator<char>(input), {});
    check(image.size() > 0xE4, "writer emits complete physical image");
    check(image.size() > 0x72 && image[0x30] == podbox::kChecksumHashAB &&
              image[0x31] == 0 && image[0x70] == 4 && image[0x71] == 0,
          "declares scheme 3 and nano companion marker");
    const auto extracted = podbox::hashAbExtractNonce(
        podbox::storedHashAb(image), podbox::hashAbSha1(image),
        options.hashAbUuid);
    check(extracted && *extracted == options.hashAbNonce,
          "writer signature verifies against physical iTunesCDB");
    const auto parsed = podbox::parseItunesDb(path);
    check(parsed.library && parsed.library->compressed &&
              parsed.library->databaseDbid == library.databaseDbid &&
              parsed.library->masterDbid == library.masterDbid,
          "compressed database preserves SQLite shared ids");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    testVectorsAndExtraction();
    testDatabaseHashing();
    testWriterIntegration();
    if (failures) return 1;
    std::printf("hashAB tests passed\n");
    return 0;
}
