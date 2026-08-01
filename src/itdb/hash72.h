#pragma once

// hash72 — the checksum the iPod nano 5G requires over its iTunesCDB. The
// database on those devices is stored compressed (the iTunesCDB container),
// and the checksum covers the compressed bytes as they sit on disk. A
// database written without a correct one is rejected by the device, which
// then shows an empty library.
//
// The algorithm is not published by Apple. It was reverse-engineered by Chris
// Lee and first implemented for libgpod's itdb_hash72.c, which is licensed
// under the LGPL 2.1; this is an independent implementation of the same
// algorithm, and the attribution is preserved in hash72.cpp.
//
// hash72 is the reason the nano 5G carries a HashInfo file
// (iPod_Control/Device/HashInfo): the device verifies a database's signature
// with the per-device (uuid, random, IV) it holds, so a writer must sign with
// the same values. The values are recovered from a database the device
// already accepts — its signature decrypts to the database's own SHA1, which
// reveals the IV — rather than guessed.
//
// Verification matters here. Every step below the composition is checkable
// against published test vectors (FIPS 197 for AES, FIPS 180-1 for SHA1), and
// `hash72Extract` exists so the composition can be checked against a real
// device: extract (IV, random) from a database the iPod already accepts,
// regenerate its signature, and compare it to the one stored inside.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace podbox {

// AES-128 ECB, exposed so the FIPS-197 vectors can pin it down. `data` must
// be a whole number of 16-byte blocks.
std::vector<std::uint8_t> aes128EcbEncrypt(const std::vector<std::uint8_t>& key,
                                           const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> aes128EcbDecrypt(const std::vector<std::uint8_t>& key,
                                           const std::vector<std::uint8_t>& data);

// SHA-1 of a complete database image with the fields hash72 requires zeroed:
// db_id (0x18), unk_0x32 (0x32), the hash58 slot (0x58) and the hash72 slot
// itself (0x72). `db` is restored before returning. Returns empty on bad
// input. This does not write the signature; see writeHash72.
std::vector<std::uint8_t> hash72Sha1(std::vector<std::uint8_t>& db);

// The 46-byte signature: 0x01 0x00, the 12 random bytes, then
// AES-128-CBC(sha1(20) | random(12)) under the fixed key with `iv`.
std::vector<std::uint8_t> hash72Signature(const std::vector<std::uint8_t>& sha1,
                                          const std::vector<std::uint8_t>& iv,
                                          const std::vector<std::uint8_t>& rndpart);

// The (IV, random) pair hidden inside a signature. A genuine signature always
// decrypts back to the database's own SHA1, so the IV it was signed with is
// recovered as sha1[0..16) XOR decrypt(signature block 1). Returns nothing
// when the signature is not hash72-shaped (wrong prefix) or `sha1` is not 20
// bytes.
struct Hash72Params {
    std::vector<std::uint8_t> iv;      // 16 bytes
    std::vector<std::uint8_t> rndpart; // 12 bytes
};
std::optional<Hash72Params> hash72Extract(const std::vector<std::uint8_t>& signature,
                                          const std::vector<std::uint8_t>& sha1);

// Computes the signature and stores it at offset 0x72, leaving `db` ready to
// write to the device. The hash covers the signature slot itself, so it is
// zeroed and restored internally — mirroring how the firmware computes it.
// Returns false when the image is too small, is not an mhbd, or the inputs
// are not 16/12 bytes.
bool writeHash72(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& iv,
                 const std::vector<std::uint8_t>& rndpart);

// Reads the signature currently stored at offset 0x72.
std::vector<std::uint8_t> storedHash72(const std::vector<std::uint8_t>& db);

// The 20-byte device UUID and the 16-byte IV that follow from a FireWire
// GUID: the UUID is the GUID zero-padded to 20 bytes, and the IV is the GUID
// with eight zero bytes appended. Empty when the GUID is not 8 bytes.
std::vector<std::uint8_t> hash72Uuid(const std::vector<std::uint8_t>& fwguid);
std::vector<std::uint8_t> hash72Iv(const std::vector<std::uint8_t>& fwguid);

// The HashInfo file (iPod_Control/Device/HashInfo) a nano 5G's iTunes keeps
// beside a hash72-signed database: "HASHv0", the device UUID (20 bytes), the
// 12 random bytes and the 16-byte IV. readHashInfo validates the length and
// magic, and returns nothing when the UUID does not match the given GUID —
// a HashInfo that is not this device's is worse than none, because signing
// with its values produces a database this iPod rejects.
struct HashInfo {
    std::vector<std::uint8_t> uuid;    // 20 bytes
    std::vector<std::uint8_t> rndpart; // 12 bytes
    std::vector<std::uint8_t> iv;      // 16 bytes
};
std::optional<HashInfo> readHashInfo(const std::filesystem::path& path,
                                     const std::vector<std::uint8_t>& fwguid);
bool writeHashInfo(const std::filesystem::path& path, const HashInfo& info);

}  // namespace podbox
