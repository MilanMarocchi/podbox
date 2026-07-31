#pragma once

// hash58 — the checksum the iPod classic (6G/7G) and nano 3G–5G require over
// their iTunesDB. A database written without a correct one is rejected by the
// device, which then shows an empty library.
//
// The algorithm is not published by Apple. It was reverse-engineered by wtbw
// and first implemented by Christophe Fergeau for libgpod, under a 3-clause
// BSD licence; this is an independent implementation of the same algorithm,
// and the attribution is preserved in hash58.cpp.
//
// Verification matters here. Every step below the composition is checkable
// against published test vectors, and `hash58OfDatabase` exists so that the
// composition can be checked against a real device: recompute the hash of a
// database the iPod already accepts and compare it to the one stored inside.
// If those agree, the implementation is right for that device.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace podbox {

// The 8 raw bytes of a FireWire GUID, decoded from the 16-hex-character
// string SysInfoExtended reports. Empty when the string is missing or is not
// 16 hex digits.
std::vector<std::uint8_t> parseFirewireGuid(const std::string& hex);

// SHA-1 of `data`. Exposed so it can be tested against the FIPS 180-1 and
// RFC 3174 vectors.
std::vector<std::uint8_t> sha1(const std::uint8_t* data, std::size_t len);

// HMAC-SHA1, per RFC 2104. Exposed for the RFC 2202 test vectors.
std::vector<std::uint8_t> hmacSha1(const std::uint8_t* key, std::size_t keyLen,
                                   const std::uint8_t* data,
                                   std::size_t dataLen);

// The 64-byte HMAC key hash58 derives from a FireWire GUID.
std::vector<std::uint8_t> hash58Key(const std::vector<std::uint8_t>& fwguid);

// The 20-byte hash58 of a complete iTunesDB image. `db` is modified in place
// while hashing — three header fields must read as zero — and restored before
// returning, so the caller sees it unchanged. Returns empty on bad input.
//
// This does not write the result into the database; see writeHash58.
std::vector<std::uint8_t> hash58OfDatabase(std::vector<std::uint8_t>& db,
                                           const std::vector<std::uint8_t>& fwguid);

// Computes the hash and stores it at offset 0x58, leaving `db` ready to write
// to the device. Returns false when the image is too small, is not an mhbd, or
// the GUID is unusable.
bool writeHash58(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& fwguid);

// Reads the hash currently stored at offset 0x58.
std::vector<std::uint8_t> storedHash58(const std::vector<std::uint8_t>& db);

}  // namespace podbox
