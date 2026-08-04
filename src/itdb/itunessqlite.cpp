#include "itdb/itunessqlite.h"

#include "itdb/hash58.h"
#include "itdb/hashab.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace podbox {
namespace {

class Database {
public:
    ~Database() { if (db_) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database() = default;

    bool open(const fs::path& path, std::string* error,
              bool readOnly = false) {
        if (sqlite3_open_v2(path.string().c_str(), &db_,
                            readOnly ? SQLITE_OPEN_READONLY
                                     : SQLITE_OPEN_READWRITE,
                            nullptr) == SQLITE_OK)
            return true;
        if (error)
            *error = "Could not open " + path.filename().string() + ": " +
                     (db_ ? sqlite3_errmsg(db_) : "SQLite error");
        return false;
    }
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement() = default;
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool prepare(sqlite3* db, const char* sql, std::string* error) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK)
            return true;
        if (error) *error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_stmt* get() const { return stmt_; }
    bool run(sqlite3* db, std::string* error) {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_DONE) return true;
        if (error) *error = sqlite3_errmsg(db);
        return false;
    }
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

bool exec(sqlite3* db, const std::string& sql, std::string* error) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (result == SQLITE_OK) return true;
    if (error) *error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    return false;
}

bool tableExists(sqlite3* db, const char* table) {
    Statement statement;
    if (!statement.prepare(
            db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            nullptr))
        return false;
    sqlite3_bind_text(statement.get(), 1, table, -1, SQLITE_TRANSIENT);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool requireTables(sqlite3* db, std::initializer_list<const char*> tables,
                   const char* filename, std::string* error) {
    for (const char* table : tables) {
        if (tableExists(db, table)) continue;
        if (error)
            *error = std::string(filename) + " is missing its " + table +
                     " table";
        return false;
    }
    return true;
}

bool begin(sqlite3* db, std::string* error) {
    return exec(db, "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;", error);
}

bool commit(sqlite3* db, std::string* error) {
    return exec(db, "COMMIT;PRAGMA wal_checkpoint(TRUNCATE);", error);
}

void rollback(sqlite3* db) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (value.empty())
        sqlite3_bind_null(stmt, index);
    else
        sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::int64_t macTime(std::int64_t unixTime) {
    if (unixTime <= 0) return 0;
    constexpr std::int64_t epoch = 2082844800LL;
    return std::min<std::int64_t>(unixTime + epoch,
                                  std::numeric_limits<std::uint32_t>::max());
}

std::uint32_t fileMarker(const Track& track) {
    std::string extension = fs::path(track.location).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".mp3") return 0x4D503320;  // MP3 fourcc
    if (extension == ".m4a") return 0x4D344120;  // M4A fourcc
    if (extension == ".m4b") return 0x4D344220;  // M4B fourcc
    if (extension == ".m4r") return 0x4D345220;  // M4R fourcc
    if (extension == ".mp4") return 0x4D503420;  // MP4 fourcc
    if (extension == ".wav") return 0x57415620;  // WAV fourcc
    if (extension == ".aif" || extension == ".aiff") return 0x41494646;
    if (extension == ".aac") return 0x41414320;  // AAC fourcc
    return 0;
}

int audioFormat(std::uint32_t marker) {
    if (marker == 0x4D503320) return 301;
    if (marker == 0x4D344120 || marker == 0x4D344220 ||
        marker == 0x4D345220 || marker == 0x4D503420)
        return 502;
    return 0;
}

std::string fileKind(std::uint32_t marker) {
    if (marker == 0x4D503320) return "MPEG audio file";
    if (marker == 0x4D344120 || marker == 0x4D344220 ||
        marker == 0x4D345220 || marker == 0x4D503420)
        return "AAC audio file";
    if (marker == 0x57415620) return "WAV audio file";
    if (marker == 0x41494646) return "AIFF audio file";
    return "Audio file";
}

std::string relativeLocation(const std::string& value) {
    std::string path = value;
    std::replace(path.begin(), path.end(), ':', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    constexpr std::string_view prefix = "iPod_Control/Music/";
    if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
    return path;
}

std::uint64_t stableId(std::string_view kind, std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](std::string_view part) {
        for (const unsigned char c : part) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
    };
    mix(kind);
    mix(value);
    hash |= 0x4000000000000000ULL;
    return hash ? hash : 1;
}

std::vector<std::uint8_t> nanoSortKey(const unsigned char* input) {
    if (!input || !*input) return {0x31, 0x01, 0x01, 0x00};
    std::string text(reinterpret_cast<const char*>(input));
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    int transformedLength = 0;
    int words = 1;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == ' ')
            ++transformedLength;
        else
            transformedLength += 2;
        if (c == ' ') ++words;
    }
    const int wordOffset = 1 + transformedLength + 3;
    std::vector<std::uint8_t> result(wordOffset + words * 2 + 1, 0);
    std::size_t output = 0;
    result[output++] = 0x30;
    int word = 0, wordLength = 0;
    for (const unsigned char c : text) {
        ++wordLength;
        if (std::isalnum(c)) {
            result[output++] = std::uint8_t(2 * c - 0x55);
            continue;
        }
        if (c == ' ') {
            result[output++] = 0x06;
            --wordLength;
            result[wordOffset + word * 2] = 0x8F;
            result[wordOffset + word * 2 + 1] =
                std::uint8_t(0x86 - wordLength);
            ++word;
            wordLength = 0;
            continue;
        }
        const std::array<std::uint8_t, 2> encoded =
            c == ':'  ? std::array<std::uint8_t, 2>{0x07, 0xD8}
            : c == '-' ? std::array<std::uint8_t, 2>{0x07, 0x90}
            : c == ',' ? std::array<std::uint8_t, 2>{0x07, 0xB2}
            : c == '.' ? std::array<std::uint8_t, 2>{0x08, 0x51}
            : c == '\'' ? std::array<std::uint8_t, 2>{0x07, 0x31}
                        : std::array<std::uint8_t, 2>{0x07, 0x90};
        result[output++] = encoded[0];
        result[output++] = encoded[1];
    }
    result[wordOffset + word * 2] = 0x8F;
    result[wordOffset + word * 2 + 1] = std::uint8_t(3 + wordLength);
    result[wordOffset - 3] = 0x01;
    result[wordOffset - 2] = std::uint8_t(text.size() + 4);
    result[wordOffset - 1] = 0x01;
    return result;
}

void sqliteNanoSortKey(sqlite3_context* context, int argc,
                       sqlite3_value** values) {
    if (argc != 1 || sqlite3_value_type(values[0]) == SQLITE_NULL) {
        const auto result = nanoSortKey(nullptr);
        sqlite3_result_blob(context, result.data(), int(result.size()),
                            SQLITE_TRANSIENT);
        return;
    }
    const auto result = nanoSortKey(sqlite3_value_text(values[0]));
    sqlite3_result_blob(context, result.data(), int(result.size()),
                        SQLITE_TRANSIENT);
}

void sqliteNanoSortSection(sqlite3_context* context, int argc,
                           sqlite3_value** values) {
    int result = 26;
    if (argc == 1) {
        const auto* value = sqlite3_value_text(values[0]);
        if (value && sqlite3_value_bytes(values[0]) >= 2 &&
            value[0] == 0x30 && value[1] >= 0x2D &&
            value[1] <= 0x5F)
            result = (value[1] - 0x2D) / 2;
    }
    sqlite3_result_int(context, result);
}

void registerNanoFunctions(sqlite3* db) {
    sqlite3_create_function_v2(db, "iPhoneSortKey", 1,
                               SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                               sqliteNanoSortKey, nullptr, nullptr, nullptr);
    sqlite3_create_function_v2(db, "iPhoneSortSection", 1,
                               SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                               sqliteNanoSortSection, nullptr, nullptr, nullptr);
}

bool updateDynamic(const Library& library, const fs::path& path,
                   std::string* error) {
    Database database;
    if (!database.open(path, error)) return false;
    sqlite3* db = database.get();
    registerNanoFunctions(db);
    if (!requireTables(db, {"item_stats", "container_ui"}, "Dynamic.itdb",
                       error) ||
        !begin(db, error))
        return false;

    auto fail = [&] { rollback(db); return false; };
    if (!exec(db,
              "CREATE TEMP TABLE podbox_items(pid INTEGER PRIMARY KEY);"
              "CREATE TEMP TABLE podbox_containers(pid INTEGER PRIMARY KEY);",
              error))
        return fail();
    Statement currentItem, stats, currentContainer, ui;
    if (!currentItem.prepare(db, "INSERT INTO podbox_items VALUES(?)", error) ||
        !stats.prepare(
            db,
            "INSERT INTO item_stats(item_pid,has_been_played,play_count_user,"
            "user_rating,user_rating_common) VALUES(?,?,?,?,?) "
            "ON CONFLICT(item_pid) DO UPDATE SET "
            "has_been_played=excluded.has_been_played,"
            "play_count_user=excluded.play_count_user,"
            "user_rating=excluded.user_rating,"
            "user_rating_common=excluded.user_rating_common",
            error) ||
        !currentContainer.prepare(db,
                                  "INSERT INTO podbox_containers VALUES(?)",
                                  error) ||
        !ui.prepare(
            db,
            "INSERT INTO container_ui(container_pid,play_order,is_reversed,"
            "album_field_order,repeat_mode,shuffle_items,has_been_shuffled) "
            "VALUES(?,7,0,1,0,0,0) ON CONFLICT(container_pid) DO NOTHING",
            error))
        return fail();

    for (const Track& track : library.tracks) {
        if (!track.dbid) { if (error) *error = "A track has no persistent id"; return fail(); }
        sqlite3_bind_int64(currentItem.get(), 1, sqlite3_int64(track.dbid));
        if (!currentItem.run(db, error)) return fail();
        currentItem.reset();
        sqlite3_bind_int64(stats.get(), 1, sqlite3_int64(track.dbid));
        sqlite3_bind_int(stats.get(), 2, track.playCount > 0);
        sqlite3_bind_int64(stats.get(), 3, track.playCount);
        sqlite3_bind_int(stats.get(), 4, track.rating);
        sqlite3_bind_int(stats.get(), 5, track.rating);
        if (!stats.run(db, error)) return fail();
        stats.reset();
    }
    std::vector<std::uint64_t> containers = {library.masterDbid};
    for (const Playlist& playlist : library.playlists)
        containers.push_back(playlist.dbid);
    for (const std::uint64_t id : containers) {
        if (!id) { if (error) *error = "A playlist has no persistent id"; return fail(); }
        sqlite3_bind_int64(currentContainer.get(), 1, sqlite3_int64(id));
        if (!currentContainer.run(db, error)) return fail();
        currentContainer.reset();
        sqlite3_bind_int64(ui.get(), 1, sqlite3_int64(id));
        if (!ui.run(db, error)) return fail();
        ui.reset();
    }
    if (!exec(db,
              "DELETE FROM item_stats WHERE item_pid NOT IN "
              "(SELECT pid FROM podbox_items);"
              "DELETE FROM container_ui WHERE container_pid NOT IN "
              "(SELECT pid FROM podbox_containers);",
              error))
        return fail();
    if (tableExists(db, "rental_info") &&
        !exec(db, "DELETE FROM rental_info WHERE item_pid NOT IN "
                  "(SELECT pid FROM podbox_items)", error))
        return fail();
    return commit(db, error);
}

using IdMap = std::unordered_map<std::string, std::uint64_t>;

IdMap textIds(sqlite3* db, const char* sql) {
    IdMap result;
    Statement statement;
    if (!statement.prepare(db, sql, nullptr)) return result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement.get(), 0);
        if (text) result[reinterpret_cast<const char*>(text)] =
            std::uint64_t(sqlite3_column_int64(statement.get(), 1));
    }
    return result;
}

std::uint64_t idFor(IdMap& ids, std::string_view type,
                    const std::string& value) {
    if (value.empty()) return 0;
    if (const auto found = ids.find(value); found != ids.end())
        return found->second;
    const std::uint64_t id = stableId(type, value);
    ids.emplace(value, id);
    return id;
}

bool updateLibrary(const Library& library, const fs::path& path,
                   std::string* error) {
    Database database;
    if (!database.open(path, error)) return false;
    sqlite3* db = database.get();
    registerNanoFunctions(db);
    if (!requireTables(db,
                       {"db_info", "item", "avformat_info", "container",
                        "item_to_container", "genre_map", "location_kind_map",
                        "album", "artist", "composer"},
                       "Library.itdb", error))
        return false;

    std::uint64_t dbid = 0, master = 0;
    {
        Statement info;
        if (!info.prepare(db, "SELECT pid,primary_container_pid FROM db_info LIMIT 1",
                          error) || sqlite3_step(info.get()) != SQLITE_ROW) {
            if (error && error->empty()) *error = "Library.itdb has no db_info row";
            return false;
        }
        dbid = std::uint64_t(sqlite3_column_int64(info.get(), 0));
        master = std::uint64_t(sqlite3_column_int64(info.get(), 1));
    }
    if (!dbid || !master || (library.databaseDbid && library.databaseDbid != dbid) ||
        (library.masterDbid && library.masterDbid != master)) {
        if (error) *error = "iTunesCDB and Library.itdb persistent ids do not match";
        return false;
    }

    IdMap artists = textIds(db, "SELECT name,pid FROM artist");
    IdMap composers = textIds(db, "SELECT name,pid FROM composer");
    IdMap albums = textIds(
        db, "SELECT album.name || char(31) || coalesce(artist.name,''),album.pid "
            "FROM album LEFT JOIN artist ON album.artist_pid=artist.pid");

    if (!begin(db, error)) return false;
    auto fail = [&] { rollback(db); return false; };
    const char* cleared[] = {
        "item_to_container", "item_to_album", "item_to_artist",
        "item_to_composer", "container_seed", "video_characteristics",
        "video_info", "store_info", "podcast_info", "avformat_info", "item",
        "container", "genre_map", "category_map", "location_kind_map",
        "album", "artist", "composer"};
    for (const char* table : cleared)
        if (tableExists(db, table) &&
            !exec(db, std::string("DELETE FROM \"") + table + "\"", error))
            return fail();

    Statement genre, item, av, kind, artist, album, composer, container, member;
    if (!genre.prepare(db, "INSERT INTO genre_map(id,genre,genre_order) VALUES(?,?,?)", error) ||
        !item.prepare(db,
            "INSERT INTO item(pid,media_kind,date_modified,year,remember_bookmark,"
            "exclude_from_shuffle,artwork_status,start_time_ms,stop_time_ms,"
            "total_time_ms,track_number,disc_number,genre_id,album_pid,artist_pid,"
            "composer_pid,title,artist,album,composer,sort_title,sort_artist,"
            "sort_album,sort_composer,title_order,artist_order,album_order,"
            "genre_order,composer_order) VALUES(?,?,?,?,0,0,2,0,?,?,?,?,?,?,?,?,?,"
            "?,?,?,?,?,?,?,?,?,?,?,?)", error) ||
        !av.prepare(db,
            "INSERT INTO avformat_info(item_pid,sub_id,audio_format,bit_rate,"
            "sample_rate,duration,gapless_heuristic_info,gapless_encoding_delay,"
            "gapless_encoding_drain,gapless_last_frame_resynch,"
            "analysis_inhibit_flags,audio_fingerprint,volume_normalization_energy) "
            "VALUES(?,0,?,?,?,0,0,0,0,0,0,0,0)", error) ||
        !kind.prepare(db, "INSERT OR IGNORE INTO location_kind_map(id,kind) VALUES(?,?)", error) ||
        !artist.prepare(db, "INSERT OR IGNORE INTO artist(pid,kind,artwork_status,artwork_album_pid,name,name_order,sort_name) VALUES(?,2,0,0,?,?,?)", error) ||
        !album.prepare(db, "INSERT OR IGNORE INTO album(pid,kind,artwork_status,artwork_item_pid,artist_pid,user_rating,name,name_order,all_compilations,feed_url,season_number) VALUES(?,2,0,0,?,0,?,?,0,NULL,0)", error) ||
        !composer.prepare(db, "INSERT OR IGNORE INTO composer(pid,name,name_order,sort_name) VALUES(?,?,?,?)", error) ||
        !container.prepare(db, "INSERT INTO container(pid,distinguished_kind,date_created,date_modified,name,name_order,parent_pid,media_kinds,workout_template_id,is_hidden,smart_is_folder) VALUES(?,0,0,0,?,?,0,?,0,?,0)", error) ||
        !member.prepare(db, "INSERT INTO item_to_container(item_pid,container_pid,physical_order,shuffle_order) VALUES(?,?,?,NULL)", error))
        return fail();

    std::unordered_map<std::string, int> genres;
    for (const Track& track : library.tracks)
        if (!track.genre.empty() && !genres.count(track.genre)) {
            const int id = int(genres.size()) + 1;
            genres[track.genre] = id;
            sqlite3_bind_int(genre.get(), 1, id);
            bindText(genre.get(), 2, track.genre);
            sqlite3_bind_int(genre.get(), 3, id - 1);
            if (!genre.run(db, error)) return fail();
            genre.reset();
        }

    for (std::size_t index = 0; index < library.tracks.size(); ++index) {
        const Track& track = library.tracks[index];
        if (!track.dbid) { if (error) *error = "A track has no persistent id"; return fail(); }
        const std::uint64_t artistId = idFor(artists, "artist", track.artist);
        const std::string albumKey = track.album + char(31) + track.artist;
        const std::uint64_t albumId = track.album.empty() ? 0 :
            idFor(albums, "album", albumKey);
        const std::uint64_t composerId = idFor(composers, "composer", track.composer);
        const std::uint32_t marker = fileMarker(track);

        int p = 0;
        sqlite3_bind_int64(item.get(), ++p, sqlite3_int64(track.dbid));
        sqlite3_bind_int64(item.get(), ++p, track.mediaType);
        sqlite3_bind_int64(item.get(), ++p, macTime(track.dateAdded));
        sqlite3_bind_int64(item.get(), ++p, track.year);
        sqlite3_bind_double(item.get(), ++p, track.lengthMs);
        sqlite3_bind_double(item.get(), ++p, track.lengthMs);
        sqlite3_bind_int64(item.get(), ++p, track.trackNumber);
        sqlite3_bind_int64(item.get(), ++p, track.discNumber);
        sqlite3_bind_int(item.get(), ++p, track.genre.empty() ? 0 : genres[track.genre]);
        sqlite3_bind_int64(item.get(), ++p, sqlite3_int64(albumId));
        sqlite3_bind_int64(item.get(), ++p, sqlite3_int64(artistId));
        sqlite3_bind_int64(item.get(), ++p, sqlite3_int64(composerId));
        for (const std::string* text : {&track.title, &track.artist, &track.album,
                                        &track.composer, &track.title,
                                        &track.artist, &track.album,
                                        &track.composer})
            bindText(item.get(), ++p, *text);
        for (int order = 0; order < 5; ++order)
            sqlite3_bind_int64(item.get(), ++p, index + 1);
        if (!item.run(db, error)) return fail();
        item.reset();

        sqlite3_bind_int64(av.get(), 1, sqlite3_int64(track.dbid));
        sqlite3_bind_int(av.get(), 2, audioFormat(marker));
        sqlite3_bind_int64(av.get(), 3, track.bitrate);
        sqlite3_bind_double(av.get(), 4, track.sampleRate);
        if (!av.run(db, error)) return fail();
        av.reset();
        sqlite3_bind_int64(kind.get(), 1, track.mediaType);
        const std::string kindName = fileKind(marker);
        bindText(kind.get(), 2, kindName);
        if (!kind.run(db, error)) return fail();
        kind.reset();

        if (artistId) {
            sqlite3_bind_int64(artist.get(), 1, sqlite3_int64(artistId));
            bindText(artist.get(), 2, track.artist);
            sqlite3_bind_int64(artist.get(), 3, index + 1);
            bindText(artist.get(), 4, track.artist);
            if (!artist.run(db, error)) return fail();
            artist.reset();
        }
        if (albumId) {
            sqlite3_bind_int64(album.get(), 1, sqlite3_int64(albumId));
            sqlite3_bind_int64(album.get(), 2, sqlite3_int64(artistId));
            bindText(album.get(), 3, track.album);
            sqlite3_bind_int64(album.get(), 4, index + 1);
            if (!album.run(db, error)) return fail();
            album.reset();
        }
        if (composerId) {
            sqlite3_bind_int64(composer.get(), 1, sqlite3_int64(composerId));
            bindText(composer.get(), 2, track.composer);
            sqlite3_bind_int64(composer.get(), 3, index + 1);
            bindText(composer.get(), 4, track.composer);
            if (!composer.run(db, error)) return fail();
            composer.reset();
        }
    }

    auto addContainer = [&](std::uint64_t id, const std::string& name,
                            bool hidden, const std::vector<std::uint32_t>& ids,
                            int order) {
        if (!id) { if (error) *error = "A playlist has no persistent id"; return false; }
        std::uint32_t kinds = 0;
        for (const std::uint32_t trackId : ids)
            for (const Track& track : library.tracks)
                if (track.id == trackId) kinds |= track.mediaType;
        sqlite3_bind_int64(container.get(), 1, sqlite3_int64(id));
        bindText(container.get(), 2, name);
        sqlite3_bind_int(container.get(), 3, order);
        sqlite3_bind_int64(container.get(), 4, kinds);
        sqlite3_bind_int(container.get(), 5, hidden);
        if (!container.run(db, error)) return false;
        container.reset();
        int position = 0;
        for (const std::uint32_t trackId : ids) {
            const auto found = std::find_if(
                library.tracks.begin(), library.tracks.end(),
                [&](const Track& track) { return track.id == trackId; });
            if (found == library.tracks.end()) continue;
            sqlite3_bind_int64(member.get(), 1, sqlite3_int64(found->dbid));
            sqlite3_bind_int64(member.get(), 2, sqlite3_int64(id));
            sqlite3_bind_int(member.get(), 3, position++);
            if (!member.run(db, error)) return false;
            member.reset();
        }
        return true;
    };
    std::vector<std::uint32_t> all;
    for (const Track& track : library.tracks) all.push_back(track.id);
    if (!addContainer(master, library.masterName.empty() ? "iPod" : library.masterName,
                      true, all, 0))
        return fail();
    for (std::size_t i = 0; i < library.playlists.size(); ++i)
        if (!addContainer(library.playlists[i].dbid, library.playlists[i].name,
                          false, library.playlists[i].trackIds, int(i + 1)))
            return fail();

    return commit(db, error);
}

bool updateLocations(const Library& library, const fs::path& path,
                     std::string* error) {
    Database database;
    if (!database.open(path, error)) return false;
    sqlite3* db = database.get();
    if (!requireTables(db, {"location", "base_location"}, "Locations.itdb",
                       error) || !begin(db, error))
        return false;
    auto fail = [&] { rollback(db); return false; };
    if (!exec(db, "DELETE FROM location;DELETE FROM base_location;"
                  "INSERT INTO base_location(id,path) VALUES(1,'iPod_Control/Music')",
              error))
        return fail();
    Statement location;
    if (!location.prepare(
            db,
            "INSERT INTO location(item_pid,sub_id,base_location_id,location_type,"
            "location,extension,kind_id,date_created,file_size) "
            "VALUES(?,0,1,1179208773,?,?,?,?,?)",
            error))
        return fail();
    for (const Track& track : library.tracks) {
        const std::string relative = relativeLocation(track.location);
        if (!track.dbid || relative.empty()) {
            if (error) *error = "A track has no usable device location";
            return fail();
        }
        sqlite3_bind_int64(location.get(), 1, sqlite3_int64(track.dbid));
        bindText(location.get(), 2, relative);
        sqlite3_bind_int64(location.get(), 3, fileMarker(track));
        sqlite3_bind_int64(location.get(), 4, track.mediaType);
        sqlite3_bind_int64(location.get(), 5, macTime(track.dateAdded));
        sqlite3_bind_int64(location.get(), 6, track.sizeBytes);
        if (!location.run(db, error)) return fail();
        location.reset();
    }
    return commit(db, error);
}

bool updateExtras(const Library& library, const fs::path& path,
                  std::string* error) {
    Database database;
    if (!database.open(path, error)) return false;
    sqlite3* db = database.get();
    if (!requireTables(db, {"chapter"}, "Extras.itdb", error) ||
        !begin(db, error))
        return false;
    auto fail = [&] { rollback(db); return false; };
    if (!exec(db, "CREATE TEMP TABLE podbox_items(pid INTEGER PRIMARY KEY)",
              error))
        return fail();
    Statement insert;
    if (!insert.prepare(db, "INSERT INTO podbox_items VALUES(?)", error))
        return fail();
    for (const Track& track : library.tracks) {
        sqlite3_bind_int64(insert.get(), 1, sqlite3_int64(track.dbid));
        if (!insert.run(db, error)) return fail();
        insert.reset();
    }
    for (const char* table : {"chapter", "lyrics"})
        if (tableExists(db, table) &&
            !exec(db, std::string("DELETE FROM ") + table +
                          " WHERE item_pid NOT IN (SELECT pid FROM podbox_items)",
                  error))
            return fail();
    return commit(db, error);
}

std::vector<std::uint8_t> locationCbk(const fs::path& locations,
                                      const std::vector<std::uint8_t>& uuid,
                                      const std::vector<std::uint8_t>& nonce) {
    std::ifstream input(locations, std::ios::binary);
    if (!input) return {};
    std::vector<std::uint8_t> blockDigests;
    std::array<std::uint8_t, 1024> block{};
    while (input.read(reinterpret_cast<char*>(block.data()), block.size())) {
        const auto digest = sha1(block.data(), block.size());
        blockDigests.insert(blockDigests.end(), digest.begin(), digest.end());
    }
    const auto finalDigest = sha1(blockDigests.data(), blockDigests.size());
    const auto signature = hashAbSignature(finalDigest, uuid, nonce);
    if (signature.empty()) return {};
    std::vector<std::uint8_t> result = signature;
    result.insert(result.end(), finalDigest.begin(), finalDigest.end());
    result.insert(result.end(), blockDigests.begin(), blockDigests.end());
    return result;
}

bool writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes,
                std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output.write(reinterpret_cast<const char*>(bytes.data()),
                     std::streamsize(bytes.size())))
        return true;
    if (error) *error = "Could not write " + path.string();
    return false;
}

std::int64_t scalar(sqlite3* db, const char* query) {
    Statement statement;
    if (!statement.prepare(db, query, nullptr) ||
        sqlite3_step(statement.get()) != SQLITE_ROW)
        return -1;
    return sqlite3_column_int64(statement.get(), 0);
}

bool validateDb(const fs::path& path, std::initializer_list<const char*> tables,
                std::string* error, Database* database) {
    if (!database->open(path, error, true)) return false;
    if (!requireTables(database->get(), tables, path.filename().string().c_str(),
                       error))
        return false;
    Statement integrity;
    if (!integrity.prepare(database->get(), "PRAGMA integrity_check", error) ||
        sqlite3_step(integrity.get()) != SQLITE_ROW ||
        std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(integrity.get(), 0)),
                    "ok") != 0) {
        if (error) *error = path.filename().string() + " failed integrity_check";
        return false;
    }
    return true;
}

}  // namespace

bool validateItunesSqliteBundle(const Library& library, const fs::path& directory,
                                const std::vector<std::uint8_t>& uuid,
                                const std::vector<std::uint8_t>& nonce,
                                std::string* error) {
    if (uuid.size() != 8 || nonce.size() != 23) {
        if (error) *error = "Invalid hashAB UUID or nonce";
        return false;
    }
    Database dynamic, libraryDb, locations, extras, genius;
    if (!validateDb(directory / "Dynamic.itdb", {"item_stats", "container_ui"},
                    error, &dynamic) ||
        !validateDb(directory / "Library.itdb",
                    {"db_info", "item", "container", "item_to_container"},
                    error, &libraryDb) ||
        !validateDb(directory / "Locations.itdb", {"location", "base_location"},
                    error, &locations) ||
        !validateDb(directory / "Extras.itdb", {"chapter"}, error, &extras) ||
        !validateDb(directory / "Genius.itdb", {"genius_metadata"}, error,
                    &genius))
        return false;
    const std::int64_t trackCount = std::int64_t(library.tracks.size());
    if (scalar(libraryDb.get(), "SELECT count(*) FROM item") != trackCount ||
        scalar(dynamic.get(), "SELECT count(*) FROM item_stats") != trackCount ||
        scalar(locations.get(), "SELECT count(*) FROM location") != trackCount ||
        scalar(libraryDb.get(), "SELECT count(*) FROM container") !=
            std::int64_t(library.playlists.size() + 1)) {
        if (error) *error = "SQLite companion database counts do not match iTunesCDB";
        return false;
    }
    const auto expected = locationCbk(directory / "Locations.itdb", uuid, nonce);
    std::ifstream cbkFile(directory / "Locations.itdb.cbk", std::ios::binary);
    const std::vector<std::uint8_t> actual(
        std::istreambuf_iterator<char>(cbkFile), {});
    if (expected.empty() || actual != expected) {
        if (error) *error = "Locations.itdb.cbk hashAB signature does not match";
        return false;
    }
    return true;
}

bool writeItunesSqliteBundle(const Library& library,
                             const fs::path& existingDirectory,
                             const fs::path& outputDirectory,
                             const std::vector<std::uint8_t>& uuid,
                             const std::vector<std::uint8_t>& nonce,
                             std::string* error) {
    std::error_code ec;
    fs::remove_all(outputDirectory, ec);
    ec.clear();
    fs::copy(existingDirectory, outputDirectory,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        if (error) *error = "Could not stage the iTunes SQLite bundle: " + ec.message();
        return false;
    }
    if (!updateDynamic(library, outputDirectory / "Dynamic.itdb", error) ||
        !updateLibrary(library, outputDirectory / "Library.itdb", error) ||
        !updateLocations(library, outputDirectory / "Locations.itdb", error) ||
        !updateExtras(library, outputDirectory / "Extras.itdb", error)) {
        fs::remove_all(outputDirectory, ec);
        return false;
    }
    const auto cbk = locationCbk(outputDirectory / "Locations.itdb", uuid, nonce);
    if (cbk.empty() ||
        !writeBytes(outputDirectory / "Locations.itdb.cbk", cbk, error) ||
        !validateItunesSqliteBundle(library, outputDirectory, uuid, nonce, error)) {
        fs::remove_all(outputDirectory, ec);
        return false;
    }
    return true;
}

}  // namespace podbox
