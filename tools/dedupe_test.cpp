// Checks the duplicate-detection core (src/library/dedupe.cpp) and the audio
// fingerprint (src/library/fingerprint.cpp).
//
//   dedupe_test                 synthetic assertions only; needs nothing
//   dedupe_test <music-dir>     also scans a real folder and reports its
//                               duplicate groups
//
// The scan is read-only. Nothing here writes to a device.

#include "library/dedupe.h"
#include "library/fingerprint.h"
#include "library/fingerprint_store.h"
#include "library/artwork.h"
#include "library/metadata.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace podbox;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void checkEq(long long got, long long want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL  %s: got %lld, want %lld\n", what.c_str(), got,
                    want);
        ++failures;
    }
}

Track mk(std::uint32_t id, const std::string& artist, const std::string& title,
         const std::string& album, std::uint32_t lengthMs,
         const std::string& ext = "mp3") {
    Track t;
    t.id = id;
    t.dbid = 1000 + id;
    t.artist = artist;
    t.title = title;
    t.album = album;
    t.lengthMs = lengthMs;
    t.location = ":iPod_Control:Music:F00:AAAA." + ext;
    t.sizeBytes = 5 * 1024 * 1024;
    return t;
}

void testKeys() {
    std::printf("duplicate keys\n");

    const Track a = mk(1, "Coldplay", "Yellow", "Parachutes", 269000);
    const Track b = mk(2, "COLDPLAY", "yellow", "parachutes", 270000);
    const Track c = mk(3, "Coldplay", "Yellow", "Best Of", 269000);

    check(duplicateKey(a, MatchMode::Exact) == duplicateKey(b, MatchMode::Exact),
          "case differences and a 1s drift still match under Exact");
    check(duplicateKey(a, MatchMode::Exact) != duplicateKey(c, MatchMode::Exact),
          "a different album separates them under Exact");
    check(duplicateKey(a, MatchMode::Loose) == duplicateKey(c, MatchMode::Loose),
          "Loose ignores the album");

    // Duration rounds to the nearest 2s, so a 4s gap must not match.
    const Track d = mk(4, "Coldplay", "Yellow", "Parachutes", 273000);
    check(duplicateKey(a, MatchMode::Exact) != duplicateKey(d, MatchMode::Exact),
          "a 4s difference does not match under Exact");

    // The guard that stops untagged rips collapsing into one another.
    Track blank = mk(5, "", "", "", 100000);
    check(!hasIdentity(blank), "a track with no artist and no title has no identity");
    check(duplicateKey(blank, MatchMode::Loose).empty(),
          "a track with no identity yields an empty key");
    Track titleOnly = mk(6, "", "Some Song", "", 100000);
    check(hasIdentity(titleOnly), "a title alone is enough identity");
}

void testKeeperRanking() {
    std::printf("keeper ranking\n");

    Track flac = mk(1, "A", "S", "Al", 200000, "flac");
    flac.bitrate = 900;
    Track mp3 = mk(2, "A", "S", "Al", 200000, "mp3");
    mp3.bitrate = 320;
    check(betterCopy(flac, mp3), "lossless beats lossy");
    check(!betterCopy(mp3, flac), "and the comparison is antisymmetric");

    // The rule must hold even when the lossy copy has more plays, because the
    // point of the rule is that quality is unrecoverable and plays are not.
    Track playedMp3 = mp3;
    playedMp3.playCount = 50;
    check(betterCopy(flac, playedMp3), "lossless still wins against a played lossy copy");

    // ALAC and AAC share .m4a on an iPod; bitrate is what separates them.
    Track alac = mk(3, "A", "S", "Al", 200000, "m4a");
    alac.bitrate = 850;
    Track aac = mk(4, "A", "S", "Al", 200000, "m4a");
    aac.bitrate = 256;
    check(isLossless(alac), "a high-bitrate .m4a reads as ALAC");
    check(!isLossless(aac), "a low-bitrate .m4a reads as AAC");
    check(betterCopy(alac, aac), "ALAC beats AAC in the same container");

    // Within one format, listening history decides.
    Track played = mk(5, "A", "S", "Al", 200000, "mp3");
    played.bitrate = 320;
    played.playCount = 10;
    Track fresh = mk(6, "A", "S", "Al", 200000, "mp3");
    fresh.bitrate = 320;
    check(betterCopy(played, fresh), "more plays wins among equals");
}

void testGrouping() {
    std::printf("grouping\n");

    Library lib;
    lib.tracks.push_back(mk(1, "A", "Song", "Album", 200000, "mp3"));
    lib.tracks.back().bitrate = 320;
    lib.tracks.push_back(mk(2, "A", "Song", "Album", 200000, "flac"));
    lib.tracks.back().bitrate = 900;
    lib.tracks.push_back(mk(3, "B", "Other", "Album", 100000));
    // Two untagged rips: distinct songs, no identity, must not group.
    lib.tracks.push_back(mk(4, "", "", "", 100000));
    lib.tracks.push_back(mk(5, "", "", "", 100000));

    const auto groups = findDuplicates(lib, MatchMode::Exact, {});
    checkEq((long long)groups.size(), 1, "one duplicate group found");
    if (!groups.empty()) {
        checkEq((long long)groups[0].trackIds.size(), 2, "the group holds both copies");
        checkEq(groups[0].trackIds[0], 2, "the FLAC is the keeper");
        check(!groups[0].allIdenticalFiles,
              "without fingerprints the group is not byte-identical");
    }

    // A group must report what removing its non-keepers frees.
    if (!groups.empty())
        checkEq((long long)groups[0].reclaimBytes, 5 * 1024 * 1024,
                "reclaimable bytes counts only the non-keepers");

    // Fingerprints agreeing promotes the group to byte-identical.
    FingerprintMap fps;
    fps[lib.tracks[0].dbid] = AudioFingerprint{1234, 5678};
    fps[lib.tracks[1].dbid] = AudioFingerprint{1234, 5678};
    const auto identical = findDuplicates(lib, MatchMode::Exact, fps);
    check(!identical.empty() && identical[0].allIdenticalFiles,
          "matching fingerprints mark the group byte-identical");

    fps[lib.tracks[1].dbid] = AudioFingerprint{1234, 9999};
    const auto differing = findDuplicates(lib, MatchMode::Exact, fps);
    check(!differing.empty() && !differing[0].allIdenticalFiles,
          "differing fingerprints do not");
}

void testBucketBoundary() {
    std::printf("duration bucket boundary\n");

    // Two rips of one song 60 ms apart, straddling a 2-second bucket edge.
    // Grouping is allowed to miss this (it just yields two groups), but a
    // membership test must not: sync reads "no match" as "not on the Mac"
    // and deletes accordingly. This pair really did get 8 songs marked for
    // deletion off a real iPod.
    const Track a = mk(1, "Polyphia", "Nightmare", "Renaissance", 246960);
    const Track b = mk(2, "Polyphia", "Nightmare", "Renaissance", 247013);
    check(duplicateKey(a, MatchMode::Exact) != duplicateKey(b, MatchMode::Exact),
          "the pair does straddle a bucket boundary");

    std::unordered_set<std::string> keys{duplicateKey(b, MatchMode::Exact)};
    check(matchesAny(a, keys, MatchMode::Exact),
          "but a lookup still finds it across the boundary");

    // A genuinely different song must still not match.
    const Track other = mk(3, "Polyphia", "Nightmare", "Renaissance", 300000);
    check(!matchesAny(other, keys, MatchMode::Exact),
          "a song a minute longer does not match");
    check(matchesAny(a, {duplicateKey(b, MatchMode::Loose)}, MatchMode::Loose),
          "loose lookups work too");
}

void testPlaylistRemoval() {
    std::printf("playlist remap\n");

    // The case dedupe must never get wrong: a playlist referencing the copy
    // that is about to be deleted has to end up referencing the keeper, not
    // lose the song.
    {
        std::vector<std::uint32_t> pl = {10, 20, 30};
        KeeperRemap remap{{20, 21}};
        applyRemovalToPlaylist({20}, &remap, &pl);
        checkEq((long long)pl.size(), 3, "remapped playlist keeps its length");
        checkEq(pl[1], 21, "the removed copy became the keeper");
    }
    // Without a keeper the entry simply goes.
    {
        std::vector<std::uint32_t> pl = {10, 20, 30};
        applyRemovalToPlaylist({20}, nullptr, &pl);
        checkEq((long long)pl.size(), 2, "an unmapped removal drops the entry");
        checkEq(pl[1], 30, "and the rest keep their order");
    }
    // Playlist held both copies: after remapping they collide, and only then
    // is one dropped.
    {
        std::vector<std::uint32_t> pl = {21, 20};
        KeeperRemap remap{{20, 21}};
        applyRemovalToPlaylist({20}, &remap, &pl);
        checkEq((long long)pl.size(), 1, "a remap-created repeat collapses");
        checkEq(pl[0], 21, "leaving the keeper");
    }
    // A pre-existing intentional repeat is not the remap's doing, so it stays.
    {
        std::vector<std::uint32_t> pl = {10, 10, 30};
        applyRemovalToPlaylist({99}, nullptr, &pl);
        checkEq((long long)pl.size(), 3, "an intentional repeat is preserved");
    }
    // Order must survive a removal from the middle of a long playlist.
    {
        std::vector<std::uint32_t> pl = {1, 2, 3, 4, 5};
        KeeperRemap remap{{3, 33}};
        applyRemovalToPlaylist({2, 3}, &remap, &pl);
        const std::vector<std::uint32_t> want = {1, 33, 4, 5};
        check(pl == want, "order is preserved across a mixed removal");
    }
}

void testTagWriting(const fs::path& sample) {
    std::printf("tag writing\n");
    if (sample.empty()) {
        std::printf("  (skipped: no audio file supplied)\n");
        return;
    }
    // Work on a copy: this rewrites tags, and never on the user's file.
    std::error_code ec;
    const fs::path copy =
        fs::temp_directory_path() / "podbox_tag_test" / sample.filename();
    fs::create_directories(copy.parent_path(), ec);
    fs::copy_file(sample, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::printf("  (skipped: could not copy a sample)\n");
        return;
    }

    const AudioFingerprint before = fingerprintFile(copy);

    Track edit = readFileMetadata(copy).track;
    edit.title = "PodBox Test Title";
    edit.artist = "PodBox Test Artist";
    std::string err;
    check(writeFileTags(copy, edit, &err), "writes tags back to a file");

    const FileMeta reread = readFileMetadata(copy);
    check(reread.ok && reread.track.title == "PodBox Test Title",
          "the new title reads back");
    check(reread.ok && reread.track.artist == "PodBox Test Artist",
          "the new artist reads back");

    // The whole point of hashing the audio stream rather than the file: a tag
    // edit must not look like a different song.
    check(fingerprintFile(copy) == before,
          "retagging does not change the fingerprint");

    fs::remove_all(copy.parent_path(), ec);
}

void testStore() {
    std::printf("fingerprint store\n");

    const fs::path mount = fs::temp_directory_path() / "podbox_store_test";
    std::error_code ec;
    fs::remove_all(mount, ec);
    fs::create_directories(mount / "iPod_Control" / "iTunes", ec);

    FingerprintStore store;
    store.put(111, AudioFingerprint{1000, 0xdeadbeefcafef00dull},
              FingerprintStore::Origin::Source);
    store.put(222, AudioFingerprint{2000, 42}, FingerprintStore::Origin::Device);
    check(store.save(mount), "saves a sidecar");

    FingerprintStore loaded;
    loaded.load(mount);
    checkEq((long long)loaded.size(), 2, "reloads both entries");
    check(loaded.get(111) && *loaded.get(111) ==
                                 AudioFingerprint{1000, 0xdeadbeefcafef00dull},
          "round-trips a 64-bit hash without loss");
    check(loaded.origin(222) == FingerprintStore::Origin::Device,
          "round-trips the origin");

    // A device-side hash must never clobber a source one: only the source
    // fingerprint survives transcoding, so it is the more valuable record.
    loaded.put(111, AudioFingerprint{9999, 1}, FingerprintStore::Origin::Device);
    check(loaded.get(111)->bytes == 1000,
          "a device hash does not overwrite a source hash");
    loaded.put(111, AudioFingerprint{7777, 2}, FingerprintStore::Origin::Source);
    check(loaded.get(111)->bytes == 7777, "but a newer source hash does");

    // Pruning drops entries whose track has left the library.
    Library lib;
    lib.tracks.push_back(mk(1, "A", "S", "Al", 1000));
    lib.tracks.back().dbid = 222;
    loaded.prune(lib);
    checkEq((long long)loaded.size(), 1, "prune drops orphaned entries");
    check(loaded.get(222) != nullptr, "and keeps live ones");

    FingerprintStore missing;
    missing.load(fs::temp_directory_path() / "podbox_no_such_mount");
    checkEq((long long)missing.size(), 0, "a missing sidecar loads as empty");

    fs::remove_all(mount, ec);
}

// Cover art, for whichever of the three container families the sample is.
// Conditional on a real file, because there is no way to assert on artwork
// without one — but when a file is supplied this is what catches a container
// PodBox silently returns nothing for, which is how FLAC went unnoticed.
void testArtwork(const fs::path& sample) {
    std::printf("artwork\n");
    if (sample.empty()) {
        std::printf("  (skipped: no audio file supplied)\n");
        return;
    }
    const podbox::ArtImage art = podbox::loadEmbeddedArtwork(sample);
    if (art.width == 0) {
        // Not a failure: plenty of files genuinely carry no cover.
        std::printf("  (no embedded art in %s)\n",
                    sample.filename().string().c_str());
        return;
    }
    check(art.width > 0 && art.height > 0, "decoded art has real dimensions");
    checkEq((long long)art.rgba.size(),
            (long long)art.width * art.height * 4, "RGBA buffer matches WxH");
}

void testFingerprint(const fs::path& sample) {
    std::printf("fingerprint\n");
    if (sample.empty()) {
        std::printf("  (skipped: no audio file supplied)\n");
        return;
    }

    const AudioFingerprint a = fingerprintFile(sample);
    check(a.ok(), "fingerprints a real audio file");
    check(a == fingerprintFile(sample), "is deterministic across two reads");

    // A byte-for-byte copy must fingerprint identically; that is the exact
    // case "I dragged the same file in twice" relies on.
    const fs::path copy =
        fs::temp_directory_path() / "podbox_fp_copy" / sample.filename();
    std::error_code ec;
    fs::create_directories(copy.parent_path(), ec);
    fs::copy_file(sample, copy, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        check(fingerprintFile(copy) == a, "a copied file fingerprints identically");
        fs::remove(copy, ec);
    }

    check(!fingerprintFile("/nonexistent/nope.mp3").ok(),
          "a missing file yields no fingerprint");
}

// Recursively collects importable audio, skipping partial-download folders.
void collect(const fs::path& root, std::vector<fs::path>* out) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            const std::string name = toLower(p.filename().string());
            if (name == "downloading" || name == "incomplete") it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isImportableAudioFile(p)) out->push_back(p);
    }
}

void scanFolder(const fs::path& root, bool verbose) {
    std::printf("\nscanning %s\n", root.c_str());

    std::vector<fs::path> files;
    collect(root, &files);
    std::printf("  %zu audio files\n", files.size());

    Library lib;
    std::map<std::string, int> byExt;
    std::uint32_t nextId = 1;
    int noIdentity = 0;
    for (const fs::path& f : files) {
        FileMeta meta = readFileMetadata(f);
        if (!meta.ok) continue;
        meta.track.id = nextId++;
        meta.track.dbid = meta.track.id;
        meta.track.location = f.string();
        if (!hasIdentity(meta.track)) ++noIdentity;
        byExt[toLower(f.extension().string())]++;
        lib.tracks.push_back(std::move(meta.track));
    }

    std::printf("  %zu readable", lib.tracks.size());
    for (const auto& [ext, n] : byExt) std::printf("  %s:%d", ext.c_str(), n);
    std::printf("\n  %d with no artist and no title (never grouped)\n", noIdentity);

    for (const auto mode : {MatchMode::Exact, MatchMode::Loose}) {
        const auto groups = findDuplicates(lib, mode, {});
        std::uint64_t copies = 0, bytes = 0;
        int mixed = 0;
        for (const auto& g : groups) {
            copies += g.trackIds.size() - 1;
            bytes += g.reclaimBytes;
            bool lossless = false, lossy = false;
            for (std::uint32_t id : g.trackIds)
                for (const Track& t : lib.tracks)
                    if (t.id == id) (isLossless(t) ? lossless : lossy) = true;
            if (lossless && lossy) ++mixed;
        }
        std::printf("  %-6s %3zu groups, %3llu redundant copies, %.1f MB, %d mixed lossless/lossy\n",
                    mode == MatchMode::Exact ? "Exact" : "Loose", groups.size(),
                    (unsigned long long)copies, double(bytes) / (1024 * 1024), mixed);

        if (!verbose) continue;
        for (const auto& g : groups) {
            for (std::size_t i = 0; i < g.trackIds.size(); ++i) {
                for (const Track& t : lib.tracks) {
                    if (t.id != g.trackIds[i]) continue;
                    std::printf("      %s %-28.28s %-30.30s %6ums %s\n",
                                i == 0 ? "keep" : "  - ", t.artist.c_str(),
                                t.title.c_str(), t.lengthMs,
                                t.location.c_str());
                }
            }
            std::printf("\n");
        }
    }
}

}  // namespace

// Media type decides which sidebar list a track lands in and whether the iPod
// keeps it out of shuffle, so the classification is worth pinning down.
void testMediaTypes() {
    std::printf("media types\n");
    // The extension wins outright: it is the one unambiguous signal.
    checkEq(podbox::classifyMediaType("book.m4b", ""),
            podbox::kMediaAudiobook, "m4b is an audiobook");
    checkEq(podbox::classifyMediaType("book.M4B", "Rock"),
            podbox::kMediaAudiobook, "extension beats genre, case-insensitively");
    // Genre is the fallback, since it is what publishers fill in.
    checkEq(podbox::classifyMediaType("ep.mp3", "Podcast"),
            podbox::kMediaPodcast, "podcast genre");
    checkEq(podbox::classifyMediaType("ep.mp3", "podcasts"),
            podbox::kMediaPodcast, "podcast genre, plural and lowercase");
    checkEq(podbox::classifyMediaType("x.m4a", "Books & Spoken"),
            podbox::kMediaAudiobook, "spoken-word genre is an audiobook");
    // Everything else is music, including the empty case.
    checkEq(podbox::classifyMediaType("song.mp3", "Rock"), podbox::kMediaAudio,
            "ordinary music");
    checkEq(podbox::classifyMediaType("song.flac", ""), podbox::kMediaAudio,
            "no genre is music");
    // A genre that merely mentions one of the words is not a match.
    checkEq(podbox::classifyMediaType("song.mp3", "Podcast Rock"),
            podbox::kMediaAudio, "partial genre match is not a podcast");
}

int main(int argc, char** argv) {
    // dedupe_test --fp a b ...   print and compare fingerprints directly
    if (argc > 2 && std::string(argv[1]) == "--fp") {
        AudioFingerprint first;
        for (int i = 2; i < argc; ++i) {
            const AudioFingerprint fp = fingerprintFile(argv[i]);
            std::error_code ec;
            std::printf("  bytes=%-12llu hash=%016llx  file=%llu  %s\n",
                        (unsigned long long)fp.bytes,
                        (unsigned long long)fp.hash,
                        (unsigned long long)fs::file_size(argv[i], ec),
                        argv[i]);
            if (i == 2)
                first = fp;
            else if (!(fp == first))
                std::printf("     ^ differs from the first\n");
        }
        return 0;
    }

    testKeys();
    testKeeperRanking();
    testGrouping();
    testBucketBoundary();
    testPlaylistRemoval();
    testStore();
    testMediaTypes();

    fs::path root, sample;
    if (argc > 1) {
        root = argv[1];
        std::vector<fs::path> files;
        collect(root, &files);
        if (!files.empty()) sample = files.front();
    }
    testFingerprint(sample);
    testArtwork(sample);
    testTagWriting(sample);

    if (!root.empty()) scanFolder(root, argc > 2);

    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
