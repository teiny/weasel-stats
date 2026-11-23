#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <rime_api.h>

namespace weasel {

struct DeploymentFile {
  std::uint64_t size = 0;
  std::uint32_t crc32 = 0;

  bool operator==(const DeploymentFile& other) const {
    return size == other.size && crc32 == other.crc32;
  }
};

struct DeploymentSnapshot {
  std::string rime_version;
  std::map<std::string, DeploymentFile> sources;
  std::map<std::string, DeploymentFile> products;
};

struct DeploymentCheck {
  bool available = false;
  bool unchanged = false;
  DeploymentSnapshot snapshot;
};

class DeploymentCache {
 public:
  DeploymentCache(std::filesystem::path user_directory,
                  std::filesystem::path shared_directory,
                  std::filesystem::path sync_directory,
                  std::string rime_version);

  DeploymentCheck Check(
      const std::atomic<bool>* cancelled = nullptr) const noexcept;
  bool PrepareDeployment(RimeApi* api,
                         const DeploymentSnapshot& before) const noexcept;
  bool VerifyDeployment(RimeApi* api) const;
  bool RecordDeployment(const DeploymentSnapshot& before,
                        bool succeeded) const noexcept;

 private:
  bool Capture(DeploymentSnapshot& snapshot,
               const std::atomic<bool>* cancelled = nullptr) const;
  bool Read(DeploymentSnapshot& snapshot) const;
  bool Write(const DeploymentSnapshot& snapshot) const;

  std::filesystem::path user_directory_;
  std::filesystem::path shared_directory_;
  std::filesystem::path sync_directory_;
  std::filesystem::path cache_path_;
  std::string rime_version_;
};

}  // namespace weasel
