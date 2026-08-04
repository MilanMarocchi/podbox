#pragma once

// HashAB is the 57-byte iTunesCDB signature used by iPod nano 6G/7G. The
// white-box transform is supplied by dstaley/hashab (The Unlicense), pinned
// in CMake to the revision PodBox tests. This wrapper owns the database field
// handling and the reversible nonce envelope around that transform.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace podbox {

inline constexpr std::size_t kHashAbSignatureSize = 57;

// SHA-1 of the physical (compressed) iTunesCDB with the four signature/id
// ranges hashAB excludes zeroed. The caller's bytes are restored.
std::vector<std::uint8_t> hashAbSha1(std::vector<std::uint8_t>& db);

// Calculates the 57-byte signature from SHA1 (20), device GUID/UUID (8), and
// nonce (23). Empty is returned for a wrongly sized input.
std::vector<std::uint8_t> hashAbSignature(
    const std::vector<std::uint8_t>& digest,
    const std::vector<std::uint8_t>& uuid,
    const std::vector<std::uint8_t>& nonce);

// Recovers the nonce embedded in a signature, then recalculates the complete
// signature to reject corrupt or foreign data. This lets PodBox retain the
// exact nonce from a database the iPod already accepts.
std::optional<std::vector<std::uint8_t>> hashAbExtractNonce(
    const std::vector<std::uint8_t>& signature,
    const std::vector<std::uint8_t>& digest,
    const std::vector<std::uint8_t>& uuid);

// The deterministic nonce used by the upstream implementation and vectors.
// Existing device databases should use hashAbExtractNonce instead.
std::vector<std::uint8_t> hashAbDefaultNonce();

bool writeHashAb(std::vector<std::uint8_t>& db,
                 const std::vector<std::uint8_t>& uuid,
                 const std::vector<std::uint8_t>& nonce);
std::vector<std::uint8_t> storedHashAb(const std::vector<std::uint8_t>& db);

}  // namespace podbox
