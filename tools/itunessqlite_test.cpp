#include "itdb/itunessqlite.h"

#include "itdb/hashab.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

bool createDb(const fs::path& path, const char* schema) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) return false;
    char* error = nullptr;
    const bool ok = sqlite3_exec(db, schema, nullptr, nullptr, &error) == SQLITE_OK;
    if (!ok) std::fprintf(stderr, "fixture SQL: %s\n", error ? error : "error");
    sqlite3_free(error);
    sqlite3_close(db);
    return ok;
}

std::int64_t scalar(const fs::path& path, const char* query) {
    sqlite3* db = nullptr;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db, query, -1, &statement, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const std::int64_t value = sqlite3_step(statement) == SQLITE_ROW
                                   ? sqlite3_column_int64(statement, 0)
                                   : -1;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

podbox::Library library() {
    podbox::Library lib;
    lib.databaseDbid = 0x1020304050607080ULL;
    lib.masterDbid = 0x0123456789ABCDEFULL;
    lib.masterName = "Test nano";
    lib.version = 0x2A;
    lib.hashingScheme = podbox::kChecksumHashAB;
    lib.compressed = true;
    podbox::Track first;
    first.id = 1;
    first.dbid = 0x1111111111111111ULL;
    first.title = "First";
    first.artist = "Artist";
    first.album = "Album";
    first.genre = "Rock";
    first.composer = "Composer";
    first.location = ":iPod_Control:Music:F00:AAAA.m4a";
    first.lengthMs = 123000;
    first.sizeBytes = 12345;
    first.bitrate = 256;
    first.sampleRate = 44100;
    first.playCount = 3;
    first.rating = 80;
    lib.tracks.push_back(first);
    podbox::Track second = first;
    second.id = 2;
    second.dbid = 0x2222222222222222ULL;
    second.title = "Second";
    second.location = ":iPod_Control:Music:F01:BBBB.mp3";
    lib.tracks.push_back(second);
    podbox::Playlist playlist;
    playlist.dbid = 0x3333333333333333ULL;
    playlist.name = "Favourites";
    playlist.trackIds = {2, 1};
    lib.playlists.push_back(playlist);
    return lib;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("podbox-sqlite-test-" + std::to_string(std::random_device{}()));
    const fs::path source = root / "source.itlp";
    const fs::path output = root / "output.itlp";
    const fs::path output2 = root / "output2.itlp";
    fs::create_directories(source);

    expect(createDb(source / "Dynamic.itdb",
        "CREATE TABLE item_stats(item_pid INTEGER PRIMARY KEY,has_been_played INTEGER DEFAULT 0,date_played INTEGER DEFAULT 0,play_count_user INTEGER DEFAULT 0,play_count_recent INTEGER DEFAULT 0,date_skipped INTEGER DEFAULT 0,skip_count_user INTEGER DEFAULT 0,skip_count_recent INTEGER DEFAULT 0,bookmark_time_ms REAL,bookmark_time_ms_common REAL,user_rating INTEGER DEFAULT 0,user_rating_common INTEGER DEFAULT 0,rental_expired INTEGER DEFAULT 0,hidden INTEGER DEFAULT 0,deleted INTEGER DEFAULT 0,has_changes INTEGER DEFAULT 0);"
        "CREATE TABLE container_ui(container_pid INTEGER PRIMARY KEY,play_order INTEGER DEFAULT 0,is_reversed INTEGER DEFAULT 0,album_field_order INTEGER DEFAULT 0,repeat_mode INTEGER DEFAULT 0,shuffle_items INTEGER DEFAULT 0,has_been_shuffled INTEGER DEFAULT 0);"),
        "create Dynamic fixture");
    expect(createDb(source / "Library.itdb",
        "CREATE TABLE db_info(pid INTEGER PRIMARY KEY,primary_container_pid INTEGER);"
        "INSERT INTO db_info VALUES(1161981756646125696,81985529216486895);"
        "CREATE TABLE item(pid INTEGER PRIMARY KEY,media_kind INTEGER,date_modified INTEGER,year INTEGER,remember_bookmark INTEGER,exclude_from_shuffle INTEGER,artwork_status INTEGER,start_time_ms REAL,stop_time_ms REAL,total_time_ms REAL,track_number INTEGER,disc_number INTEGER,genre_id INTEGER,album_pid INTEGER,artist_pid INTEGER,composer_pid INTEGER,title TEXT,artist TEXT,album TEXT,composer TEXT,sort_title TEXT,sort_artist TEXT,sort_album TEXT,sort_composer TEXT,title_order INTEGER,artist_order INTEGER,album_order INTEGER,genre_order INTEGER,composer_order INTEGER);"
        "CREATE TABLE avformat_info(item_pid INTEGER,sub_id INTEGER,audio_format INTEGER,bit_rate INTEGER,sample_rate REAL,duration INTEGER,gapless_heuristic_info INTEGER,gapless_encoding_delay INTEGER,gapless_encoding_drain INTEGER,gapless_last_frame_resynch INTEGER,analysis_inhibit_flags INTEGER,audio_fingerprint INTEGER,volume_normalization_energy INTEGER,PRIMARY KEY(item_pid,sub_id));"
        "CREATE TABLE container(pid INTEGER PRIMARY KEY,distinguished_kind INTEGER,date_created INTEGER,date_modified INTEGER,name TEXT,name_order INTEGER,parent_pid INTEGER,media_kinds INTEGER,workout_template_id INTEGER,is_hidden INTEGER,smart_is_folder INTEGER);"
        "CREATE TABLE item_to_container(item_pid INTEGER,container_pid INTEGER,physical_order INTEGER,shuffle_order INTEGER);"
        "CREATE TABLE genre_map(id INTEGER PRIMARY KEY,genre TEXT UNIQUE,genre_order INTEGER);"
        "CREATE TABLE location_kind_map(id INTEGER PRIMARY KEY,kind TEXT UNIQUE);"
        "CREATE TABLE album(pid INTEGER PRIMARY KEY,kind INTEGER,artwork_status INTEGER,artwork_item_pid INTEGER,artist_pid INTEGER,user_rating INTEGER,name TEXT,name_order INTEGER,all_compilations INTEGER,feed_url TEXT,season_number INTEGER);"
        "CREATE TABLE artist(pid INTEGER PRIMARY KEY,kind INTEGER,artwork_status INTEGER,artwork_album_pid INTEGER,name TEXT,name_order INTEGER,sort_name TEXT);"
        "CREATE TABLE composer(pid INTEGER PRIMARY KEY,name TEXT,name_order INTEGER,sort_name TEXT);"
        "CREATE TABLE device_extension(marker TEXT);INSERT INTO device_extension VALUES('preserve me');"
        "CREATE TABLE ext_item_sort(item_pid INTEGER PRIMARY KEY,sort_key BLOB);"
        "CREATE TRIGGER device_item_insert AFTER INSERT ON item BEGIN INSERT INTO ext_item_sort VALUES(new.pid,iPhoneSortKey(new.title));END;"
        "CREATE TRIGGER device_item_delete AFTER DELETE ON item BEGIN DELETE FROM ext_item_sort WHERE item_pid=old.pid;END;"),
        "create Library fixture");
    expect(createDb(source / "Locations.itdb",
        "CREATE TABLE location(item_pid INTEGER,sub_id INTEGER,base_location_id INTEGER,location_type INTEGER,location TEXT,extension INTEGER,kind_id INTEGER,date_created INTEGER,file_size INTEGER,PRIMARY KEY(item_pid,sub_id));"
        "CREATE TABLE base_location(id INTEGER PRIMARY KEY,path TEXT);"),
        "create Locations fixture");
    expect(createDb(source / "Extras.itdb", "CREATE TABLE chapter(item_pid INTEGER PRIMARY KEY,data BLOB);"),
           "create Extras fixture");
    expect(createDb(source / "Genius.itdb", "CREATE TABLE genius_metadata(genius_id INTEGER PRIMARY KEY,version INTEGER,data BLOB);"),
           "create Genius fixture");
    std::ofstream(source / "Locations.itdb.cbk", std::ios::binary);

    const auto uuid = std::vector<std::uint8_t>{0xF8,0x32,0xC6,0x59,0x17,0xDA,0x67,0x85};
    const auto nonce = podbox::hashAbDefaultNonce();
    podbox::Library lib = library();
    std::string error;
    expect(podbox::writeItunesSqliteBundle(lib, source, output, uuid, nonce, &error),
           error.empty() ? "write SQLite bundle" : error);
    expect(podbox::validateItunesSqliteBundle(lib, output, uuid, nonce, &error),
           error.empty() ? "validate SQLite bundle" : error);
    expect(scalar(output / "Library.itdb", "SELECT count(*) FROM item") == 2,
           "writes two library items");
    expect(scalar(output / "Library.itdb", "SELECT count(*) FROM item_to_container") == 4,
           "writes master and playlist membership");
    expect(scalar(output / "Dynamic.itdb", "SELECT play_count_user FROM item_stats WHERE item_pid=1229782938247303441") == 3,
           "writes dynamic play count");
    expect(scalar(output / "Library.itdb", "SELECT count(*) FROM device_extension") == 1,
           "preserves device-specific schema and data");
    expect(scalar(output / "Library.itdb", "SELECT count(*) FROM ext_item_sort WHERE length(sort_key)>4") == 2,
           "runs preserved nano sort-key triggers for new items");

    lib.playlists[0].name = "Renamed";
    error.clear();
    expect(podbox::writeItunesSqliteBundle(lib, output, output2, uuid, nonce, &error),
           error.empty() ? "rewrite staged bundle" : error);
    expect(scalar(output2 / "Library.itdb", "SELECT count(*) FROM device_extension") == 1,
           "preserves extension across another write");

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) return 1;
    std::printf("iTunes SQLite tests passed\n");
    return 0;
}
