#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

#include <WeaselStatsProtocol.h>

#include "StatsDatabase.h"
#include "WinSqlite.h"

namespace fs = std::filesystem;
using weasel::stats::MessageType;
using weasel::stats::Request;
using weasel::stats::Response;
using weasel::stats::ResponseStatus;

namespace {

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
  const auto device_id =
      FixedString(request.device_id, sizeof(request.device_id));
  switch (request.type) {
    case MessageType::kCommit:
      if (server_id.empty() || device_id.empty() || !request.day ||
          !request.sequence || !request.text_size) {
        response.status = ResponseStatus::kInvalidRequest;
        return true;
      }
      if (!database.RecordCommit(
              server_id, request.sequence, device_id, request.day,
              std::string_view(request.text, request.text_size), response)) {
        response.status = ResponseStatus::kDatabaseUnavailable;
      }
      return true;
    case MessageType::kCorrection:
      if (server_id.empty() || device_id.empty() || !request.day ||
          !request.sequence) {
        response.status = ResponseStatus::kInvalidRequest;
        return true;
      }
      if (!database.RecordCorrection(server_id, request.sequence, device_id,
                                     request.day, request.backspace_count,
                                     request.deleted_ascii_letters, response)) {
        response.status = ResponseStatus::kDatabaseUnavailable;
      }
      return true;
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
      stop = true;
      return true;
    default:
      response.status = ResponseStatus::kInvalidRequest;
      return true;
  }
}

int RunPipeServer(weasel::stats::StatsDatabase& database) {
  bool stop = false;
  while (!stop) {
    HANDLE pipe =
        CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                             PIPE_REJECT_REMOTE_CLIENTS,
                         1, sizeof(Response), sizeof(Request), 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
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
        HandleRequest(database, request, response, stop);
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

  HANDLE mutex = CreateMutexW(nullptr, TRUE, MutexName().c_str());
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mutex) {
      CloseHandle(mutex);
    }
    return 0;
  }

  const fs::path data_directory = UserDataDirectory();
  if (data_directory.empty()) {
    CloseHandle(mutex);
    return 1;
  }

  int result = 1;
  try {
    fs::create_directories(data_directory);
    weasel::stats::WinSqlite sqlite;
    if (sqlite.Load()) {
      weasel::stats::StatsDatabase database(sqlite);
      if (database.Open(data_directory / L"weasel-input-statistics.sqlite3")) {
        result = RunPipeServer(database);
      }
    }
  } catch (...) {
    result = 1;
  }

  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return result;
}
