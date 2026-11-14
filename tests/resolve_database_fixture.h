#pragma once

// DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A Resolve
// Project.db reduced to the tables and columns ReadResolveDatabaseTimelines
// reads, with the names and column types Resolve 21 gives them, written by
// the test that needs it. No row comes from a client's project; the grade
// bodies are the ones tests/fixtures/resolve_grade/make_bodies.py writes.

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace resolve_fixture {

struct Item {
    // V1 is 1.
    int track = 1;
    // Timeline frames, origin included (90000 at 25 i/s from 01:00:00:00).
    int64_t start = 0;
    int64_t duration = 0;
    std::string name;
    std::string path;
    // Empty: the item has no version table at all.
    std::string body;
    bool correction = true;
    std::string version = "Version 1";
    // A remote version, when `remote` is set.
    bool remote = false;
    bool remote_correction = false;
    // A group, when not empty.
    std::string group;
    bool group_correction = false;
};

struct Timeline {
    std::string name;
    std::vector<Item> items;
    // The timeline's own grade, when not empty.
    std::string body;
    bool correction = true;
};

// name -> body bytes, from bodies.txt.
inline std::map<std::string, std::string> LoadBodies(
    const std::filesystem::path& file) {
    std::map<std::string, std::string> bodies;
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        std::string bytes;
        for (size_t index = space + 1; index + 1 < line.size(); index += 2)
            bytes.push_back(static_cast<char>(
                std::stoi(line.substr(index, 2), nullptr, 16)));
        bodies[line.substr(0, space)] = bytes;
    }
    return bodies;
}

inline bool Exec(sqlite3* db, const std::string& sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) !=
        SQLITE_OK) {
        error = message != nullptr ? message : "sqlite error";
        sqlite3_free(message);
        return false;
    }
    return true;
}

inline bool Insert(sqlite3* db, const std::string& sql,
                   const std::vector<std::string>& texts,
                   const std::vector<std::pair<int, std::string>>& blobs,
                   std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    for (size_t index = 0; index < texts.size(); ++index) {
        if (texts[index] == "\x01null")
            sqlite3_bind_null(statement, static_cast<int>(index + 1));
        else
            sqlite3_bind_text(statement, static_cast<int>(index + 1),
                              texts[index].c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& blob : blobs)
        sqlite3_bind_blob(statement, blob.first, blob.second.data(),
                          static_cast<int>(blob.second.size()),
                          SQLITE_TRANSIENT);
    const bool done = sqlite3_step(statement) == SQLITE_DONE;
    if (!done) error = sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    return done;
}

inline const std::string kNull = "\x01null";

inline std::string Id(const std::string& kind, int number) {
    std::ostringstream id;
    id << kind << '-' << number;
    return id.str();
}

// GR16 -- a project configuration as SM_Config.FieldsBlob holds it: a
// count, the field SetupBA in UTF-16, its type, a byte, its size, a 1, its
// length, then 0x80 and the settings' protobuf, uncompressed: field 209 the
// colour-space-aware tools, field 36's message (past eight bytes) field 19
// the contrast S-curve, field 202 the working luminance when `luminance` is
// not -1, field 7 the colour science when `science` is not -1.
inline std::string ConfigBlob(bool aware, bool sCurve, int luminance = -1,
                              int science = -1) {
    const auto word = [](uint32_t value) {
        std::string out(4, '\0');
        for (int i = 0; i < 4; ++i)
            out[static_cast<size_t>(i)] =
                static_cast<char>((value >> (24 - 8 * i)) & 0xff);
        return out;
    };
    std::string inner = std::string("\0\0\0\2\0\0\0\3", 8);
    if (!sCurve) inner += std::string("\x98\x01\x00", 3);
    std::string message;
    // Field 7 (tag 0x38), the colour science: absent for DaVinci YRGB.
    if (science >= 0) {
        message += '\x38';
        message += static_cast<char>(science);
    }
    // Field 202 (tag 0xd0 0x0c), the working luminance: absent for HDR 1000.
    if (luminance >= 0) {
        message += "\xd0\x0c";
        message += static_cast<char>(luminance);
    }
    if (aware) message += std::string("\x88\x0d\x01", 3);
    message += "\xa2\x02";
    message += static_cast<char>(inner.size());
    message += inner;
    const std::string name("\0S\0e\0t\0u\0p\0B\0A", 14);
    const std::string data = std::string(1, '\x80') + message;
    return word(1) + word(1) + word(14) + name + word(12) +
           std::string(1, '\0') + word(static_cast<uint32_t>(data.size()) + 8) +
           word(1) + word(static_cast<uint32_t>(data.size())) + data;
}

// Adds SM_Config, with that blob, to a database Write made.
inline bool WriteConfig(const std::filesystem::path& path,
                        const std::string& blob, std::string& error) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        error = "cannot open fixture database";
        sqlite3_close(db);
        return false;
    }
    const bool ok =
        Exec(db, "create table SM_Config (FieldsBlob BLOB);", error) &&
        Insert(db, "insert into SM_Config values (?1)", {}, {{1, blob}}, error);
    sqlite3_close(db);
    return ok;
}

inline bool Write(const std::filesystem::path& path,
                  const std::vector<Timeline>& timelines, std::string& error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        error = "cannot create fixture database";
        sqlite3_close(db);
        return false;
    }
    const char* schema = R"SQL(
create table Sm2Timeline (Sm2Timeline_id uuid, DbType character varying(128),
  Name character varying, Type INTEGER, Sequence uuid);
create table Sm2SequenceContainer (Sm2SequenceContainer_id uuid,
  DbType character varying(128), Sm2Sequence_id uuid);
create table Sm2SequenceContainer_Sm2TiTrack (DbOwner uuid, DbAssociate uuid,
  DbPropertyName character varying, DbIndex INTEGER);
create table Sm2TiItem (Sm2TiItem_id uuid, DbType character varying(128),
  Name character varying, Start character varying,
  Duration character varying, MediaFilePath character varying,
  pLmVerTable uuid, Sm2TiTrack_id uuid);
create table "ListMgt::LmVersionTable" ("ListMgt::LmVersionTable_id" uuid,
  VerType character varying, pRemoteTable uuid, pActive uuid,
  Sm2MpMedia_id uuid, Sm2Sequence_id uuid, Sm2TiItem_id uuid,
  LinkedGroup uuid, Sm2Group_id uuid);
create table "ListMgt::LmVersion" ("ListMgt::LmVersion_id" uuid,
  Name character varying, HasCorrection boolean, VerType character varying,
  Body BLOB, "ListMgt::LmVersionTable_id" uuid);
create table Sm2Group (Sm2Group_id uuid, Name character varying,
  pLmVerTable uuid);
)SQL";
    bool ok = Exec(db, schema, error);
    int counter = 0;
    const auto version = [&](const std::string& table, const std::string& name,
                             bool correction, const std::string& body) {
        const std::string id = Id("version", ++counter);
        ok = ok && Insert(db,
                          "insert into \"ListMgt::LmVersion\" values "
                          "(?1, ?2, ?3, '0', ?5, ?4)",
                          {id, name, correction ? "1" : "0", table},
                          {{5, body}}, error);
        return id;
    };
    std::map<std::string, std::string> groups;
    for (size_t t = 0; ok && t < timelines.size(); ++t) {
        const Timeline& timeline = timelines[t];
        const std::string sequence = Id("sequence", ++counter);
        const std::string container = Id("container", ++counter);
        ok = ok && Insert(db,
                          "insert into Sm2Timeline values "
                          "(?1, 'Sm2Timeline', ?2, 0, ?3)",
                          {Id("timeline", ++counter), timeline.name, sequence},
                          {}, error);
        ok = ok && Insert(db,
                          "insert into Sm2SequenceContainer values "
                          "(?1, 'Sm2SequenceContainer', ?2)",
                          {container, sequence}, {}, error);
        if (!timeline.body.empty()) {
            const std::string table = Id("table", ++counter);
            const std::string active =
                version(table, "Version 1", timeline.correction, timeline.body);
            ok = ok && Insert(db,
                              "insert into \"ListMgt::LmVersionTable\" "
                              "values (?1, '1', null, ?2, null, ?3, null, "
                              "null, null)",
                              {table, active, sequence}, {}, error);
        }
        std::map<int, std::string> tracks;
        for (const Item& item : timeline.items) {
            if (tracks.count(item.track)) continue;
            tracks[item.track] = Id("track", ++counter);
            ok = ok && Insert(db,
                              "insert into Sm2SequenceContainer_Sm2TiTrack "
                              "values (?1, ?2, 'VideoTrackVec', ?3)",
                              {container, tracks[item.track],
                               std::to_string(item.track - 1)},
                              {}, error);
        }
        for (const Item& item : timeline.items) {
            const std::string itemId = Id("item", ++counter);
            std::string tableId = kNull;
            if (!item.body.empty()) {
                tableId = Id("table", ++counter);
                const std::string active =
                    version(tableId, item.version, item.correction, item.body);
                std::string remote = kNull;
                if (item.remote) {
                    remote = Id("table", ++counter);
                    const std::string remoteActive = version(
                        remote, "Version 1", item.remote_correction, item.body);
                    ok = ok && Insert(db,
                                      "insert into \"ListMgt::LmVersionTable\""
                                      " values (?1, '0', null, ?2, 'media', "
                                      "null, null, null, null)",
                                      {remote, remoteActive}, {}, error);
                }
                std::string group = kNull;
                if (!item.group.empty()) {
                    if (!groups.count(item.group)) {
                        const std::string groupId = Id("group", ++counter);
                        const std::string groupTable = Id("table", ++counter);
                        groups[item.group] = groupId;
                        version(groupTable, "Pre-Clip", item.group_correction,
                                item.body);
                        ok = ok &&
                             Insert(db,
                                    "insert into \"ListMgt::LmVersionTable\" "
                                    "values (?1, '0', null, null, null, null, "
                                    "null, null, ?2)",
                                    {groupTable, groupId}, {}, error) &&
                             Insert(
                                 db, "insert into Sm2Group values (?1, ?2, ?3)",
                                 {groupId, item.group, groupTable}, {}, error);
                    }
                    group = groups[item.group];
                }
                ok = ok && Insert(db,
                                  "insert into \"ListMgt::LmVersionTable\" "
                                  "values (?1, '0', ?2, ?3, null, null, ?4, "
                                  "?5, null)",
                                  {tableId, remote, active, itemId, group}, {},
                                  error);
            }
            ok = ok && Insert(db,
                              "insert into Sm2TiItem values "
                              "(?1, 'Sm2TiItem', ?2, ?3, ?4, ?5, ?6, ?7)",
                              {itemId, item.name, std::to_string(item.start),
                               std::to_string(item.duration), item.path,
                               tableId, tracks[item.track]},
                              {}, error);
        }
    }
    sqlite3_close(db);
    return ok;
}

}  // namespace resolve_fixture
