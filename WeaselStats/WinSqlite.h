#pragma once

#include <Windows.h>

struct sqlite3;
struct sqlite3_stmt;
using sqlite3_int64 = long long;
using sqlite3_destructor_type = void(WINAPI*)(void*);

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_OPEN_FULLMUTEX = 0x00010000;
#define SQLITE_TRANSIENT ((sqlite3_destructor_type) - 1)

namespace weasel::stats {

class WinSqlite {
 public:
  WinSqlite() = default;
  ~WinSqlite();

  WinSqlite(const WinSqlite&) = delete;
  WinSqlite& operator=(const WinSqlite&) = delete;

  bool Load();
  bool available() const { return module_ != nullptr; }

  using OpenV2 = int(WINAPI*)(const char*, sqlite3**, int, const char*);
  using Close = int(WINAPI*)(sqlite3*);
  using Exec = int(WINAPI*)(sqlite3*,
                            const char*,
                            int(WINAPI*)(void*, int, char**, char**),
                            void*,
                            char**);
  using Free = void(WINAPI*)(void*);
  using PrepareV2 =
      int(WINAPI*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
  using Step = int(WINAPI*)(sqlite3_stmt*);
  using Finalize = int(WINAPI*)(sqlite3_stmt*);
  using BindText = int(
      WINAPI*)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type);
  using BindInt64 = int(WINAPI*)(sqlite3_stmt*, int, sqlite3_int64);
  using ColumnInt64 = sqlite3_int64(WINAPI*)(sqlite3_stmt*, int);
  using Changes = int(WINAPI*)(sqlite3*);
  using Errmsg = const char*(WINAPI*)(sqlite3*);
  using BusyTimeout = int(WINAPI*)(sqlite3*, int);
  using LibversionNumber = int(WINAPI*)();

  OpenV2 open_v2 = nullptr;
  Close close = nullptr;
  Exec exec = nullptr;
  Free free = nullptr;
  PrepareV2 prepare_v2 = nullptr;
  Step step = nullptr;
  Finalize finalize = nullptr;
  BindText bind_text = nullptr;
  BindInt64 bind_int64 = nullptr;
  ColumnInt64 column_int64 = nullptr;
  Changes changes = nullptr;
  Errmsg errmsg = nullptr;
  BusyTimeout busy_timeout = nullptr;
  LibversionNumber libversion_number = nullptr;

 private:
  HMODULE module_ = nullptr;
};

}  // namespace weasel::stats
