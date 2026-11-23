#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <WeaselStatsProtocol.h>

#include "WinSqlite.h"

namespace weasel::stats {

class StatsDatabase {
 public:
  explicit StatsDatabase(WinSqlite& sqlite) : sqlite_(sqlite) {}
  ~StatsDatabase();

  StatsDatabase(const StatsDatabase&) = delete;
  StatsDatabase& operator=(const StatsDatabase&) = delete;

  bool Open(const std::filesystem::path& path);
  bool RecordCommit(std::string_view server_id,
                    std::uint64_t sequence,
                    std::string_view device_id,
                    std::uint32_t day,
                    std::string_view text,
                    Response& response);
  bool RecordCorrection(std::string_view server_id,
                        std::uint64_t sequence,
                        std::string_view device_id,
                        std::uint32_t day,
                        std::uint32_t backspaces,
                        std::uint32_t deleted_ascii_letters,
                        Response& response);
  bool GetSummary(std::uint32_t day, Response& response);
  bool Synchronize(const std::filesystem::path& sync_directory,
                   std::uint32_t day,
                   Response& response);

 private:
  bool Execute(const char* sql);
  bool CreateSchema();
  bool MigrateSchema1();
  bool MigrateSchema2();
  bool ValidateSchema3();
  bool InitializeGeneration();
  bool IsSyncInitialized(bool& initialized);
  bool SetSyncInitialized();
  bool ValidateSnapshot(const std::filesystem::path& path,
                        int& schema_version);
  bool MergeSnapshot(const std::filesystem::path& path, int schema_version);
  bool PublishSnapshot(const std::filesystem::path& path);
  bool BeginEvent(std::string_view server_id,
                  std::uint64_t sequence,
                  bool& duplicate);
  bool AdvanceRevision(std::uint64_t& revision);
  bool UpdateDailyTotals(std::string_view device_id,
                         std::uint32_t day,
                         std::uint64_t han_characters,
                         std::uint64_t english_words,
                         std::uint64_t commits,
                         std::uint64_t backspaces,
                         std::uint64_t deleted_ascii_letters,
                         std::uint64_t revision);
  WinSqlite& sqlite_;
  sqlite3* database_ = nullptr;
  std::filesystem::path database_path_;
  std::string generation_id_;
};

}  // namespace weasel::stats
