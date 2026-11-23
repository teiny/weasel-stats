#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace weasel::stats {

class InstallationIdentity {
 public:
  explicit InstallationIdentity(
      std::filesystem::path user_data_directory,
      std::uint64_t retry_interval_milliseconds = 30000);

  const std::string& device_id();

 private:
  void Reload(std::uint64_t now);

  std::filesystem::path installation_path_;
  std::string device_id_ = "unknown";
  std::uint64_t retry_interval_milliseconds_ = 0;
  std::uint64_t next_retry_tick_ = 0;
};

}  // namespace weasel::stats
