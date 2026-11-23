#include "DeploymentSnapshot.h"

#include <boost/crc.hpp>
#include <array>
#include <fstream>
#include <iomanip>
#include <set>
#include <utility>
#include <vector>
#include <Windows.h>

namespace fs = std::filesystem;

namespace weasel {
namespace {

constexpr char kCacheVersion[] = "weasel-deployment-crc32-v1";

bool ReadFile(const fs::path& path,
              DeploymentFile& file,
              const std::atomic<bool>* cancelled) {
  const auto modified = fs::last_write_time(path);
  const auto size = fs::file_size(path);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  boost::crc_32_type crc;
  std::array<char, 65536> buffer;
  std::uint64_t bytes = 0;
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    if (cancelled && cancelled->load()) {
      return false;
    }
    crc.process_bytes(buffer.data(), static_cast<std::size_t>(input.gcount()));
    bytes += static_cast<std::uint64_t>(input.gcount());
  }
  if (!input.eof() || bytes != size || fs::file_size(path) != size ||
      fs::last_write_time(path) != modified) {
    return false;
  }
  file.size = bytes;
  file.crc32 = crc.checksum();
  return true;
}

bool IsRuntimeDirectory(const fs::path& path) {
  const auto name = path.filename().u8string();
  const auto extension = path.extension().u8string();
  return name == "sync" || name == "trash" || name == ".git" || name == "log" ||
         name.compare(0, 19, ".weasel-stats-sync-") == 0 ||
         extension == ".userdb";
}

bool SameSources(const DeploymentSnapshot& first,
                 const DeploymentSnapshot& second) {
  return first.rime_version == second.rime_version &&
         first.sources == second.sources;
}

bool ReadCompiledConfig(RimeApi* api,
                        const std::string& name,
                        const char* list,
                        std::vector<std::string>& schemas) {
  RimeConfig config{};
  if (!api->config_open(name.c_str(), &config)) {
    return false;
  }
  int timestamp = 0;
  bool valid =
      api->config_get_int(&config, ("__build_info/timestamps/" + name).c_str(),
                          &timestamp) &&
      timestamp > 0;
  if (valid && list) {
    RimeConfigIterator iterator{};
    if (api->config_begin_list(&iterator, &config, list)) {
      while (api->config_next(&iterator)) {
        std::string path = iterator.path;
        if (name == "default") {
          path += "/schema";
        }
        const char* schema = api->config_get_cstring(&config, path.c_str());
        if (!schema || !*schema) {
          valid = false;
          break;
        }
        schemas.emplace_back(schema);
      }
      api->config_end(&iterator);
    } else {
      valid = name != "default";
    }
  }
  api->config_close(&config);
  return valid;
}

}  // namespace

DeploymentCache::DeploymentCache(fs::path user_directory,
                                 fs::path shared_directory,
                                 fs::path sync_directory,
                                 std::string rime_version)
    : user_directory_(std::move(user_directory)),
      shared_directory_(std::move(shared_directory)),
      sync_directory_(std::move(sync_directory)),
      cache_path_(user_directory_ / L"build" / L".weasel-deployment-crc32"),
      rime_version_(std::move(rime_version)) {}

bool DeploymentCache::Capture(DeploymentSnapshot& snapshot,
                              const std::atomic<bool>* cancelled) const {
  snapshot = DeploymentSnapshot{};
  snapshot.rime_version = rime_version_;
  const std::array<std::pair<fs::path, std::string>, 2> roots = {
      {{user_directory_, "user/"}, {shared_directory_, "shared/"}}};
  for (const auto& root : roots) {
    if (!fs::is_directory(root.first)) {
      return false;
    }
    for (fs::recursive_directory_iterator entry(root.first), end; entry != end;
         ++entry) {
      if (cancelled && cancelled->load()) {
        return false;
      }
      const fs::path path = entry->path();
      if (entry->is_directory()) {
        if (IsRuntimeDirectory(path) ||
            (!sync_directory_.empty() &&
             fs::weakly_canonical(path) ==
                 fs::weakly_canonical(sync_directory_))) {
          entry.disable_recursion_pending();
        } else if (entry->is_symlink()) {
          // Do not silently omit source files behind a directory link.
          return false;
        }
        continue;
      }
      if (!entry->is_regular_file()) {
        continue;
      }
      const fs::path relative = path.lexically_relative(root.first);
      const auto name = path.filename().u8string();
      const auto extension = path.extension().u8string();
      const bool in_build = *relative.begin() == L"build";
      const bool product =
          (in_build && extension == ".yaml") || extension == ".bin";
      const bool source =
          !in_build && ((extension == ".yaml" && name != "user.yaml" &&
                         name != "installation.yaml") ||
                        name == "essay.txt");
      if (!source && !product) {
        continue;
      }
      DeploymentFile file;
      if (!ReadFile(path, file, cancelled)) {
        return false;
      }
      const std::string key = root.second + relative.generic_u8string();
      (product ? snapshot.products : snapshot.sources).emplace(key, file);
    }
  }
  return !snapshot.sources.empty();
}

bool DeploymentCache::Read(DeploymentSnapshot& snapshot) const {
  std::ifstream input(cache_path_, std::ios::binary);
  std::string version;
  if (!(input >> version >> std::quoted(snapshot.rime_version)) ||
      version != kCacheVersion) {
    return false;
  }
  char kind;
  while (input >> kind) {
    DeploymentFile file;
    std::string path;
    if (!(input >> file.size >> file.crc32 >> std::quoted(path)) ||
        (kind != 'S' && kind != 'P') || path.empty() ||
        !(kind == 'S' ? snapshot.sources : snapshot.products)
             .emplace(path, file)
             .second) {
      return false;
    }
  }
  return input.eof() && !snapshot.sources.empty() && !snapshot.products.empty();
}

bool DeploymentCache::Write(const DeploymentSnapshot& snapshot) const {
  fs::path temporary = cache_path_;
  temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << kCacheVersion << ' ' << std::quoted(snapshot.rime_version)
           << '\n';
    for (const auto& file : snapshot.sources) {
      output << "S " << file.second.size << ' ' << file.second.crc32 << ' '
             << std::quoted(file.first) << '\n';
    }
    for (const auto& file : snapshot.products) {
      output << "P " << file.second.size << ' ' << file.second.crc32 << ' '
             << std::quoted(file.first) << '\n';
    }
    output.flush();
    if (!output) {
      output.close();
      std::error_code error;
      fs::remove(temporary, error);
      return false;
    }
  }
  if (!MoveFileExW(temporary.c_str(), cache_path_.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code error;
    fs::remove(temporary, error);
    return false;
  }
  return true;
}

DeploymentCheck DeploymentCache::Check(
    const std::atomic<bool>* cancelled) const noexcept {
  DeploymentCheck result;
  try {
    result.available = Capture(result.snapshot, cancelled);
    DeploymentSnapshot recorded;
    result.unchanged = result.available && Read(recorded) &&
                       SameSources(recorded, result.snapshot) &&
                       recorded.products == result.snapshot.products;
  } catch (...) {
    result = DeploymentCheck{};
  }
  return result;
}

bool DeploymentCache::PrepareDeployment(
    RimeApi* api,
    const DeploymentSnapshot& before) const noexcept {
  if (!RIME_API_AVAILABLE(api, user_config_open) ||
      !RIME_API_AVAILABLE(api, config_clear)) {
    return false;
  }
  try {
    fs::create_directories(user_directory_ / L"build");
    DeploymentSnapshot recorded;
    if (Read(recorded)) {
      for (const auto& file : before.products) {
        const auto previous = recorded.products.find(file.first);
        if (file.first.compare(0, 11, "user/build/") == 0 &&
            fs::u8path(file.first).extension() == L".bin" &&
            previous != recorded.products.end() &&
            !(file.second == previous->second)) {
          // Rime checks the checksum stored in the binary's header, not its
          // full contents. Remove a changed generated binary to force repair.
          fs::remove(user_directory_ / fs::u8path(file.first.substr(5)));
        }
      }
    }
    std::set<std::string> configs = {"default", "weasel"};
    for (const auto& file : before.sources) {
      const auto name = fs::u8path(file.first).filename();
      if (name.u8string().size() > 12 &&
          name.u8string().compare(name.u8string().size() - 12, 12,
                                  ".schema.yaml") == 0) {
        configs.insert(name.stem().u8string());
      }
    }
    for (const auto& name : configs) {
      // Invalidate generated metadata only. Keeping the old settings allows
      // a failed rebuild to retain its previous usable configuration.
      RimeConfig config{};
      if (!api->user_config_open(("build/" + name).c_str(), &config)) {
        return false;
      }
      const bool cleared = api->config_clear(&config, "__build_info") != False;
      api->config_close(&config);
      if (!cleared) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool DeploymentCache::VerifyDeployment(RimeApi* api) const {
  std::vector<std::string> schemas;
  if (!ReadCompiledConfig(api, "default", "schema_list", schemas) ||
      !ReadCompiledConfig(api, "weasel", nullptr, schemas)) {
    return false;
  }
  std::set<std::string> checked;
  for (std::size_t index = 0; index < schemas.size(); ++index) {
    const std::string schema = schemas[index];
    if (!checked.insert(schema).second) {
      continue;
    }
    if (!ReadCompiledConfig(api, schema + ".schema", "schema/dependencies",
                            schemas)) {
      return false;
    }
  }
  return true;
}

bool DeploymentCache::RecordDeployment(const DeploymentSnapshot& before,
                                       bool succeeded) const noexcept {
  if (!succeeded) {
    return false;
  }
  try {
    DeploymentSnapshot after;
    return Capture(after) && SameSources(before, after) &&
           !after.products.empty() && Write(after);
  } catch (...) {
    return false;
  }
}

}  // namespace weasel
