#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <WeaselStatsProtocol.h>

#include "../../WeaselStats/InstallationIdentity.h"
#include "../../WeaselStats/StatsDatabase.h"
#include "../../WeaselStats/StatsReport.h"
#include "../../WeaselStats/TextMetrics.h"
#include "../../WeaselStats/WinSqlite.h"
#include "../../WeaselServer/InputStatisticsClient.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    const std::string output = std::string("FAILED: ") + message + "\r\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), output.data(),
              static_cast<DWORD>(output.size()), &written, nullptr);
    ++failures;
  }
}

void TestTextMetrics() {
  auto metrics = weasel::stats::CountTextMetrics(u8"中国 hello world");
  Check(metrics.han_characters == 2, "counts Han characters");
  Check(metrics.english_words == 2, "counts ASCII word runs");
  Check(metrics.overview_units() == 4, "combines overview units");

  metrics = weasel::stats::CountTextMetrics("hello-world don't");
  Check(metrics.han_characters == 0, "does not count punctuation as Han");
  Check(metrics.english_words == 4,
        "treats hyphen and apostrophe as word separators");

  metrics = weasel::stats::CountTextMetrics(u8"。😀");
  Check(metrics.overview_units() == 0,
        "does not count punctuation or emoji in overview");
}

void TestInstallationIdentity() {
  const fs::path directory =
      fs::temp_directory_path() /
      (L"weasel-stats-identity-test-" +
       std::to_wstring(GetCurrentProcessId()));
  std::error_code error;
  fs::remove_all(directory, error);
  error.clear();
  fs::create_directories(directory, error);
  Check(!error, "creates installation identity test directory");
  if (error) {
    return;
  }

  weasel::stats::InstallationIdentity identity(directory, 0);
  Check(identity.device_id() == "unknown",
        "uses unknown when installation.yaml is missing");

  {
    std::ofstream installation(directory / L"installation.yaml",
                               std::ios::binary | std::ios::trunc);
    installation << "distribution_code_name: Weasel\n";
  }
  Check(identity.device_id() == "unknown",
        "uses unknown when installation_id is missing");

  {
    std::ofstream installation(directory / L"installation.yaml",
                               std::ios::binary | std::ios::trunc);
    installation << "installation_id: \"Worker\"\n";
  }
  Check(identity.device_id() == "Worker",
        "reloads a valid installation id after an unknown result");

  fs::remove_all(directory, error);
  Check(!error, "removes installation identity test directory");
}

void TestDatabase() {
  weasel::stats::WinSqlite sqlite;
  Check(sqlite.Load(), "loads winsqlite3 from the Windows system directory");
  if (!sqlite.available()) {
    return;
  }

  const fs::path directory =
      fs::temp_directory_path() /
      (L"weasel-stats-test-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code error;
  fs::remove_all(directory, error);
  fs::create_directories(directory, error);
  Check(!error, "creates isolated test directory");
  if (error) {
    return;
  }

  {
    const fs::path database_path = directory / L"statistics.sqlite3";
    weasel::stats::StatsDatabase database(sqlite);
    Check(database.Open(database_path),
          "creates statistics database");

    weasel::stats::Response response{};
    Check(database.RecordCommit("server-a", 1, "device-a", 20260910,
                                u8"中国 hello world", response),
          "records a commit");
    Check(response.overview_units == 4 && response.commits == 1,
          "returns committed daily summary");

    Check(database.RecordCommit("server-a", 1, "device-a", 20260910,
                                u8"中国 hello world", response),
          "accepts duplicate event idempotently");
    Check(response.overview_units == 4 && response.commits == 1,
          "does not count duplicate sequence twice");

    Check(database.RecordCommit("server-a", 2, "device-b", 20260910, "hello",
                                response),
          "records another device contribution");
    Check(response.overview_units == 5 && response.commits == 2,
          "sums device components for the overview");

    Check(database.RecordCorrection("server-a", 3, "device-a", 20260910, 1, 1,
                                    response),
          "records correction counters");
    Check(response.backspaces == 1 && response.deleted_ascii_letters == 1,
          "returns correction counters");

    Check(database.GetSummary(20260909, response),
          "queries a day without a stored row");
    Check(response.status == weasel::stats::ResponseStatus::kOk &&
              response.day == 20260909 && response.overview_units == 0 &&
              response.commits == 0,
          "represents a missing day as a valid zero");

    weasel::stats::StatsReport report(sqlite, database_path);
    weasel::stats::ReportQuery query;
    query.granularity = weasel::stats::ReportGranularity::kDay;
    query.anchor = 202609;
    std::string json;
    Check(report.BuildJson(query, json), "builds statistics report JSON");
    Check(json.find("\"devices\"") == std::string::npos &&
              json.find("\"allDevices\"") == std::string::npos,
          "does not expose a device dimension in the report");
    const std::size_t day = json.find("\"key\":20260910");
    const std::size_t end = json.find('}', day);
    const std::size_t value = json.find("\"value\":5", day);
    Check(day != std::string::npos && value != std::string::npos &&
              end != std::string::npos && value < end,
          "aggregates report values across devices");
  }

  fs::remove_all(directory, error);
  Check(!error, "removes isolated test directory");
}

void TestClientProcess(const fs::path& install_directory) {
  const fs::path directory =
      fs::temp_directory_path() /
      (L"weasel-stats-client-test-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code error;
  fs::remove_all(directory, error);
  error.clear();
  fs::create_directories(directory, error);
  Check(!error, "creates isolated client test directory");
  if (error) {
    return;
  }

  InputStatisticsClient client;
  client.Start("test-device", install_directory, nullptr, 0);

  StatisticsSummary summary;
  for (int attempt = 0; attempt < 50; ++attempt) {
    summary = client.GetSummary();
    if (summary.status == weasel::stats::SummaryStatus::kValid) {
      break;
    }
    Sleep(50);
  }
  Check(summary.status == weasel::stats::SummaryStatus::kValid,
        "starts WeaselStats and obtains an initial summary");
  Check(summary.overview_units == 0,
        "initial summary is a valid zero, not unavailable");
  DWORD overview_units = MAXDWORD;
  Check(client.TryGetTodayOverview(overview_units) && overview_units == 0,
        "exposes a valid zero overview to the server menu cache");

  Check(client.TryEnqueueCommit(u8"中国 hello"),
        "enqueues a commit without IPC on the caller thread");
  for (int attempt = 0; attempt < 50; ++attempt) {
    summary = client.GetSummary();
    if (summary.status == weasel::stats::SummaryStatus::kValid &&
        summary.overview_units == 3) {
      break;
    }
    Sleep(50);
  }
  Check(summary.overview_units == 3,
        "receives committed overview through the named pipe");
  Check(client.TryGetTodayOverview(overview_units) && overview_units == 3,
        "exposes the committed overview to the server menu cache");
  client.Stop();

  for (int attempt = 0; attempt < 20; ++attempt) {
    error.clear();
    fs::remove_all(directory, error);
    if (!error) {
      break;
    }
    Sleep(50);
  }
  Check(!error, "removes isolated client test directory");
}

void TestMissingProcessIsBounded() {
  InputStatisticsClient client;
  const ULONGLONG started = GetTickCount64();
  client.Start("test-device",
               fs::temp_directory_path() / L"missing-weasel-stats", nullptr,
               0);
  bool notified = false;
  for (int attempt = 0; attempt < 60; ++attempt) {
    if (client.ConsumeFailureNotification()) {
      notified = true;
      break;
    }
    Sleep(50);
  }
  client.Stop();
  Check(notified, "reports an unavailable statistics process once");
  Check(GetTickCount64() - started < 5000,
        "stops retrying a missing statistics process within a fixed bound");
  Check(
      client.GetSummary().status == weasel::stats::SummaryStatus::kUnavailable,
      "does not report a missing statistics process as zero input");
  DWORD overview_units = 0;
  Check(!client.TryGetTodayOverview(overview_units),
        "does not expose an unavailable summary to the menu");
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--metrics") {
    TestTextMetrics();
    return failures ? 1 : 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--database") {
    TestDatabase();
    return failures ? 1 : 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--identity") {
    TestInstallationIdentity();
    return failures ? 1 : 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--missing") {
    TestMissingProcessIsBounded();
    return failures ? 1 : 0;
  }
  TestTextMetrics();
  TestInstallationIdentity();
  TestDatabase();
  if (argc == 2) {
    TestClientProcess(fs::path(argv[1]).parent_path());
  }
  TestMissingProcessIsBounded();
  if (!failures) {
    std::cout << "All WeaselStats tests passed.\n";
  }
  return failures ? 1 : 0;
}
