#include "InputStatisticsClient.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

std::uint32_t CurrentLocalDay() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  return static_cast<std::uint32_t>(time.wYear) * 10000 +
         static_cast<std::uint32_t>(time.wMonth) * 100 + time.wDay;
}

std::wstring PipeName() {
  DWORD session_id = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  return L"\\\\.\\pipe\\WeaselStats-" + std::to_wstring(session_id);
}

std::string MakeServerId() {
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);
  const ULONGLONG file_time =
      (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  char buffer[weasel::stats::kServerIdCapacity] = {};
  sprintf_s(buffer, "%08lx-%016llx-%016llx", GetCurrentProcessId(), file_time,
            GetTickCount64());
  return buffer;
}

void CopyFixed(char* destination,
               std::size_t capacity,
               const std::string& value) {
  const std::size_t length = (std::min)(capacity - 1, value.size());
  memcpy(destination, value.data(), length);
  destination[length] = '\0';
}

}  // namespace

InputStatisticsClient::~InputStatisticsClient() {
  Stop();
}

void InputStatisticsClient::Start(
    const std::string& device_id,
    const std::filesystem::path& install_directory,
    HWND notification_window,
    UINT notification_message) noexcept {
  notification_window_ = notification_window;
  notification_message_ = notification_message;
  try {
    if (worker_.joinable()) {
      return;
    }
    stats_executable_ = install_directory / L"WeaselStats.exe";
    device_id_ = device_id.empty() ? "unknown-device" : device_id;
    server_id_ = MakeServerId();
    queue_ = std::make_unique<std::array<Event, kQueueCapacity>>();
    queue_head_ = 0;
    queue_tail_ = 0;
    queue_size_ = 0;
    next_sequence_ = 1;
    stop_requested_.store(false);
    failure_notification_pending_.store(false);
    failure_signaled_.store(false);
    sync_pending_.store(false);
    sync_failure_notification_pending_.store(false);
    worker_ = std::thread(&InputStatisticsClient::WorkerMain, this);
  } catch (...) {
    SignalFailure();
  }
}

void InputStatisticsClient::Stop() noexcept {
  try {
    stop_requested_.store(true);
    queue_changed_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    sync_pending_.store(false);
  } catch (...) {
  }
}

bool InputStatisticsClient::TryEnqueueCommit(const char* utf8_text) noexcept {
  try {
    if (!utf8_text || !queue_ || stop_requested_.load()) {
      return false;
    }
    const std::size_t length =
        strnlen_s(utf8_text, weasel::stats::kCommitTextCapacity);
    if (!length || length >= weasel::stats::kCommitTextCapacity) {
      return false;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || queue_size_ == queue_->size()) {
      return false;
    }
    Event& event = (*queue_)[queue_tail_];
    event = Event{};
    event.type = EventType::kCommit;
    event.day = CurrentLocalDay();
    event.sequence = next_sequence_++;
    event.text_size = static_cast<std::uint32_t>(length);
    memcpy(event.text, utf8_text, length);
    queue_tail_ = (queue_tail_ + 1) % queue_->size();
    ++queue_size_;
    lock.unlock();
    queue_changed_.notify_one();
    return true;
  } catch (...) {
    return false;
  }
}

bool InputStatisticsClient::TryEnqueueCorrection(
    std::uint32_t backspaces,
    std::uint32_t deleted_ascii_letters) noexcept {
  try {
    if (!backspaces || !queue_ || stop_requested_.load()) {
      return false;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || queue_size_ == queue_->size()) {
      return false;
    }
    Event& event = (*queue_)[queue_tail_];
    event = Event{};
    event.type = EventType::kCorrection;
    event.day = CurrentLocalDay();
    event.sequence = next_sequence_++;
    event.backspaces = backspaces;
    event.deleted_ascii_letters = deleted_ascii_letters;
    queue_tail_ = (queue_tail_ + 1) % queue_->size();
    ++queue_size_;
    lock.unlock();
    queue_changed_.notify_one();
    return true;
  } catch (...) {
    return false;
  }
}

bool InputStatisticsClient::TryEnqueueSync(
    const std::string& sync_directory) noexcept {
  try {
    if (sync_directory.empty() ||
        sync_directory.size() >= weasel::stats::kCommitTextCapacity ||
        !queue_ || stop_requested_.load()) {
      SignalSyncFailure();
      return false;
    }
    bool expected = false;
    if (!sync_pending_.compare_exchange_strong(expected, true)) {
      return true;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || queue_size_ == queue_->size()) {
      sync_pending_.store(false);
      SignalSyncFailure();
      return false;
    }
    Event& event = (*queue_)[queue_tail_];
    event = Event{};
    event.type = EventType::kSync;
    event.day = CurrentLocalDay();
    event.text_size = static_cast<std::uint32_t>(sync_directory.size());
    memcpy(event.text, sync_directory.data(), sync_directory.size());
    queue_tail_ = (queue_tail_ + 1) % queue_->size();
    ++queue_size_;
    lock.unlock();
    queue_changed_.notify_one();
    return true;
  } catch (...) {
    sync_pending_.store(false);
    SignalSyncFailure();
    return false;
  }
}

StatisticsSummary InputStatisticsClient::GetSummary() const noexcept {
  try {
    std::unique_lock<std::mutex> lock(summary_mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
      return summary_;
    }
  } catch (...) {
    return StatisticsSummary{};
  }
  return StatisticsSummary{};
}

bool InputStatisticsClient::TryGetTodayOverview(
    DWORD& overview_units) const noexcept {
  overview_units = 0;
  const StatisticsSummary summary = GetSummary();
  if (summary.status != weasel::stats::SummaryStatus::kValid ||
      summary.day != CurrentLocalDay()) {
    return false;
  }
  overview_units = static_cast<DWORD>((std::min)(
      summary.overview_units, static_cast<std::uint64_t>(MAXDWORD) - 1));
  return true;
}

bool InputStatisticsClient::ConsumeFailureNotification() noexcept {
  return failure_notification_pending_.exchange(false);
}

bool InputStatisticsClient::ConsumeSyncFailureNotification() noexcept {
  return sync_failure_notification_pending_.exchange(false);
}

void InputStatisticsClient::WorkerMain() noexcept {
  try {
    LaunchStatsProcess();

    weasel::stats::Response response{};
    if (!SendWithRetry(MakeSummaryRequest(), response)) {
      MarkUnavailable();
      SignalFailure();
      return;
    }
    UpdateSummary(response);

    while (!stop_requested_.load()) {
      Event event{};
      if (TryPop(event)) {
        if (event.type == EventType::kSync) {
          const bool synchronized =
              SendSyncWithRetry(MakeRequest(event), response);
          if (weasel::stats::IsValid(response) &&
              response.day == event.day &&
              (response.status == weasel::stats::ResponseStatus::kOk ||
               response.status ==
                   weasel::stats::ResponseStatus::kSyncIncomplete)) {
            UpdateSummary(response);
          }
          sync_pending_.store(false);
          if (!synchronized && !stop_requested_.load()) {
            SignalSyncFailure();
          }
          continue;
        }
        if (!SendWithRetry(MakeRequest(event), response)) {
          MarkUnavailable();
          SignalFailure();
          return;
        }
        UpdateSummary(response);
        continue;
      }
      if (stop_requested_.load()) {
        break;
      }
      if (!SendWithRetry(MakeSummaryRequest(), response)) {
        MarkUnavailable();
        SignalFailure();
        return;
      }
      UpdateSummary(response);
    }

    auto shutdown = MakeSummaryRequest();
    shutdown.type = weasel::stats::MessageType::kShutdown;
    SendOnce(shutdown, response, kPipeTimeoutMilliseconds);
  } catch (...) {
    sync_pending_.store(false);
    try {
      MarkUnavailable();
    } catch (...) {
    }
    SignalFailure();
  }
}

bool InputStatisticsClient::LaunchStatsProcess() {
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(
      stats_executable_.c_str(), nullptr, nullptr, nullptr,
      FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
      &startup_info, &process_info);
  if (created) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
  }
  return !!created;
}

bool InputStatisticsClient::SendWithRetry(const weasel::stats::Request& request,
                                          weasel::stats::Response& response) {
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (stop_requested_.load()) {
      return false;
    }
    if (SendOnce(request, response, kPipeTimeoutMilliseconds) &&
        response.status == weasel::stats::ResponseStatus::kOk) {
      return true;
    }
    if (attempt + 1 < kMaxAttempts && WaitForStop(150u << attempt)) {
      return false;
    }
  }
  return false;
}

bool InputStatisticsClient::SendSyncWithRetry(
    const weasel::stats::Request& request,
    weasel::stats::Response& response) {
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (stop_requested_.load()) {
      return false;
    }
    if (SendOnce(request, response, kSyncTimeoutMilliseconds) &&
        response.status == weasel::stats::ResponseStatus::kOk) {
      return true;
    }
    if (attempt + 1 < kMaxAttempts && WaitForStop(150u << attempt)) {
      return false;
    }
  }
  return false;
}

bool InputStatisticsClient::SendOnce(const weasel::stats::Request& request,
                                     weasel::stats::Response& response,
                                     DWORD timeout_milliseconds) {
  const bool allow_during_stop =
      request.type == weasel::stats::MessageType::kShutdown;
  const ULONGLONG deadline = GetTickCount64() + timeout_milliseconds;
  const std::wstring pipe_name = PipeName();
  while (allow_during_stop || !stop_requested_.load()) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    const DWORD wait = static_cast<DWORD>((std::min<ULONGLONG>)(
        deadline - now, 100));
    if (WaitNamedPipeW(pipe_name.c_str(), wait)) {
      break;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_SEM_TIMEOUT && error != ERROR_FILE_NOT_FOUND &&
        error != ERROR_PIPE_BUSY) {
      return false;
    }
    if (error == ERROR_FILE_NOT_FOUND) {
      if (allow_during_stop) {
        Sleep(wait);
      } else if (WaitForStop(wait)) {
        return false;
      }
    }
  }
  if (!allow_during_stop && stop_requested_.load()) {
    return false;
  }

  HANDLE pipe =
      CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    CloseHandle(pipe);
    return false;
  }

  struct PendingTransaction {
    OVERLAPPED operation{};
    weasel::stats::Response response{};
  };
  auto transaction = std::make_unique<PendingTransaction>();
  HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!completion) {
    CloseHandle(pipe);
    return false;
  }
  transaction->operation.hEvent = completion;
  DWORD bytes_read = 0;
  response = weasel::stats::Response{};
  BOOL completed = TransactNamedPipe(
      pipe, const_cast<weasel::stats::Request*>(&request), sizeof(request),
      &transaction->response, sizeof(transaction->response), &bytes_read,
      &transaction->operation);
  if (!completed && GetLastError() != ERROR_IO_PENDING) {
    CloseHandle(completion);
    CloseHandle(pipe);
    return false;
  }

  bool success = completed != FALSE;
  while (!success && (allow_during_stop || !stop_requested_.load())) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      break;
    }
    const DWORD wait = static_cast<DWORD>((std::min<ULONGLONG>)(
        deadline - now, 100));
    const DWORD result = WaitForSingleObject(completion, wait);
    if (result == WAIT_OBJECT_0) {
      success =
          GetOverlappedResult(pipe, &transaction->operation, &bytes_read,
                              FALSE) != FALSE;
      break;
    }
    if (result != WAIT_TIMEOUT) {
      break;
    }
  }
  if (!success) {
    CancelIoEx(pipe, &transaction->operation);
    CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;
    if (WaitForSingleObject(completion, 1000) != WAIT_OBJECT_0) {
      transaction.release();
      completion = nullptr;
    }
  } else {
    response = transaction->response;
  }
  if (completion) {
    CloseHandle(completion);
  }
  if (pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe);
  }
  return success && bytes_read == sizeof(response) &&
         weasel::stats::IsValid(response);
}

bool InputStatisticsClient::WaitForStop(DWORD milliseconds) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  return queue_changed_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                 [this] { return stop_requested_.load(); });
}

bool InputStatisticsClient::TryPop(Event& event) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (!queue_size_) {
    queue_changed_.wait_for(lock, std::chrono::seconds(30), [this] {
      return stop_requested_.load() || queue_size_ != 0;
    });
  }
  if (!queue_size_) {
    return false;
  }
  event = (*queue_)[queue_head_];
  queue_head_ = (queue_head_ + 1) % queue_->size();
  --queue_size_;
  return true;
}

weasel::stats::Request InputStatisticsClient::MakeRequest(
    const Event& event) const {
  weasel::stats::Request request{};
  switch (event.type) {
    case EventType::kCommit:
      request.type = weasel::stats::MessageType::kCommit;
      break;
    case EventType::kCorrection:
      request.type = weasel::stats::MessageType::kCorrection;
      break;
    case EventType::kSync:
      request.type = weasel::stats::MessageType::kSync;
      break;
  }
  request.day = event.day;
  request.sequence = event.sequence;
  request.backspace_count = event.backspaces;
  request.deleted_ascii_letters = event.deleted_ascii_letters;
  request.text_size = event.text_size;
  CopyFixed(request.server_id, sizeof(request.server_id), server_id_);
  CopyFixed(request.device_id, sizeof(request.device_id), device_id_);
  if (event.text_size) {
    memcpy(request.text, event.text, event.text_size);
  }
  return request;
}

weasel::stats::Request InputStatisticsClient::MakeSummaryRequest() const {
  weasel::stats::Request request{};
  request.type = weasel::stats::MessageType::kGetToday;
  request.day = CurrentLocalDay();
  return request;
}

void InputStatisticsClient::UpdateSummary(
    const weasel::stats::Response& response) {
  std::lock_guard<std::mutex> lock(summary_mutex_);
  if (summary_.status == weasel::stats::SummaryStatus::kValid &&
      summary_.day == response.day && summary_.revision > response.revision) {
    return;
  }
  summary_.status = weasel::stats::SummaryStatus::kValid;
  summary_.day = response.day;
  summary_.revision = response.revision;
  summary_.overview_units = response.overview_units;
  summary_.updated_tick = GetTickCount64();
}

void InputStatisticsClient::MarkUnavailable() {
  std::lock_guard<std::mutex> lock(summary_mutex_);
  summary_.status = weasel::stats::SummaryStatus::kUnavailable;
}

void InputStatisticsClient::SignalFailure() noexcept {
  if (failure_signaled_.exchange(true)) {
    return;
  }
  failure_notification_pending_.store(true);
  if (notification_window_ && notification_message_) {
    PostMessage(notification_window_, notification_message_, 0, 0);
  }
}

void InputStatisticsClient::SignalSyncFailure() noexcept {
  sync_failure_notification_pending_.store(true);
  if (notification_window_ && notification_message_) {
    PostMessage(notification_window_, notification_message_, 0, 0);
  }
}
