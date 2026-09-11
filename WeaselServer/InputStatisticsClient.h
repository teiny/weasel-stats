#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <WeaselStatsProtocol.h>

struct StatisticsSummary {
  weasel::stats::SummaryStatus status =
      weasel::stats::SummaryStatus::kUnavailable;
  std::uint32_t day = 0;
  std::uint64_t revision = 0;
  std::uint64_t overview_units = 0;
  ULONGLONG updated_tick = 0;
};

class InputStatisticsClient {
 public:
  InputStatisticsClient() = default;
  ~InputStatisticsClient();

  InputStatisticsClient(const InputStatisticsClient&) = delete;
  InputStatisticsClient& operator=(const InputStatisticsClient&) = delete;

  void Start(const std::string& device_id,
             const std::filesystem::path& install_directory,
             HWND notification_window,
             UINT notification_message) noexcept;
  void Stop() noexcept;

  bool TryEnqueueCommit(const char* utf8_text) noexcept;
  bool TryEnqueueCorrection(std::uint32_t backspaces,
                            std::uint32_t deleted_ascii_letters) noexcept;
  bool TryEnqueueSync(const std::string& sync_directory) noexcept;
  StatisticsSummary GetSummary() const noexcept;
  bool TryGetTodayOverview(DWORD& overview_units) const noexcept;
  bool ConsumeFailureNotification() noexcept;
  bool ConsumeSyncFailureNotification() noexcept;

 private:
  enum class EventType { kCommit, kCorrection, kSync };
  struct Event {
    EventType type = EventType::kCommit;
    std::uint32_t day = 0;
    std::uint64_t sequence = 0;
    std::uint32_t backspaces = 0;
    std::uint32_t deleted_ascii_letters = 0;
    std::uint32_t text_size = 0;
    char text[weasel::stats::kCommitTextCapacity] = {};
  };

  static constexpr std::size_t kQueueCapacity = 128;
  static constexpr int kMaxAttempts = 3;
  static constexpr DWORD kPipeTimeoutMilliseconds = 300;
  static constexpr DWORD kSyncTimeoutMilliseconds = 30000;

  void WorkerMain() noexcept;
  bool LaunchStatsProcess();
  bool SendWithRetry(const weasel::stats::Request& request,
                     weasel::stats::Response& response);
  bool SendSyncWithRetry(const weasel::stats::Request& request,
                         weasel::stats::Response& response);
  bool SendOnce(const weasel::stats::Request& request,
                weasel::stats::Response& response,
                DWORD timeout_milliseconds);
  bool WaitForStop(DWORD milliseconds);
  bool TryPop(Event& event);
  weasel::stats::Request MakeRequest(const Event& event) const;
  weasel::stats::Request MakeSummaryRequest() const;
  void UpdateSummary(const weasel::stats::Response& response);
  void MarkUnavailable();
  void SignalFailure() noexcept;
  void SignalSyncFailure() noexcept;

  std::filesystem::path stats_executable_;
  std::string device_id_;
  std::string server_id_;
  HWND notification_window_ = nullptr;
  UINT notification_message_ = 0;

  std::unique_ptr<std::array<Event, kQueueCapacity>> queue_;
  std::size_t queue_head_ = 0;
  std::size_t queue_tail_ = 0;
  std::size_t queue_size_ = 0;
  std::uint64_t next_sequence_ = 1;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_changed_;

  mutable std::mutex summary_mutex_;
  StatisticsSummary summary_;

  std::atomic<bool> stop_requested_{true};
  std::atomic<bool> failure_notification_pending_{false};
  std::atomic<bool> failure_signaled_{false};
  std::atomic<bool> sync_pending_{false};
  std::atomic<bool> sync_failure_notification_pending_{false};
  std::thread worker_;
};
