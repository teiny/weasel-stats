#include "WinSqlite.h"

#include <array>
#include <string>

namespace weasel::stats {
namespace {

constexpr ULONG kMinimumWinSqliteBuild = 14393;  // Windows 10 1607 (RS1)

struct RtlOsVersionInfo {
  ULONG size = sizeof(RtlOsVersionInfo);
  ULONG major_version = 0;
  ULONG minor_version = 0;
  ULONG build_number = 0;
  ULONG platform_id = 0;
  WCHAR service_pack[128] = {};
};

bool HasSupportedWinSqliteAbi() {
  using RtlGetVersion = LONG(WINAPI*)(RtlOsVersionInfo*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return false;
  }
  const auto get_version =
      reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion"));
  RtlOsVersionInfo version;
  if (!get_version || get_version(&version) != 0) {
    return false;
  }
  return version.major_version > 10 ||
         (version.major_version == 10 &&
          version.build_number >= kMinimumWinSqliteBuild);
}

}  // namespace

WinSqlite::~WinSqlite() {
  if (module_) {
    FreeLibrary(module_);
  }
}

bool WinSqlite::Load() {
  if (module_) {
    return true;
  }
  if (!HasSupportedWinSqliteAbi()) {
    return false;
  }

  std::array<wchar_t, MAX_PATH> system_directory{};
  const UINT length = GetSystemDirectoryW(
      system_directory.data(), static_cast<UINT>(system_directory.size()));
  if (!length || length >= system_directory.size()) {
    return false;
  }
  std::wstring path(system_directory.data(), length);
  path.append(L"\\winsqlite3.dll");
  module_ = LoadLibraryW(path.c_str());
  if (!module_) {
    return false;
  }

#define LOAD_SQLITE_API(member, symbol)                                    \
  member =                                                                 \
      reinterpret_cast<decltype(member)>(GetProcAddress(module_, symbol)); \
  if (!member) {                                                           \
    FreeLibrary(module_);                                                  \
    module_ = nullptr;                                                     \
    return false;                                                          \
  }

  LOAD_SQLITE_API(open_v2, "sqlite3_open_v2");
  LOAD_SQLITE_API(close, "sqlite3_close");
  LOAD_SQLITE_API(exec, "sqlite3_exec");
  LOAD_SQLITE_API(free, "sqlite3_free");
  LOAD_SQLITE_API(prepare_v2, "sqlite3_prepare_v2");
  LOAD_SQLITE_API(step, "sqlite3_step");
  LOAD_SQLITE_API(finalize, "sqlite3_finalize");
  LOAD_SQLITE_API(bind_text, "sqlite3_bind_text");
  LOAD_SQLITE_API(bind_int64, "sqlite3_bind_int64");
  LOAD_SQLITE_API(column_int64, "sqlite3_column_int64");
  LOAD_SQLITE_API(changes, "sqlite3_changes");
  LOAD_SQLITE_API(errmsg, "sqlite3_errmsg");
  LOAD_SQLITE_API(busy_timeout, "sqlite3_busy_timeout");
  LOAD_SQLITE_API(libversion_number, "sqlite3_libversion_number");

#undef LOAD_SQLITE_API
  return true;
}

}  // namespace weasel::stats
