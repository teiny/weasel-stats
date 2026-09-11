#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "WinSqlite.h"

namespace weasel::stats {

enum class ReportGranularity {
  kDay,
  kMonth,
  kYear,
};

struct ReportQuery {
  ReportGranularity granularity = ReportGranularity::kDay;
  int anchor = 0;
  std::vector<std::string> device_ids;
};

class StatsReport {
 public:
  StatsReport(WinSqlite& sqlite, std::filesystem::path database_path)
      : sqlite_(sqlite), database_path_(std::move(database_path)) {}

  bool BuildJson(const ReportQuery& query, std::string& json) noexcept;

 private:
  WinSqlite& sqlite_;
  std::filesystem::path database_path_;
};

}  // namespace weasel::stats
