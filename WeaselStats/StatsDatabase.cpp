#include "StatsDatabase.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "TextMetrics.h"

namespace weasel::stats {
namespace {

class Statement {
 public:
  Statement(WinSqlite& sqlite, sqlite3* database, const char* sql)
      : sqlite_(sqlite) {
    if (sqlite_.prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK) {
      statement_ = nullptr;
    }
  }

  ~Statement() {
    if (statement_) {
      sqlite_.finalize(statement_);
    }
  }

  sqlite3_stmt* get() const { return statement_; }

 private:
  WinSqlite& sqlite_;
  sqlite3_stmt* statement_ = nullptr;
};

bool BindText(WinSqlite& sqlite,
              sqlite3_stmt* statement,
              int index,
              std::string_view value) {
  if (value.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return sqlite.bind_text(statement, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) == SQLITE_OK;
}

bool BindInteger(WinSqlite& sqlite,
                 sqlite3_stmt* statement,
                 int index,
                 std::uint64_t value) {
  if (value >
      static_cast<std::uint64_t>((std::numeric_limits<sqlite3_int64>::max)())) {
    return false;
  }
  return sqlite.bind_int64(statement, index,
                           static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool ExecuteSql(WinSqlite& sqlite, sqlite3* database, const char* sql) {
  char* error = nullptr;
  const int result = sqlite.exec(database, sql, nullptr, nullptr, &error);
  if (error) {
    sqlite.free(error);
  }
  return result == SQLITE_OK;
}

bool ReadInteger(WinSqlite& sqlite,
                 sqlite3* database,
                 const char* sql,
                 sqlite3_int64& value) {
  Statement statement(sqlite, database, sql);
  if (!statement.get() || sqlite.step(statement.get()) != SQLITE_ROW) {
    return false;
  }
  value = sqlite.column_int64(statement.get(), 0);
  return true;
}

bool ReadText(WinSqlite& sqlite,
              sqlite3* database,
              const char* sql,
              std::string& value) {
  Statement statement(sqlite, database, sql);
  if (!statement.get() || sqlite.step(statement.get()) != SQLITE_ROW) {
    return false;
  }
  const unsigned char* text = sqlite.column_text(statement.get(), 0);
  if (!text) {
    return false;
  }
  value = reinterpret_cast<const char*>(text);
  return true;
}

std::string MakeGenerationId() {
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);
  const ULONGLONG file_time =
      (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  char value[64] = {};
  sprintf_s(value, "%08lx-%016llx-%016llx", GetCurrentProcessId(), file_time,
            GetTickCount64());
  return value;
}

class TemporarySnapshot {
 public:
  TemporarySnapshot(std::filesystem::path directory,
                    std::filesystem::path file)
      : directory_(std::move(directory)), file_(std::move(file)) {}

  ~TemporarySnapshot() {
    std::error_code error;
    std::filesystem::remove(file_, error);
    error.clear();
    std::filesystem::remove(directory_, error);
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path file_;
};

}  // namespace

StatsDatabase::~StatsDatabase() {
  if (database_) {
    sqlite_.close(database_);
  }
}

bool StatsDatabase::Open(const std::filesystem::path& path) {
  if (!sqlite_.available() || database_) {
    return false;
  }
  const std::string utf8_path = path.u8string();
  if (sqlite_.open_v2(
          utf8_path.c_str(), &database_,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
          nullptr) != SQLITE_OK) {
    if (database_) {
      sqlite_.close(database_);
      database_ = nullptr;
    }
    return false;
  }
  sqlite_.busy_timeout(database_, 1000);
  sqlite3_int64 schema_version = 0;
  if (!ReadInteger(sqlite_, database_, "PRAGMA user_version;", schema_version) ||
      schema_version < 0 || schema_version > 2 ||
      !Execute("PRAGMA journal_mode=WAL;") ||
      !Execute("PRAGMA synchronous=NORMAL;")) {
    return false;
  }

  if (schema_version == 0) {
    sqlite3_int64 existing_totals = 0;
    if (!ReadInteger(sqlite_, database_,
                     "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                     "AND name='daily_totals';",
                     existing_totals)) {
      return false;
    }
    if (existing_totals ? !MigrateSchema1() : !CreateSchema()) {
      return false;
    }
  } else if (schema_version == 1) {
    if (!MigrateSchema1()) {
      return false;
    }
  } else if (!ValidateSchema2()) {
    return false;
  }

  database_path_ = path;
  return InitializeGeneration();
}

bool StatsDatabase::Execute(const char* sql) {
  return ExecuteSql(sqlite_, database_, sql);
}

bool StatsDatabase::CreateSchema() {
  if (!Execute("BEGIN IMMEDIATE;")) {
    return false;
  }
  const bool success = Execute(
      "CREATE TABLE meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
      "INSERT INTO meta(key,value) VALUES('revision',0);"
      "CREATE TABLE local_state(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
      "CREATE TABLE processed_sessions("
      "server_id TEXT PRIMARY KEY,last_sequence INTEGER NOT NULL);"
      "CREATE TABLE daily_totals("
      "device_id TEXT NOT NULL,generation_id TEXT NOT NULL,"
      "day INTEGER NOT NULL,han_characters INTEGER NOT NULL,"
      "english_words INTEGER NOT NULL,commits INTEGER NOT NULL,"
      "backspaces INTEGER NOT NULL,deleted_ascii_letters INTEGER NOT NULL,"
      "revision INTEGER NOT NULL,"
      "PRIMARY KEY(device_id,generation_id,day));"
      "CREATE INDEX daily_totals_day ON daily_totals(day);"
      "CREATE TABLE daily_terms("
      "device_id TEXT NOT NULL,generation_id TEXT NOT NULL,"
      "day INTEGER NOT NULL,term TEXT NOT NULL,count INTEGER NOT NULL,"
      "revision INTEGER NOT NULL,"
      "PRIMARY KEY(device_id,generation_id,day,term));"
      "CREATE INDEX daily_terms_day_count ON daily_terms(day,count DESC);"
      "PRAGMA user_version=2;");
  if (!success || !Execute("COMMIT;")) {
    Execute("ROLLBACK;");
    return false;
  }
  return true;
}

bool StatsDatabase::MigrateSchema1() {
  if (!Execute("BEGIN IMMEDIATE;")) {
    return false;
  }
  const bool success = Execute(
      "DROP INDEX IF EXISTS daily_totals_day;"
      "DROP INDEX IF EXISTS daily_terms_day_count;"
      "ALTER TABLE daily_totals RENAME TO daily_totals_schema1;"
      "ALTER TABLE daily_terms RENAME TO daily_terms_schema1;"
      "CREATE TABLE daily_totals("
      "device_id TEXT NOT NULL,generation_id TEXT NOT NULL,"
      "day INTEGER NOT NULL,han_characters INTEGER NOT NULL,"
      "english_words INTEGER NOT NULL,commits INTEGER NOT NULL,"
      "backspaces INTEGER NOT NULL,deleted_ascii_letters INTEGER NOT NULL,"
      "revision INTEGER NOT NULL,"
      "PRIMARY KEY(device_id,generation_id,day));"
      "INSERT INTO daily_totals "
      "SELECT device_id,'legacy',day,han_characters,english_words,commits,"
      "backspaces,deleted_ascii_letters,revision FROM daily_totals_schema1;"
      "CREATE INDEX daily_totals_day ON daily_totals(day);"
      "CREATE TABLE daily_terms("
      "device_id TEXT NOT NULL,generation_id TEXT NOT NULL,"
      "day INTEGER NOT NULL,term TEXT NOT NULL,count INTEGER NOT NULL,"
      "revision INTEGER NOT NULL,"
      "PRIMARY KEY(device_id,generation_id,day,term));"
      "INSERT INTO daily_terms "
      "SELECT device_id,'legacy',day,term,count,revision "
      "FROM daily_terms_schema1;"
      "CREATE INDEX daily_terms_day_count ON daily_terms(day,count DESC);"
      "DROP TABLE daily_totals_schema1;"
      "DROP TABLE daily_terms_schema1;"
      "CREATE TABLE IF NOT EXISTS local_state("
      "key TEXT PRIMARY KEY,value TEXT NOT NULL);"
      "PRAGMA user_version=2;");
  if (!success || !Execute("COMMIT;")) {
    Execute("ROLLBACK;");
    return false;
  }
  return ValidateSchema2();
}

bool StatsDatabase::ValidateSchema2() {
  return Execute(
      "SELECT key,value FROM meta LIMIT 0;"
      "SELECT key,value FROM local_state LIMIT 0;"
      "SELECT server_id,last_sequence FROM processed_sessions LIMIT 0;"
      "SELECT device_id,generation_id,day,han_characters,english_words,"
      "commits,backspaces,deleted_ascii_letters,revision "
      "FROM daily_totals LIMIT 0;"
      "SELECT device_id,generation_id,day,term,count,revision "
      "FROM daily_terms LIMIT 0;");
}

bool StatsDatabase::InitializeGeneration() {
  Statement query(sqlite_, database_,
                  "SELECT value FROM local_state WHERE key='generation_id';");
  if (!query.get()) {
    return false;
  }
  const int query_result = sqlite_.step(query.get());
  if (query_result == SQLITE_ROW) {
    const unsigned char* value = sqlite_.column_text(query.get(), 0);
    if (!value || !*value) {
      return false;
    }
    generation_id_ = reinterpret_cast<const char*>(value);
    return true;
  }
  if (query_result != SQLITE_DONE) {
    return false;
  }

  generation_id_ = MakeGenerationId();
  Statement insert(
      sqlite_, database_,
      "INSERT INTO local_state(key,value) VALUES('generation_id',?1);");
  return insert.get() &&
         BindText(sqlite_, insert.get(), 1, generation_id_) &&
         sqlite_.step(insert.get()) == SQLITE_DONE;
}

bool StatsDatabase::BeginEvent(std::string_view server_id,
                               std::uint64_t sequence,
                               bool& duplicate) {
  duplicate = false;
  if (!Execute("BEGIN IMMEDIATE;")) {
    return false;
  }

  Statement insert(sqlite_, database_,
                   "INSERT OR IGNORE INTO processed_sessions(server_id,"
                   "last_sequence) VALUES(?1,0);");
  if (!insert.get() || !BindText(sqlite_, insert.get(), 1, server_id) ||
      sqlite_.step(insert.get()) != SQLITE_DONE) {
    Execute("ROLLBACK;");
    return false;
  }

  Statement query(sqlite_, database_,
                  "SELECT last_sequence FROM processed_sessions "
                  "WHERE server_id=?1;");
  if (!query.get() || !BindText(sqlite_, query.get(), 1, server_id) ||
      sqlite_.step(query.get()) != SQLITE_ROW) {
    Execute("ROLLBACK;");
    return false;
  }
  const auto last_sequence =
      static_cast<std::uint64_t>(sqlite_.column_int64(query.get(), 0));
  if (sequence <= last_sequence) {
    duplicate = true;
    return Execute("COMMIT;");
  }

  Statement update(sqlite_, database_,
                   "UPDATE processed_sessions SET last_sequence=?1 "
                   "WHERE server_id=?2;");
  if (!update.get() || !BindInteger(sqlite_, update.get(), 1, sequence) ||
      !BindText(sqlite_, update.get(), 2, server_id) ||
      sqlite_.step(update.get()) != SQLITE_DONE) {
    Execute("ROLLBACK;");
    return false;
  }
  return true;
}

bool StatsDatabase::AdvanceRevision(std::uint64_t& revision) {
  if (!Execute("UPDATE meta SET value=value+1 WHERE key='revision';")) {
    return false;
  }
  Statement query(sqlite_, database_,
                  "SELECT value FROM meta WHERE key='revision';");
  if (!query.get() || sqlite_.step(query.get()) != SQLITE_ROW) {
    return false;
  }
  revision = static_cast<std::uint64_t>(sqlite_.column_int64(query.get(), 0));
  return true;
}

bool StatsDatabase::UpdateDailyTotals(std::string_view device_id,
                                      std::uint32_t day,
                                      std::uint64_t han_characters,
                                      std::uint64_t english_words,
                                      std::uint64_t commits,
                                      std::uint64_t backspaces,
                                      std::uint64_t deleted_ascii_letters,
                                      std::uint64_t revision) {
  Statement insert(
      sqlite_, database_,
      "INSERT OR IGNORE INTO daily_totals(device_id,generation_id,day,"
      "han_characters,english_words,commits,backspaces,"
      "deleted_ascii_letters,revision) VALUES(?1,?2,?3,0,0,0,0,0,0);");
  if (!insert.get() || !BindText(sqlite_, insert.get(), 1, device_id) ||
      !BindText(sqlite_, insert.get(), 2, generation_id_) ||
      !BindInteger(sqlite_, insert.get(), 3, day) ||
      sqlite_.step(insert.get()) != SQLITE_DONE) {
    return false;
  }

  Statement update(sqlite_, database_,
                   "UPDATE daily_totals SET han_characters=han_characters+?1,"
                   "english_words=english_words+?2,commits=commits+?3,"
                   "backspaces=backspaces+?4,"
                   "deleted_ascii_letters=deleted_ascii_letters+?5,revision=?6 "
                   "WHERE device_id=?7 AND generation_id=?8 AND day=?9;");
  return update.get() &&
         BindInteger(sqlite_, update.get(), 1, han_characters) &&
         BindInteger(sqlite_, update.get(), 2, english_words) &&
         BindInteger(sqlite_, update.get(), 3, commits) &&
         BindInteger(sqlite_, update.get(), 4, backspaces) &&
         BindInteger(sqlite_, update.get(), 5, deleted_ascii_letters) &&
         BindInteger(sqlite_, update.get(), 6, revision) &&
         BindText(sqlite_, update.get(), 7, device_id) &&
         BindText(sqlite_, update.get(), 8, generation_id_) &&
         BindInteger(sqlite_, update.get(), 9, day) &&
         sqlite_.step(update.get()) == SQLITE_DONE;
}

bool StatsDatabase::UpdateTerm(std::string_view device_id,
                               std::uint32_t day,
                               std::string_view text,
                               std::uint64_t revision) {
  Statement insert(
      sqlite_, database_,
      "INSERT OR IGNORE INTO daily_terms(device_id,generation_id,day,term,"
      "count,revision) VALUES(?1,?2,?3,?4,0,0);");
  if (!insert.get() || !BindText(sqlite_, insert.get(), 1, device_id) ||
      !BindText(sqlite_, insert.get(), 2, generation_id_) ||
      !BindInteger(sqlite_, insert.get(), 3, day) ||
      !BindText(sqlite_, insert.get(), 4, text) ||
      sqlite_.step(insert.get()) != SQLITE_DONE) {
    return false;
  }

  Statement update(sqlite_, database_,
                   "UPDATE daily_terms SET count=count+1,revision=?1 "
                   "WHERE device_id=?2 AND generation_id=?3 AND day=?4 "
                   "AND term=?5;");
  return update.get() && BindInteger(sqlite_, update.get(), 1, revision) &&
         BindText(sqlite_, update.get(), 2, device_id) &&
         BindText(sqlite_, update.get(), 3, generation_id_) &&
         BindInteger(sqlite_, update.get(), 4, day) &&
         BindText(sqlite_, update.get(), 5, text) &&
         sqlite_.step(update.get()) == SQLITE_DONE;
}

bool StatsDatabase::RecordCommit(std::string_view server_id,
                                 std::uint64_t sequence,
                                 std::string_view device_id,
                                 std::uint32_t day,
                                 std::string_view text,
                                 Response& response) {
  bool duplicate = false;
  if (!BeginEvent(server_id, sequence, duplicate)) {
    return false;
  }
  if (!duplicate) {
    const TextMetrics metrics = CountTextMetrics(text);
    std::uint64_t revision = 0;
    if (!AdvanceRevision(revision) ||
        !UpdateDailyTotals(device_id, day, metrics.han_characters,
                           metrics.english_words, 1, 0, 0, revision) ||
        !UpdateTerm(device_id, day, text, revision) || !Execute("COMMIT;")) {
      Execute("ROLLBACK;");
      return false;
    }
  }
  return GetSummary(day, response);
}

bool StatsDatabase::RecordCorrection(std::string_view server_id,
                                     std::uint64_t sequence,
                                     std::string_view device_id,
                                     std::uint32_t day,
                                     std::uint32_t backspaces,
                                     std::uint32_t deleted_ascii_letters,
                                     Response& response) {
  bool duplicate = false;
  if (!BeginEvent(server_id, sequence, duplicate)) {
    return false;
  }
  if (!duplicate) {
    std::uint64_t revision = 0;
    if (!AdvanceRevision(revision) ||
        !UpdateDailyTotals(device_id, day, 0, 0, 0, backspaces,
                           deleted_ascii_letters, revision) ||
        !Execute("COMMIT;")) {
      Execute("ROLLBACK;");
      return false;
    }
  }
  return GetSummary(day, response);
}

bool StatsDatabase::IsSyncInitialized(bool& initialized) {
  initialized = false;
  Statement query(
      sqlite_, database_,
      "SELECT value FROM local_state WHERE key='sync_initialized';");
  if (!query.get()) {
    return false;
  }
  const int result = sqlite_.step(query.get());
  if (result == SQLITE_DONE) {
    return true;
  }
  if (result != SQLITE_ROW) {
    return false;
  }
  const unsigned char* value = sqlite_.column_text(query.get(), 0);
  if (!value) {
    return false;
  }
  initialized = strcmp(reinterpret_cast<const char*>(value), "1") == 0;
  return true;
}

bool StatsDatabase::SetSyncInitialized() {
  Statement statement(
      sqlite_, database_,
      "INSERT OR REPLACE INTO local_state(key,value) "
      "VALUES('sync_initialized','1');");
  return statement.get() && sqlite_.step(statement.get()) == SQLITE_DONE;
}

bool StatsDatabase::ValidateSnapshot(const std::filesystem::path& path,
                                     int& schema_version) {
  schema_version = 0;
  sqlite3* snapshot = nullptr;
  const std::string utf8_path = path.u8string();
  if (sqlite_.open_v2(utf8_path.c_str(), &snapshot,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (snapshot) {
      sqlite_.close(snapshot);
    }
    return false;
  }

  sqlite_.busy_timeout(snapshot, 1000);
  sqlite3_int64 version = 0;
  std::string quick_check;
  bool valid =
      ReadInteger(sqlite_, snapshot, "PRAGMA user_version;", version) &&
      (version == 1 || version == 2) &&
      ReadText(sqlite_, snapshot, "PRAGMA quick_check;", quick_check) &&
      quick_check == "ok";
  if (valid) {
    const char* schema_check =
        version == 2
            ? "SELECT device_id,generation_id,day,han_characters,"
              "english_words,commits,backspaces,deleted_ascii_letters,"
              "revision FROM daily_totals LIMIT 0;"
              "SELECT device_id,generation_id,day,term,count,revision "
              "FROM daily_terms LIMIT 0;"
            : "SELECT device_id,day,han_characters,english_words,commits,"
              "backspaces,deleted_ascii_letters,revision "
              "FROM daily_totals LIMIT 0;"
              "SELECT device_id,day,term,count,revision "
              "FROM daily_terms LIMIT 0;";
    valid = ExecuteSql(sqlite_, snapshot, schema_check);
  }

  sqlite3_int64 invalid_rows = 0;
  if (valid) {
    const char* row_check =
        version == 2
            ? "SELECT "
              "(SELECT COUNT(*) FROM daily_totals WHERE "
              "typeof(device_id)<>'text' OR length(device_id)=0 OR "
              "typeof(generation_id)<>'text' OR length(generation_id)=0 OR "
              "typeof(day)<>'integer' OR day<=0 OR "
              "typeof(han_characters)<>'integer' OR han_characters<0 OR "
              "typeof(english_words)<>'integer' OR english_words<0 OR "
              "typeof(commits)<>'integer' OR commits<0 OR "
              "typeof(backspaces)<>'integer' OR backspaces<0 OR "
              "typeof(deleted_ascii_letters)<>'integer' OR "
              "deleted_ascii_letters<0 OR typeof(revision)<>'integer' OR "
              "revision<0) + "
              "(SELECT COUNT(*) FROM daily_terms WHERE "
              "typeof(device_id)<>'text' OR length(device_id)=0 OR "
              "typeof(generation_id)<>'text' OR length(generation_id)=0 OR "
              "typeof(day)<>'integer' OR day<=0 OR typeof(term)<>'text' OR "
              "length(term)=0 OR length(CAST(term AS BLOB))>=4096 OR "
              "typeof(count)<>'integer' OR count<0 OR "
              "typeof(revision)<>'integer' OR revision<0);"
            : "SELECT "
              "(SELECT COUNT(*) FROM daily_totals WHERE "
              "typeof(device_id)<>'text' OR length(device_id)=0 OR "
              "typeof(day)<>'integer' OR day<=0 OR "
              "typeof(han_characters)<>'integer' OR han_characters<0 OR "
              "typeof(english_words)<>'integer' OR english_words<0 OR "
              "typeof(commits)<>'integer' OR commits<0 OR "
              "typeof(backspaces)<>'integer' OR backspaces<0 OR "
              "typeof(deleted_ascii_letters)<>'integer' OR "
              "deleted_ascii_letters<0 OR typeof(revision)<>'integer' OR "
              "revision<0) + "
              "(SELECT COUNT(*) FROM daily_terms WHERE "
              "typeof(device_id)<>'text' OR length(device_id)=0 OR "
              "typeof(day)<>'integer' OR day<=0 OR typeof(term)<>'text' OR "
              "length(term)=0 OR length(CAST(term AS BLOB))>=4096 OR "
              "typeof(count)<>'integer' OR count<0 OR "
              "typeof(revision)<>'integer' OR revision<0);";
    valid = ReadInteger(sqlite_, snapshot, row_check, invalid_rows) &&
            invalid_rows == 0;
  }

  const bool closed = sqlite_.close(snapshot) == SQLITE_OK;
  if (valid && closed) {
    schema_version = static_cast<int>(version);
    return true;
  }
  return false;
}

bool StatsDatabase::MergeSnapshot(const std::filesystem::path& path,
                                  int schema_version) {
  const std::string utf8_path = path.u8string();
  {
    Statement attach(sqlite_, database_,
                     "ATTACH DATABASE ?1 AS incoming;");
    if (!attach.get() || !BindText(sqlite_, attach.get(), 1, utf8_path) ||
        sqlite_.step(attach.get()) != SQLITE_DONE) {
      return false;
    }
  }

  if (!Execute("BEGIN IMMEDIATE;")) {
    Execute("DETACH DATABASE incoming;");
    return false;
  }

  const char* totals_sql =
      schema_version == 2
          ? "INSERT OR REPLACE INTO main.daily_totals("
            "device_id,generation_id,day,han_characters,english_words,"
            "commits,backspaces,deleted_ascii_letters,revision) "
            "SELECT src.device_id,src.generation_id,src.day,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.han_characters>dst.han_characters THEN src.han_characters "
            "ELSE dst.han_characters END,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.english_words>dst.english_words THEN src.english_words "
            "ELSE dst.english_words END,"
            "CASE WHEN dst.device_id IS NULL OR src.commits>dst.commits "
            "THEN src.commits ELSE dst.commits END,"
            "CASE WHEN dst.device_id IS NULL OR src.backspaces>dst.backspaces "
            "THEN src.backspaces ELSE dst.backspaces END,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.deleted_ascii_letters>dst.deleted_ascii_letters "
            "THEN src.deleted_ascii_letters ELSE dst.deleted_ascii_letters END,"
            "CASE WHEN dst.device_id IS NULL OR src.revision>dst.revision "
            "THEN src.revision ELSE dst.revision END "
            "FROM incoming.daily_totals AS src "
            "LEFT JOIN main.daily_totals AS dst ON "
            "dst.device_id=src.device_id AND "
            "dst.generation_id=src.generation_id AND dst.day=src.day;"
          : "INSERT OR REPLACE INTO main.daily_totals("
            "device_id,generation_id,day,han_characters,english_words,"
            "commits,backspaces,deleted_ascii_letters,revision) "
            "SELECT src.device_id,'legacy',src.day,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.han_characters>dst.han_characters THEN src.han_characters "
            "ELSE dst.han_characters END,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.english_words>dst.english_words THEN src.english_words "
            "ELSE dst.english_words END,"
            "CASE WHEN dst.device_id IS NULL OR src.commits>dst.commits "
            "THEN src.commits ELSE dst.commits END,"
            "CASE WHEN dst.device_id IS NULL OR src.backspaces>dst.backspaces "
            "THEN src.backspaces ELSE dst.backspaces END,"
            "CASE WHEN dst.device_id IS NULL OR "
            "src.deleted_ascii_letters>dst.deleted_ascii_letters "
            "THEN src.deleted_ascii_letters ELSE dst.deleted_ascii_letters END,"
            "CASE WHEN dst.device_id IS NULL OR src.revision>dst.revision "
            "THEN src.revision ELSE dst.revision END "
            "FROM incoming.daily_totals AS src "
            "LEFT JOIN main.daily_totals AS dst ON "
            "dst.device_id=src.device_id AND dst.generation_id='legacy' "
            "AND dst.day=src.day;";
  const char* terms_sql =
      schema_version == 2
          ? "INSERT OR REPLACE INTO main.daily_terms("
            "device_id,generation_id,day,term,count,revision) "
            "SELECT src.device_id,src.generation_id,src.day,src.term,"
            "CASE WHEN dst.device_id IS NULL OR src.count>dst.count "
            "THEN src.count ELSE dst.count END,"
            "CASE WHEN dst.device_id IS NULL OR src.revision>dst.revision "
            "THEN src.revision ELSE dst.revision END "
            "FROM incoming.daily_terms AS src "
            "LEFT JOIN main.daily_terms AS dst ON "
            "dst.device_id=src.device_id AND "
            "dst.generation_id=src.generation_id AND dst.day=src.day "
            "AND dst.term=src.term;"
          : "INSERT OR REPLACE INTO main.daily_terms("
            "device_id,generation_id,day,term,count,revision) "
            "SELECT src.device_id,'legacy',src.day,src.term,"
            "CASE WHEN dst.device_id IS NULL OR src.count>dst.count "
            "THEN src.count ELSE dst.count END,"
            "CASE WHEN dst.device_id IS NULL OR src.revision>dst.revision "
            "THEN src.revision ELSE dst.revision END "
            "FROM incoming.daily_terms AS src "
            "LEFT JOIN main.daily_terms AS dst ON "
            "dst.device_id=src.device_id AND dst.generation_id='legacy' "
            "AND dst.day=src.day AND dst.term=src.term;";
  const bool merged =
      Execute(totals_sql) && Execute(terms_sql) &&
      Execute(
          "UPDATE meta SET value=max("
          "value,COALESCE((SELECT MAX(revision) FROM daily_totals),0),"
          "COALESCE((SELECT MAX(revision) FROM daily_terms),0))+1 "
          "WHERE key='revision';") &&
      Execute("COMMIT;");
  if (!merged) {
    Execute("ROLLBACK;");
  }
  const bool detached = Execute("DETACH DATABASE incoming;");
  return merged && detached;
}

bool StatsDatabase::PublishSnapshot(const std::filesystem::path& path) {
  std::filesystem::path temporary = path;
  temporary += L".tmp.";
  temporary += std::to_wstring(GetCurrentProcessId());
  temporary += L".";
  temporary += std::to_wstring(GetTickCount64());

  std::error_code error;
  if (std::filesystem::exists(temporary, error) || error) {
    return false;
  }

  sqlite3* snapshot = nullptr;
  const std::string utf8_path = temporary.u8string();
  bool success =
      sqlite_.open_v2(utf8_path.c_str(), &snapshot,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) == SQLITE_OK;
  if (success) {
    success = ExecuteSql(sqlite_, snapshot,
                         "PRAGMA journal_mode=DELETE;"
                         "PRAGMA synchronous=FULL;");
  }

  sqlite3_backup* backup = nullptr;
  if (success) {
    backup = sqlite_.backup_init(snapshot, "main", database_, "main");
    success = backup && sqlite_.backup_step(backup, -1) == SQLITE_DONE;
  }
  if (backup && sqlite_.backup_finish(backup) != SQLITE_OK) {
    success = false;
  }
  if (snapshot && sqlite_.close(snapshot) != SQLITE_OK) {
    success = false;
  }

  if (success) {
    success =
        MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!success) {
    std::filesystem::remove(temporary, error);
  }
  return success;
}

bool StatsDatabase::Synchronize(const std::filesystem::path& sync_directory,
                                std::uint32_t day,
                                Response& response) {
  const auto finish = [this, day, &response](bool synchronized) {
    if (!GetSummary(day, response)) {
      response = Response{};
      return false;
    }
    return synchronized;
  };

  if (sync_directory.empty()) {
    return finish(false);
  }

  bool initialized = false;
  if (!IsSyncInitialized(initialized)) {
    return finish(false);
  }

  std::error_code error;
  const bool root_exists = std::filesystem::exists(sync_directory, error);
  if (error ||
      (root_exists && !std::filesystem::is_directory(sync_directory, error)) ||
      error) {
    return finish(false);
  }

  const std::filesystem::path shared_snapshot =
      sync_directory / L"weasel-input-statistics.sqlite3";
  const bool shared_exists = std::filesystem::exists(shared_snapshot, error);
  if (error || (initialized && !shared_exists)) {
    return finish(false);
  }
  if (shared_exists) {
    if (!std::filesystem::is_regular_file(shared_snapshot, error) || error) {
      return finish(false);
    }

    std::filesystem::path staging_directory;
    for (int attempt = 0;
         attempt < 10 && staging_directory.empty();
         ++attempt) {
      std::filesystem::path candidate =
          database_path_.parent_path() /
          std::filesystem::u8path(".weasel-stats-sync-" +
                                  MakeGenerationId() + "-" +
                                  std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        staging_directory = std::move(candidate);
      } else if (error) {
        return finish(false);
      }
    }
    if (staging_directory.empty()) {
      return finish(false);
    }

    const std::filesystem::path local_copy =
        staging_directory / L"incoming.sqlite3";
    TemporarySnapshot temporary_snapshot(staging_directory, local_copy);
    std::filesystem::copy_file(shared_snapshot, local_copy,
                               std::filesystem::copy_options::none, error);
    if (error) {
      return finish(false);
    }
    int schema_version = 0;
    if (!ValidateSnapshot(local_copy, schema_version) ||
        !MergeSnapshot(local_copy, schema_version)) {
      return finish(false);
    }
  }

  std::filesystem::create_directories(sync_directory, error);
  if (error || !PublishSnapshot(shared_snapshot)) {
    return finish(false);
  }
  if (!initialized && !SetSyncInitialized()) {
    return finish(false);
  }
  return finish(true);
}

bool StatsDatabase::GetSummary(std::uint32_t day, Response& response) {
  Statement totals(sqlite_, database_,
                   "SELECT COALESCE(SUM(han_characters),0),"
                   "COALESCE(SUM(english_words),0),COALESCE(SUM(commits),0),"
                   "COALESCE(SUM(backspaces),0),"
                   "COALESCE(SUM(deleted_ascii_letters),0) "
                   "FROM daily_totals WHERE day=?1;");
  if (!totals.get() || !BindInteger(sqlite_, totals.get(), 1, day) ||
      sqlite_.step(totals.get()) != SQLITE_ROW) {
    return false;
  }
  Statement revision(sqlite_, database_,
                     "SELECT value FROM meta WHERE key='revision';");
  if (!revision.get() || sqlite_.step(revision.get()) != SQLITE_ROW) {
    return false;
  }

  response = Response{};
  response.status = ResponseStatus::kOk;
  response.day = day;
  response.revision =
      static_cast<std::uint64_t>(sqlite_.column_int64(revision.get(), 0));
  response.han_characters =
      static_cast<std::uint64_t>(sqlite_.column_int64(totals.get(), 0));
  response.english_words =
      static_cast<std::uint64_t>(sqlite_.column_int64(totals.get(), 1));
  response.overview_units = response.han_characters + response.english_words;
  response.commits =
      static_cast<std::uint64_t>(sqlite_.column_int64(totals.get(), 2));
  response.backspaces =
      static_cast<std::uint64_t>(sqlite_.column_int64(totals.get(), 3));
  response.deleted_ascii_letters =
      static_cast<std::uint64_t>(sqlite_.column_int64(totals.get(), 4));
  return true;
}

}  // namespace weasel::stats
