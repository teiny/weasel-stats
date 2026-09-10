#include "InputStatisticsClient.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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

std::wstring QuoteArgument(const std::wstring& value) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(character);
      backslashes = 0;
    } else {
      result.append(backslashes, L'\\');
      backslashes = 0;
      result.push_back(character);
    }
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
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
    const std::filesystem::path& data_directory,
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
    data_directory_ = data_directory;
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
    SendOnce(shutdown, response);
  } catch (...) {
    try {
      MarkUnavailable();
    } catch (...) {
    }
    SignalFailure();
  }
}

bool InputStatisticsClient::LaunchStatsProcess() {
  std::wstring command = QuoteArgument(stats_executable_.wstring()) +
                         L" --data-directory " +
                         QuoteArgument(data_directory_.wstring());
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(
      stats_executable_.c_str(), mutable_command.data(), nullptr, nullptr,
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
    if (stop_requested_.load() || SendOnce(request, response)) {
      return !stop_requested_.load();
    }
    if (attempt + 1 < kMaxAttempts && WaitForStop(150u << attempt)) {
      return false;
    }
  }
  return false;
}

bool InputStatisticsClient::SendOnce(const weasel::stats::Request& request,
                                     weasel::stats::Response& response) {
  DWORD bytes_read = 0;
  response = weasel::stats::Response{};
  const BOOL success = CallNamedPipeW(
      PipeName().c_str(), const_cast<weasel::stats::Request*>(&request),
      sizeof(request), &response, sizeof(response), &bytes_read,
      kPipeTimeoutMilliseconds);
  return success && bytes_read == sizeof(response) &&
         weasel::stats::IsValid(response) &&
         response.status == weasel::stats::ResponseStatus::kOk;
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
  request.type = event.type == EventType::kCommit
                     ? weasel::stats::MessageType::kCommit
                     : weasel::stats::MessageType::kCorrection;
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
