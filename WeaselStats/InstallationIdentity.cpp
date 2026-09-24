#include "InstallationIdentity.h"

#include <Windows.h>

#include <fstream>
#include <string_view>
#include <utility>

#include <WeaselStatsProtocol.h>

namespace weasel::stats {
namespace {

constexpr char kUnknownDevice[] = "unknown";
constexpr std::string_view kInstallationIdKey = "installation_id:";

std::string_view Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

std::string ReadInstallationId(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return kUnknownDevice;
  }

  std::string line;
  bool first_line = true;
  while (std::getline(input, line)) {
    if (first_line && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf) {
      line.erase(0, 3);
    }
    first_line = false;

    std::string_view trimmed = Trim(line);
    if (trimmed.rfind(kInstallationIdKey, 0) != 0) {
      continue;
    }

    trimmed = Trim(trimmed.substr(kInstallationIdKey.size()));
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\''))) {
      trimmed.remove_prefix(1);
      trimmed.remove_suffix(1);
    }
    if (trimmed.empty() || trimmed.size() >= kDeviceIdCapacity) {
      return kUnknownDevice;
    }
    return std::string(trimmed);
  }
  return kUnknownDevice;
}

}  // namespace

InstallationIdentity::InstallationIdentity(
    std::filesystem::path user_data_directory,
    std::uint64_t retry_interval_milliseconds)
    : installation_path_(std::move(user_data_directory) /
                         L"installation.yaml"),
      retry_interval_milliseconds_(retry_interval_milliseconds) {
  Reload(GetTickCount64());
}

const std::string& InstallationIdentity::device_id() {
  const std::uint64_t now = GetTickCount64();
  if (device_id_ == kUnknownDevice && now >= next_retry_tick_) {
    Reload(now);
  }
  return device_id_;
}

void InstallationIdentity::Reload(std::uint64_t now) {
  try {
    device_id_ = ReadInstallationId(installation_path_);
  } catch (...) {
    device_id_ = kUnknownDevice;
  }
  next_retry_tick_ = now + retry_interval_milliseconds_;
}

}  // namespace weasel::stats
