// CLI add: copies (or transcodes) audio files onto a mounted iPod and updates
// every database that model needs. The existing accepted database is verified
// before any audio is copied; signed devices are never written on guesswork.
//   podbox_add <mount> [--guid <16-hex>] [--alac|--mp3] <file>...
#include "device/usb_serial.h"
#include "itdb/hash58.h"
#include "itdb/hash72.h"
#include "itdb/hashab.h"
#include "itdb/itunesdb.h"
#include "itdb/itunessd.h"
#include "itdb/itunessqlite.h"
#include "library/metadata.h"
#include "library/transcode.h"
#include "sync/sync_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct SigningContext {
    podbox::WriteOptions options;
    std::optional<podbox::HashInfo> newHashInfo;
    Bytes guid;
    Bytes hashAbNonce;
};

struct Replacement {
    fs::path live;
    fs::path staged;
    fs::path old;
    bool hadLive = false;
    bool movedLive = false;
    bool installed = false;
};

bool readBytes(const fs::path &path, Bytes *bytes, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "Could not read " + path.string();
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

std::string readText(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string plistString(const std::string &xml, const std::string &key) {
    const std::size_t keyAt = xml.find("<key>" + key + "</key>");
    if (keyAt == std::string::npos)
        return {};
    const std::size_t startTag = xml.find("<string>", keyAt);
    if (startTag == std::string::npos)
        return {};
    const std::size_t start = startTag + std::strlen("<string>");
    const std::size_t end = xml.find("</string>", start);
    return end == std::string::npos ? std::string{}
                                    : xml.substr(start, end - start);
}

fs::path databasePath(const fs::path &mount) {
    const fs::path dir = mount / "iPod_Control" / "iTunes";
    const fs::path cdb = dir / "iTunesCDB";
    std::error_code ec;
    if (fs::exists(cdb, ec) && fs::file_size(cdb, ec) > 0)
        return cdb;
    return dir / "iTunesDB";
}

Bytes guidForMount(const fs::path &mount, const std::string &overrideGuid) {
    if (!overrideGuid.empty())
        return podbox::parseFirewireGuid(overrideGuid);
    const fs::path extended =
        mount / "iPod_Control" / "Device" / "SysInfoExtended";
    std::string value = plistString(readText(extended), "FireWireGUID");
    if (value.empty())
        value = podbox::usbSerialForMount(mount);
    return podbox::parseFirewireGuid(value);
}

bool prepareSigning(const podbox::Library &library, const fs::path &mount,
                    const fs::path &dbPath, const std::string &overrideGuid,
                    SigningContext *context, std::string *error) {
    if (library.hashingScheme == podbox::kChecksumNone)
        return true;
    if (library.hashingScheme != podbox::kChecksumHash58 &&
        library.hashingScheme != podbox::kChecksumHash72 &&
        library.hashingScheme != podbox::kChecksumHashAB) {
        if (error)
            *error = "Unsupported database hashing scheme " +
                     std::to_string(library.hashingScheme);
        return false;
    }

    context->guid = guidForMount(mount, overrideGuid);
    if (context->guid.empty()) {
        if (error)
            *error = "This iPod needs a checksum, but its FireWire GUID could "
                     "not be found (use --guid <16-hex> to supply it)";
        return false;
    }

    Bytes image;
    if (!readBytes(dbPath, &image, error))
        return false;
    if (library.hashingScheme == podbox::kChecksumHash58) {
        const Bytes stored = podbox::storedHash58(image);
        const Bytes computed = podbox::hash58OfDatabase(image, context->guid);
        if (computed.empty() || stored != computed) {
            if (error)
                *error = "Could not reproduce this iPod's hash58 checksum; "
                         "nothing was changed";
            return false;
        }
        context->options.hash58Guid = context->guid;
        return true;
    }

    if (library.hashingScheme == podbox::kChecksumHash72) {
        const Bytes digest = podbox::hash72Sha1(image);
        const Bytes stored = podbox::storedHash72(image);
        if (digest.empty() || stored.empty()) {
            if (error)
                *error = "Could not read this iPod's hash72 checksum";
            return false;
        }
        const fs::path hashInfo =
            mount / "iPod_Control" / "Device" / "HashInfo";
        Bytes iv;
        Bytes random;
        if (const auto info = podbox::readHashInfo(hashInfo, context->guid)) {
            if (podbox::hash72Signature(digest, info->iv, info->rndpart) !=
                stored) {
                if (error)
                    *error =
                        "This iPod's database does not match its HashInfo; "
                        "nothing was changed";
                return false;
            }
            iv = info->iv;
            random = info->rndpart;
        } else {
            const auto recovered = podbox::hash72Extract(stored, digest);
            if (!recovered) {
                if (error)
                    *error = "Could not reproduce this iPod's hash72 checksum; "
                             "nothing was changed";
                return false;
            }
            iv = recovered->iv;
            random = recovered->rndpart;
            context->newHashInfo =
                podbox::HashInfo{podbox::hash72Uuid(context->guid), random, iv};
        }
        context->options.hash72Iv = std::move(iv);
        context->options.hash72Rndpart = std::move(random);
        context->options.compressed = library.compressed;
        return true;
    }

    const Bytes digest = podbox::hashAbSha1(image);
    const auto nonce = podbox::hashAbExtractNonce(podbox::storedHashAb(image),
                                                  digest, context->guid);
    if (!nonce) {
        if (error)
            *error = "Could not reproduce this nano's hashAB checksum; nothing "
                     "was changed";
        return false;
    }
    const fs::path sqlite = dbPath.parent_path() / "iTunes Library.itlp";
    if (!podbox::validateItunesSqliteBundle(library, sqlite, context->guid,
                                            *nonce, error))
        return false;
    context->hashAbNonce = *nonce;
    context->options.hashAbUuid = context->guid;
    context->options.hashAbNonce = *nonce;
    context->options.compressed = true;
    return true;
}

bool validateStagedDatabase(const podbox::Library &expected,
                            const fs::path &path, const SigningContext &context,
                            std::string *error) {
    const podbox::ParseResult parsed = podbox::parseItunesDb(path);
    if (!parsed.library ||
        parsed.library->tracks.size() != expected.tracks.size() ||
        parsed.library->hashingScheme != expected.hashingScheme) {
        if (error)
            *error = parsed.error.empty()
                         ? "Staged database did not pass its read-back check"
                         : "Staged database check failed: " + parsed.error;
        return false;
    }
    if (expected.hashingScheme == podbox::kChecksumNone)
        return true;

    Bytes image;
    if (!readBytes(path, &image, error))
        return false;
    if (expected.hashingScheme == podbox::kChecksumHash58)
        return podbox::storedHash58(image) ==
               podbox::hash58OfDatabase(image, context.guid);
    if (expected.hashingScheme == podbox::kChecksumHash72) {
        const Bytes digest = podbox::hash72Sha1(image);
        return podbox::storedHash72(image) ==
               podbox::hash72Signature(digest, context.options.hash72Iv,
                                       context.options.hash72Rndpart);
    }
    if (expected.hashingScheme == podbox::kChecksumHashAB) {
        const Bytes digest = podbox::hashAbSha1(image);
        return podbox::storedHashAb(image) ==
               podbox::hashAbSignature(digest, context.guid,
                                       context.hashAbNonce);
    }
    return false;
}

bool validateShuffleStats(const fs::path &path, std::size_t expected) {
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 4> count{};
    if (!in.read(reinterpret_cast<char *>(count.data()), count.size()))
        return false;
    const std::uint32_t actual = count[0] | (std::uint32_t(count[1]) << 8) |
                                 (std::uint32_t(count[2]) << 16) |
                                 (std::uint32_t(count[3]) << 24);
    return actual == expected;
}

bool makeBackup(const fs::path &live, std::string *error) {
    std::error_code ec;
    if (!fs::exists(live, ec))
        return true;
    const fs::path backup = live.string() + ".podbox-backup";
    if (fs::exists(backup, ec))
        return true;
    if (fs::is_directory(live, ec))
        fs::copy(live, backup,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 ec);
    else
        fs::copy_file(live, backup, ec);
    if (!ec)
        return true;
    if (error)
        *error = "Could not back up " + live.string() + ": " + ec.message();
    return false;
}

bool replaceTogether(std::vector<Replacement> *replacements,
                     bool *rollbackComplete, std::string *error) {
    if (rollbackComplete)
        *rollbackComplete = true;
    std::error_code ec;
    for (Replacement &item : *replacements) {
        if (!fs::exists(item.staged, ec)) {
            if (error)
                *error = "Missing staged file " + item.staged.string();
            return false;
        }
        item.old = item.live.string() + ".podbox-old";
        fs::remove_all(item.old, ec);
        ec.clear();
    }

    auto rollback = [&] {
        bool complete = true;
        std::error_code rollbackError;
        for (Replacement &item : *replacements)
            if (item.installed) {
                fs::remove_all(item.live, rollbackError);
                if (rollbackError)
                    complete = false;
            }
        for (auto it = replacements->rbegin(); it != replacements->rend();
             ++it) {
            rollbackError.clear();
            if (it->movedLive && fs::exists(it->old, rollbackError)) {
                fs::rename(it->old, it->live, rollbackError);
                if (rollbackError)
                    complete = false;
            } else if (it->movedLive) {
                complete = false;
            }
        }
        if (rollbackComplete)
            *rollbackComplete = complete;
    };

    for (Replacement &item : *replacements) {
        item.hadLive = fs::exists(item.live, ec);
        if (!item.hadLive)
            continue;
        fs::rename(item.live, item.old, ec);
        if (ec) {
            const std::string detail = ec.message();
            rollback();
            if (error)
                *error = "Could not stage the existing " + item.live.string() +
                         " for replacement: " + detail;
            return false;
        }
        item.movedLive = true;
    }
    for (Replacement &item : *replacements) {
        fs::rename(item.staged, item.live, ec);
        if (ec) {
            const std::string detail = ec.message();
            rollback();
            if (error)
                *error =
                    "Could not install " + item.live.string() + ": " + detail;
            return false;
        }
        item.installed = true;
    }
    for (Replacement &item : *replacements)
        fs::remove_all(item.old, ec);
    return true;
}

void removePaths(const std::vector<fs::path> &paths) {
    std::error_code ec;
    for (const fs::path &path : paths)
        fs::remove_all(path, ec);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: podbox_add <mount-point> [--guid <16-hex>] "
                     "[--alac|--mp3] <file>...\n");
        return 2;
    }
    const fs::path mount = argv[1];
    const fs::path dbPath = databasePath(mount);
    const fs::path itunesDir = mount / "iPod_Control" / "iTunes";
    const fs::path sdPath = itunesDir / "iTunesSD";
    const fs::path statsPath = itunesDir / "iTunesStats";
    const podbox::ItunesSdKind sdKind = podbox::detectItunesSd(sdPath);
    if (sdKind == podbox::ItunesSdKind::Legacy) {
        std::fprintf(stderr,
                     "error: this older iPod shuffle uses an unsupported "
                     "iTunesSD format\n");
        return 1;
    }

    podbox::ImportFormat format = podbox::ImportFormat::Original;
    std::string guidOverride;
    int firstFile = 2;
    while (firstFile < argc && argv[firstFile][0] == '-') {
        if (std::strcmp(argv[firstFile], "--alac") == 0) {
            format = podbox::ImportFormat::Alac;
            ++firstFile;
        } else if (std::strcmp(argv[firstFile], "--mp3") == 0) {
            format = podbox::ImportFormat::Mp3;
            ++firstFile;
        } else if (std::strcmp(argv[firstFile], "--guid") == 0 &&
                   firstFile + 1 < argc) {
            guidOverride = argv[firstFile + 1];
            firstFile += 2;
        } else if (std::strcmp(argv[firstFile], "--") == 0) {
            ++firstFile;
            break;
        } else {
            std::fprintf(stderr, "error: unknown or incomplete option: %s\n",
                         argv[firstFile]);
            return 2;
        }
    }
    if (firstFile == argc) {
        std::fprintf(stderr, "error: no audio files were supplied\n");
        return 2;
    }
    if (!guidOverride.empty() &&
        podbox::parseFirewireGuid(guidOverride).empty()) {
        std::fprintf(stderr,
                     "error: --guid must be exactly 16 hexadecimal digits\n");
        return 2;
    }

    podbox::ParseResult result = podbox::parseItunesDb(dbPath);
    if (!result.library) {
        std::fprintf(stderr, "error: %s\n", result.error.c_str());
        return 1;
    }
    podbox::Library library = std::move(*result.library);
    SigningContext signing;
    std::string error;
    if (!prepareSigning(library, mount, dbPath, guidOverride, &signing,
                        &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::uint32_t nextId = 100;
    for (const auto &track : library.tracks)
        nextId = std::max(nextId, track.id + 1);
    std::mt19937_64 rng{std::random_device{}()};
    std::vector<fs::path> importedFiles;
    int added = 0;
    for (int i = firstFile; i < argc; ++i) {
        const fs::path source = argv[i];
        podbox::FileMeta metadata = podbox::readFileMetadata(source);
        if (!metadata.ok) {
            std::fprintf(stderr, "skip: %s\n", metadata.error.c_str());
            continue;
        }
        std::string location;
        const fs::path destination = podbox::allocateMusicPath(
            mount, podbox::importExtension(format, source), &location);
        std::string importError;
        if (destination.empty() ||
            !podbox::importAudio(format, source, destination, &importError)) {
            if (!destination.empty()) {
                std::error_code cleanupError;
                fs::remove(destination, cleanupError);
            }
            std::fprintf(stderr, "skip: %s\n",
                         importError.empty()
                             ? source.filename().string().c_str()
                             : importError.c_str());
            continue;
        }
        importedFiles.push_back(destination);
        std::error_code ec;
        if (const std::uint32_t size =
                std::uint32_t(fs::file_size(destination, ec));
            !ec)
            metadata.track.sizeBytes = size;
        metadata.track.id = nextId++;
        metadata.track.dbid = rng();
        metadata.track.location = location;
        library.tracks.push_back(std::move(metadata.track));
        std::printf("added \"%s\" -> %s\n", library.tracks.back().title.c_str(),
                    location.c_str());
        ++added;
    }
    if (!added) {
        std::fprintf(stderr, "nothing added\n");
        return 1;
    }

    const fs::path dbTmp = dbPath.string() + ".podbox-tmp";
    const fs::path sqlitePath = itunesDir / "iTunes Library.itlp";
    const fs::path sqliteTmp = sqlitePath.string() + ".podbox-tmp";
    const fs::path sdTmp = sdPath.string() + ".podbox-tmp";
    const fs::path statsTmp = statsPath.string() + ".podbox-tmp";
    const fs::path hashInfoPath =
        mount / "iPod_Control" / "Device" / "HashInfo";
    const fs::path hashInfoTmp = hashInfoPath.string() + ".podbox-tmp";
    std::vector<fs::path> staged{dbTmp};
    std::vector<fs::path> newAnnouncements;
    std::vector<fs::path> expectedAnnouncements;
    auto fail = [&](const std::string &message, bool removeImports = true) {
        removePaths(staged);
        if (removeImports) {
            removePaths(importedFiles);
            removePaths(newAnnouncements);
        }
        std::fprintf(stderr, "error: %s\n", message.c_str());
        return 1;
    };

    if (!podbox::writeItunesDb(library, dbTmp, &error, signing.options) ||
        !validateStagedDatabase(library, dbTmp, signing, &error)) {
        if (error.empty())
            error = "Staged database checksum did not verify";
        return fail(error);
    }

    const bool writeSqlite = library.hashingScheme == podbox::kChecksumHashAB;
    if (writeSqlite) {
        staged.push_back(sqliteTmp);
        if (!podbox::writeItunesSqliteBundle(library, sqlitePath, sqliteTmp,
                                             signing.guid, signing.hashAbNonce,
                                             &error))
            return fail(error);
    }

    if (sdKind == podbox::ItunesSdKind::Modern) {
        staged.push_back(sdTmp);
        staged.push_back(statsTmp);
        const fs::path tracks = mount / "iPod_Control" / "Speakable" / "Tracks";
        std::error_code ec;
        for (std::size_t i = library.tracks.size() - std::size_t(added);
             i < library.tracks.size(); ++i) {
            const fs::path voice =
                tracks /
                (podbox::shuffleVoiceOverName(library.tracks[i].dbid) + ".wav");
            expectedAnnouncements.push_back(voice);
            if (!fs::exists(voice, ec))
                newAnnouncements.push_back(voice);
        }
        if (!podbox::writeItunesSd(library, sdPath, sdTmp, mount, &error) ||
            !podbox::writeShuffleStats(library, sdPath, statsPath, statsTmp,
                                       &error))
            return fail(error);
        const auto check = podbox::parseItunesSd(sdTmp, &error);
        if (!check || check->tracks.size() != library.tracks.size())
            return fail(error.empty() ? "Staged Shuffle database did not verify"
                                      : error);
        if (!validateShuffleStats(statsTmp, library.tracks.size()))
            return fail("Staged Shuffle statistics did not verify");
        for (const fs::path &voice : expectedAnnouncements)
            if (!fs::exists(voice, ec))
                return fail("A Shuffle VoiceOver file was not generated");
    }

    if (signing.newHashInfo) {
        staged.push_back(hashInfoTmp);
        if (!podbox::writeHashInfo(hashInfoTmp, *signing.newHashInfo))
            return fail("Could not stage this nano's HashInfo file");
    }

    std::vector<Replacement> replacements{{dbPath, dbTmp}};
    if (writeSqlite)
        replacements.push_back({sqlitePath, sqliteTmp});
    if (sdKind == podbox::ItunesSdKind::Modern) {
        replacements.push_back({sdPath, sdTmp});
        replacements.push_back({statsPath, statsTmp});
    }
    if (signing.newHashInfo)
        replacements.push_back({hashInfoPath, hashInfoTmp});
    for (const Replacement &item : replacements)
        if (!makeBackup(item.live, &error))
            return fail(error);
    bool rollbackComplete = true;
    if (!replaceTogether(&replacements, &rollbackComplete, &error)) {
        if (!rollbackComplete)
            error += "; automatic rollback was incomplete, so imported audio "
                     "was retained for recovery";
        return fail(error, rollbackComplete);
    }

    if (library.compressed && dbPath.filename() == "iTunesCDB") {
        std::ofstream placeholder(itunesDir / "iTunesDB",
                                  std::ios::binary | std::ios::trunc);
    }
    std::printf("database updated: %zu tracks (checksum scheme %u)\n",
                library.tracks.size(), library.hashingScheme);
    return 0;
}
