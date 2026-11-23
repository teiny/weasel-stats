#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <WeaselStatsProtocol.h>

#include "InstallationIdentity.h"
#include "StatsDatabase.h"
#include "StatsView.h"
#include "WinSqlite.h"

namespace fs = std::filesystem;
using weasel::stats::MessageType;
using weasel::stats::Request;
using weasel::stats::Response;
using weasel::stats::ResponseStatus;

namespace {

void WriteStartupLog(const char* stage,
                     DWORD win32_error = ERROR_SUCCESS) noexcept {
  wchar_t temp_directory[MAX_PATH] = {};
  const DWORD length = GetTempPathW(_countof(temp_directory), temp_directory);
  if (!length || length >= _countof(temp_directory)) {
    return;
  }

  constexpr wchar_t kLogFileName[] = L"WeaselStats-startup.log";
  if (length + _countof(kLogFileName) > _countof(temp_directory)) {
    return;
  }
  memcpy(temp_directory + length, kLogFileName, sizeof(kLogFileName));
  HANDLE file = CreateFileW(temp_directory, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  SYSTEMTIME time{};
  GetLocalTime(&time);
  char line[256] = {};
  const int line_length = sprintf_s(
      line, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu stage=%s error=%lu\r\n",
      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
      time.wSecond, time.wMilliseconds, GetCurrentProcessId(), stage,
      win32_error);
  if (line_length > 0) {
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(line_length), &written, nullptr);
  }
  CloseHandle(file);
}

std::wstring PipeName() {
  DWORD session_id = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  return L"\\\\.\\pipe\\WeaselStats-" + std::to_wstring(session_id);
}

std::wstring MutexName() {
  DWORD session_id = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  return L"Local\\WeaselStats-" + std::to_wstring(session_id);
}

bool IsViewMode(PWSTR command_line) {
  std::wstring_view arguments = command_line ? command_line : L"";
  const auto first = arguments.find_first_not_of(L" \t\r\n");
  if (first == std::wstring_view::npos) {
    return false;
  }
  const auto last = arguments.find_last_not_of(L" \t\r\n");
  return arguments.substr(first, last - first + 1) == L"--view";
}

fs::path UserDataDirectory() {
  wchar_t path[MAX_PATH] = {};
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel", 0,
                    KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    DWORD size = sizeof(path);
    DWORD type = 0;
    const LSTATUS result =
        RegQueryValueExW(key, L"RimeUserDir", nullptr, &type,
                         reinterpret_cast<LPBYTE>(path), &size);
    RegCloseKey(key);
    if (result == ERROR_SUCCESS && type == REG_SZ && path[0]) {
      return fs::path(path);
    }
  }
  const DWORD expanded =
      ExpandEnvironmentStringsW(L"%AppData%\\Rime", path, _countof(path));
  if (!expanded || expanded > _countof(path)) {
    return {};
  }
  return fs::path(path);
}

std::string_view FixedString(const char* value, std::size_t capacity) {
  return {value, strnlen_s(value, capacity)};
}

bool HandleRequest(weasel::stats::StatsDatabase& database,
                   weasel::stats::InstallationIdentity& identity,
                   const Request& request,
                   Response& response,
                   bool& stop) {
  response = Response{};
  if (!weasel::stats::IsValid(request)) {
    response.status = ResponseStatus::kInvalidRequest;
    return true;
  }

  const auto server_id =
      FixedString(request.server_id, sizeof(request.server_id));
  switch (request.type) {
    case MessageType::kCommit: {
      if (server_id.empty() || !request.day || !request.sequence ||
          !request.text_size) {
        response.status = ResponseStatus::kInvalidRequest;
        return true;
      }
      const std::string& device_id = identity.device_id();
      if (!database.RecordCommit(
              server_id, request.sequence, device_id, request.day,
              std::string_view(request.text, request.text_size), response)) {
        response.status = ResponseStatus::kDatabaseUnavailable;
      }
      return true;
    }
    case MessageType::kCorrection: {
      if (server_id.empty() || !request.day || !request.sequence) {
        response.status = ResponseStatus::kInvalidRequest;
        return true;
      }
      const std::string& device_id = identity.device_id();
      if (!database.RecordCorrection(server_id, request.sequence, device_id,
                                     request.day, request.backspace_count,
                                     request.deleted_ascii_letters, response)) {
        response.status = ResponseStatus::kDatabaseUnavailable;
      }
      return true;
    }
    case MessageType::kGetToday:
      if (!request.day || !database.GetSummary(request.day, response)) {
        response.status = ResponseStatus::kDatabaseUnavailable;
      }
      return true;
    case MessageType::kSync:
      if (!request.day || !request.text_size) {
        response.status = ResponseStatus::kInvalidRequest;
        return true;
      }
      try {
        const fs::path sync_directory = fs::u8path(
            std::string(request.text, request.text_size));
        if (!database.Synchronize(sync_directory, request.day, response)) {
          response.status =
              response.status == ResponseStatus::kOk
                  ? ResponseStatus::kSyncIncomplete
                  : ResponseStatus::kDatabaseUnavailable;
        }
      } catch (...) {
        response = Response{};
        response.status = ResponseStatus::kSyncIncomplete;
      }
      return true;
    case MessageType::kShutdown:
      response.status = ResponseStatus::kOk;
      WriteStartupLog("shutdown_request_received");
      stop = true;
      return true;
    default:
      response.status = ResponseStatus::kInvalidRequest;
      return true;
  }
}

int RunPipeServer(weasel::stats::StatsDatabase& database,
                  weasel::stats::InstallationIdentity& identity) {
  bool stop = false;
  while (!stop) {
    HANDLE pipe =
        CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                             PIPE_REJECT_REMOTE_CLIENTS,
                         1, sizeof(Response), sizeof(Request), 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      WriteStartupLog("pipe_create_failed", GetLastError());
      return 1;
    }

    const bool connected = ConnectNamedPipe(pipe, nullptr) ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      Request request{};
      DWORD bytes_read = 0;
      Response response{};
      if (ReadFile(pipe, &request, sizeof(request), &bytes_read, nullptr) &&
          bytes_read == sizeof(request)) {
        HandleRequest(database, identity, request, response, stop);
      }
      DWORD bytes_written = 0;
      WriteFile(pipe, &response, sizeof(response), &bytes_written, nullptr);
      FlushFileBuffers(pipe);
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE,
                    PWSTR command_line,
                    int) {
  if (IsViewMode(command_line)) {
    WriteStartupLog("view_mode_begin");
    const fs::path data_directory = UserDataDirectory();
    if (data_directory.empty()) {
      WriteStartupLog("view_user_data_directory_failed");
      return 1;
    }
    const int result = weasel::stats::RunStatsView(instance, data_directory);
    WriteStartupLog("view_mode_end", static_cast<DWORD>(result));
    return result;
  }

  WriteStartupLog("service_mode_begin");
  if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
    WriteStartupLog("set_priority_failed", GetLastError());
  }

  HANDLE mutex = CreateMutexW(nullptr, TRUE, MutexName().c_str());
  if (!mutex) {
    WriteStartupLog("mutex_create_failed", GetLastError());
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    WriteStartupLog("mutex_already_exists");
    CloseHandle(mutex);
    return 0;
  }

  const fs::path data_directory = UserDataDirectory();
  if (data_directory.empty()) {
    WriteStartupLog("user_data_directory_failed");
    CloseHandle(mutex);
    return 1;
  }

  int result = 1;
  try {
    fs::create_directories(data_directory);
    WriteStartupLog("data_directory_ready");
    weasel::stats::WinSqlite sqlite;
    if (sqlite.Load()) {
      WriteStartupLog("sqlite_loaded");
      weasel::stats::StatsDatabase database(sqlite);
      if (database.Open(data_directory / L"weasel-input-statistics.sqlite3")) {
        weasel::stats::InstallationIdentity identity(data_directory);
        WriteStartupLog("database_opened");
        WriteStartupLog("pipe_server_begin");
        result = RunPipeServer(database, identity);
      } else {
        WriteStartupLog("database_open_failed");
      }
    } else {
      WriteStartupLog("sqlite_load_failed", GetLastError());
    }
  } catch (...) {
    WriteStartupLog("service_exception");
    result = 1;
  }

  ReleaseMutex(mutex);
  CloseHandle(mutex);
  WriteStartupLog("service_mode_end", static_cast<DWORD>(result));
  return result;
}
