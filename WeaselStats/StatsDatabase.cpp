#include "StatsDatabase.h"

#include <limits>

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
  {
    Statement schema_version(sqlite_, database_, "PRAGMA user_version;");
    if (!schema_version.get() ||
        sqlite_.step(schema_version.get()) != SQLITE_ROW ||
        sqlite_.column_int64(schema_version.get(), 0) > 1) {
      return false;
    }
  }
  sqlite_.busy_timeout(database_, 1000);
  return Execute("PRAGMA journal_mode=WAL;") &&
         Execute("PRAGMA synchronous=NORMAL;") &&
         Execute(
             "CREATE TABLE IF NOT EXISTS meta("
             "key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
             "INSERT OR IGNORE INTO meta(key,value) VALUES('revision',0);"
             "CREATE TABLE IF NOT EXISTS processed_sessions("
             "server_id TEXT PRIMARY KEY,last_sequence INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS daily_totals("
             "device_id TEXT NOT NULL,day INTEGER NOT NULL,"
             "han_characters INTEGER NOT NULL,english_words INTEGER NOT NULL,"
             "commits INTEGER NOT NULL,backspaces INTEGER NOT NULL,"
             "deleted_ascii_letters INTEGER NOT NULL,revision INTEGER NOT NULL,"
             "PRIMARY KEY(device_id,day));"
             "CREATE INDEX IF NOT EXISTS daily_totals_day ON daily_totals(day);"
             "CREATE TABLE IF NOT EXISTS daily_terms("
             "device_id TEXT NOT NULL,day INTEGER NOT NULL,term TEXT NOT NULL,"
             "count INTEGER NOT NULL,revision INTEGER NOT NULL,"
             "PRIMARY KEY(device_id,day,term));"
             "CREATE INDEX IF NOT EXISTS daily_terms_day_count "
             "ON daily_terms(day,count DESC);"
             "PRAGMA user_version=1;");
}

bool StatsDatabase::Execute(const char* sql) {
  char* error = nullptr;
  const int result = sqlite_.exec(database_, sql, nullptr, nullptr, &error);
  if (error) {
    sqlite_.free(error);
  }
  return result == SQLITE_OK;
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
      "INSERT OR IGNORE INTO daily_totals(device_id,day,han_characters,"
      "english_words,commits,backspaces,deleted_ascii_letters,revision) "
      "VALUES(?1,?2,0,0,0,0,0,0);");
  if (!insert.get() || !BindText(sqlite_, insert.get(), 1, device_id) ||
      !BindInteger(sqlite_, insert.get(), 2, day) ||
      sqlite_.step(insert.get()) != SQLITE_DONE) {
    return false;
  }

  Statement update(sqlite_, database_,
                   "UPDATE daily_totals SET han_characters=han_characters+?1,"
                   "english_words=english_words+?2,commits=commits+?3,"
                   "backspaces=backspaces+?4,"
                   "deleted_ascii_letters=deleted_ascii_letters+?5,revision=?6 "
                   "WHERE device_id=?7 AND day=?8;");
  return update.get() &&
         BindInteger(sqlite_, update.get(), 1, han_characters) &&
         BindInteger(sqlite_, update.get(), 2, english_words) &&
         BindInteger(sqlite_, update.get(), 3, commits) &&
         BindInteger(sqlite_, update.get(), 4, backspaces) &&
         BindInteger(sqlite_, update.get(), 5, deleted_ascii_letters) &&
         BindInteger(sqlite_, update.get(), 6, revision) &&
         BindText(sqlite_, update.get(), 7, device_id) &&
         BindInteger(sqlite_, update.get(), 8, day) &&
         sqlite_.step(update.get()) == SQLITE_DONE;
}

bool StatsDatabase::UpdateTerm(std::string_view device_id,
                               std::uint32_t day,
                               std::string_view text,
                               std::uint64_t revision) {
  Statement insert(
      sqlite_, database_,
      "INSERT OR IGNORE INTO daily_terms(device_id,day,term,count,revision) "
      "VALUES(?1,?2,?3,0,0);");
  if (!insert.get() || !BindText(sqlite_, insert.get(), 1, device_id) ||
      !BindInteger(sqlite_, insert.get(), 2, day) ||
      !BindText(sqlite_, insert.get(), 3, text) ||
      sqlite_.step(insert.get()) != SQLITE_DONE) {
    return false;
  }

  Statement update(sqlite_, database_,
                   "UPDATE daily_terms SET count=count+1,revision=?1 "
                   "WHERE device_id=?2 AND day=?3 AND term=?4;");
  return update.get() && BindInteger(sqlite_, update.get(), 1, revision) &&
         BindText(sqlite_, update.get(), 2, device_id) &&
         BindInteger(sqlite_, update.get(), 3, day) &&
         BindText(sqlite_, update.get(), 4, text) &&
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
