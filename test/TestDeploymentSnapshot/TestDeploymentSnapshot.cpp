#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../../RimeWithWeasel/DeploymentSnapshot.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  if (!output) {
    throw std::runtime_error("cannot write test fixture");
  }
}

void TestSnapshots(const fs::path& directory) {
  const auto user = directory / L"user";
  const auto shared = directory / L"shared";
  const auto source = user / L"cn_dicts" / L"test.dict.yaml";
  const auto product = user / L"build" / L"test.prism.bin";
  Write(shared / L"default.yaml", "config_version: 1\n");
  Write(source, "123456789");
  Write(product, "compiled");
  weasel::DeploymentCache cache(user, shared, user / L"portable-sync",
                                "1.13.1");

  const auto initial = cache.Check();
  Check(initial.available && !initial.unchanged,
        "missing cache requires deployment");
  Check(initial.snapshot.sources.at("user/cn_dicts/test.dict.yaml").crc32 ==
            0xcbf43926,
        "uses the standard CRC32 check value");
  Check(!cache.RecordDeployment(initial.snapshot, false),
        "does not record a failed deployment");
  Check(!cache.Check().unchanged,
        "failed deployment does not establish a cache");
  Check(cache.RecordDeployment(initial.snapshot, true),
        "records successful deployment");
  Check(cache.Check().unchanged, "identical files skip deployment");
  std::atomic<bool> cancelled{true};
  Check(!cache.Check(&cancelled).available,
        "background scans can be cancelled");
  Check(cache.Check().unchanged, "cancelled scans preserve the valid baseline");

  const auto modified = fs::last_write_time(source);
  fs::last_write_time(source, modified + std::chrono::hours(1));
  fs::last_write_time(product, modified + std::chrono::hours(2));
  Check(cache.Check().unchanged, "timestamp-only changes do not deploy");
  Write(source, "223456789");
  fs::last_write_time(source, modified);
  Check(!cache.Check().unchanged,
        "same-size content changes with preserved timestamps deploy");
  Check(!cache.RecordDeployment(initial.snapshot, true),
        "sources changed during deployment are not recorded");
  Write(source, "123456789");
  Check(cache.Check().unchanged, "restored content matches the original cache");

  Write(user / L"user.yaml", "var:\n  last_build_time: 123\n");
  Write(user / L"installation.yaml", "installation_id: Worker\n");
  Write(user / L"test.userdb" / L"entries.yaml", "personal data\n");
  Write(user / L".temp.userdb" / L"data.bin", "temporary dictionary\n");
  Write(user / L"portable-sync" / L"Worker" / L"test.schema.yaml", "backup\n");
  Write(user / L"sync" / L"Other" / L"default.yaml", "other backup\n");
  Write(user / L"trash" / L"old.yaml", "old source\n");
  Write(user / L"weasel-input-statistics.sqlite3", "statistics\n");
  Write(user / L".weasel-stats-sync-123" / L"incoming.sqlite3", "incoming\n");
  Check(
      cache.Check().unchanged,
      "personal dictionaries, sync, statistics and temporary data are ignored");

  const auto added = user / fs::u8path(u8"cn_dicts/新增.dict.yaml");
  Write(added, "new dictionary\n");
  Check(!cache.Check().unchanged, "new nested Unicode dictionary deploys");
  fs::remove(added);
  const auto renamed = user / L"cn_dicts" / L"renamed.dict.yaml";
  fs::rename(source, renamed);
  Check(!cache.Check().unchanged, "renamed source deploys");
  fs::rename(renamed, source);
  fs::remove(source);
  Check(!cache.Check().unchanged, "removed source deploys");
  Write(source, "123456789");

  fs::remove(product);
  Check(!cache.Check().unchanged, "missing product deploys");
  Write(product, "corrupt!");
  Check(!cache.Check().unchanged, "same-size product corruption deploys");
  Write(product, "compiled");
  Check(cache.Check().unchanged, "unchanged products are reused");

  const auto manifest = user / L"build" / L".weasel-deployment-crc32";
  Write(manifest, "broken snapshot\n");
  Check(!cache.Check().unchanged, "invalid cache deploys");
  Check(cache.RecordDeployment(initial.snapshot, true),
        "repairs invalid cache");
  {
    std::ofstream output(manifest, std::ios::app);
    output << "S 10";
  }
  Check(!cache.Check().unchanged, "truncated cache record deploys");
  Check(cache.RecordDeployment(initial.snapshot, true), "restores valid cache");
  HANDLE held_cache =
      CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(held_cache != INVALID_HANDLE_VALUE, "holds cache against replacement");
  if (held_cache != INVALID_HANDLE_VALUE) {
    Check(!cache.RecordDeployment(initial.snapshot, true),
          "cache replacement failure is reported without throwing");
    CloseHandle(held_cache);
    Check(cache.Check().unchanged,
          "failed replacement retains the old baseline");
  }
  weasel::DeploymentCache newer(user, shared, user / L"portable-sync", "newer");
  Check(!newer.Check().unchanged, "Rime version change deploys");

  HANDLE locked = CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(locked != INVALID_HANDLE_VALUE,
        "locks source to simulate read failure");
  if (locked != INVALID_HANDLE_VALUE) {
    Check(!cache.Check().available, "source read failure falls back safely");
    CloseHandle(locked);
  }
}

std::atomic<bool> deployment_succeeded{false};
std::atomic<int> deployments{0};

void OnNotify(void*, uintptr_t, const char* type, const char* value) {
  if (std::string(type) == "deploy") {
    if (std::string(value) == "start") {
      ++deployments;
    }
    deployment_succeeded.store(std::string(value) == "success");
  }
}

bool Deploy(RimeApi* api,
            const weasel::DeploymentCache& cache,
            const weasel::DeploymentCheck& check) {
  // Weasel releases Rime during external maintenance and initializes it again
  // before its automatic deployment check.
  api->finalize();
  api->initialize(nullptr);
  Check(cache.PrepareDeployment(api, check.snapshot),
        "invalidates generated metadata through the public Rime API");
  deployment_succeeded.store(false);
  if (!api->start_maintenance(True)) {
    return false;
  }
  api->join_maintenance_thread();
  const bool success = deployment_succeeded.load();
  const bool view =
      success && api->deploy_config_file("weasel.yaml", "config_version");
  return cache.RecordDeployment(check.snapshot,
                                success && view && cache.VerifyDeployment(api));
}

void TestRime(const fs::path& directory) {
  const auto user = directory / L"user";
  const auto shared = directory / L"shared";
  const auto custom = user / L"default.custom.yaml";
  fs::create_directories(directory / L"logs");
  Write(shared / L"default.yaml",
        "config_version: '1'\nschema_list:\n  - schema: test\nmenu:\n  "
        "page_size: 3\n");
  Write(shared / L"weasel.yaml",
        "config_version: '1'\nstyle:\n  font_point: 12\n");
  Write(custom, "patch:\n  menu/page_size: 3\n");
  Write(
      shared / L"test.schema.yaml",
      "schema:\n  schema_id: test\n  name: Test\n  version: '1'\n"
      "engine:\n  processors: [speller, selector, navigator, express_editor]\n"
      "  segmentors: [abc_segmentor, fallback_segmentor]\n"
      "  translators: [script_translator]\n"
      "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n"
      "translator:\n  dictionary: test\n");
  const std::string words =
      u8"---\nname: test\nversion: '1'\nsort: "
      u8"by_weight\n...\n甲\tjia\t1\n乙\tyi\t1\n";
  Write(shared / L"test.dict.yaml", words);

  RimeApi* api = rime_get_api();
  const auto user_path = user.u8string();
  const auto shared_path = shared.u8string();
  RIME_STRUCT(RimeTraits, traits);
  traits.user_data_dir = user_path.c_str();
  traits.shared_data_dir = shared_path.c_str();
  traits.prebuilt_data_dir = shared_path.c_str();
  traits.app_name = "rime.weasel-crc32-test";
  traits.log_dir = "";
  traits.min_log_level = 2;
  api->setup(&traits);
  api->set_notification_handler(OnNotify, nullptr);
  api->initialize(nullptr);
  weasel::DeploymentCache cache(user, shared, user / L"sync",
                                api->get_version());

  Check(Deploy(api, cache, cache.Check()),
        "first deployment establishes cache");
  const auto prism = user / L"build" / L"test.prism.bin";
  const auto table = user / L"build" / L"test.table.bin";
  Check(fs::is_regular_file(prism) && fs::is_regular_file(table),
        "real Rime produces dictionary and spelling index");
  const int built = deployments.load();
  const auto output_time = fs::last_write_time(prism);

  const RimeSessionId session = api->create_session();
  Check(session && api->select_schema(session, "test"),
        "opens isolated Rime session");
  Check(api->simulate_key_sequence(session, "jia "),
        "records personal vocabulary");
  Check(api->sync_user_data(),
        "performs real personal dictionary synchronization");
  api->join_maintenance_thread();
  const auto after_sync = cache.Check();
  Check(after_sync.unchanged,
        "personal vocabulary sync does not require compilation");
  Check(fs::last_write_time(prism) == output_time,
        "personal vocabulary sync does not rebuild the spelling index");
  // Synchronization has its own deploy notifications; count subsequent builds.
  const int synchronized = deployments.load();
  for (int attempt = 0; attempt < 10; ++attempt) {
    fs::last_write_time(custom, fs::file_time_type::clock::now());
    Check(cache.Check().unchanged, "ten timestamp-only checks skip deployment");
  }
  Check(deployments.load() == synchronized && synchronized >= built,
        "content checks do not invoke maintenance");

  const auto modified = fs::last_write_time(custom);
  Write(custom, "patch:\n  menu/page_size: 5\n");
  fs::last_write_time(custom, modified);
  api->finalize();
  api->initialize(nullptr);
  const auto changed = cache.Check();
  Check(changed.available && !changed.unchanged,
        "CRC32 finds a real configuration edit");
  Check(Deploy(api, cache, changed), "changed content is successfully rebuilt");
  RimeConfig config{};
  int page_size = 0;
  Check(api->config_open("default", &config),
        "opens regenerated default config");
  Check(api->config_get_int(&config, "menu/page_size", &page_size) &&
            page_size == 5,
        "same-timestamp configuration edit reaches the compiled result");
  api->config_close(&config);
  Check(cache.Check().unchanged, "new successful build is reusable");

  fs::remove(prism);
  const auto missing = cache.Check();
  Check(!missing.unchanged, "real missing index requires deployment");
  Check(Deploy(api, cache, missing) && fs::is_regular_file(prism),
        "real maintenance repairs the missing index");
  const auto intact =
      cache.Check().snapshot.products.at("user/build/test.table.bin");
  {
    std::fstream binary(table, std::ios::binary | std::ios::in | std::ios::out);
    binary.seekg(-1, std::ios::end);
    char last = 0;
    binary.get(last);
    binary.seekp(-1, std::ios::end);
    binary.put(last ^ 1);
  }
  const auto corrupted = cache.Check();
  Check(!corrupted.unchanged, "detects corruption outside a binary header");
  Check(Deploy(api, cache, corrupted),
        "repairs a binary with a valid old header");
  Check(
      cache.Check().snapshot.products.at("user/build/test.table.bin") == intact,
      "reconstructed table matches its original complete CRC32");
  Write(shared / L"default.yaml", "schema_list: [\n");
  const auto invalid = cache.Check();
  Check(!Deploy(api, cache, invalid),
        "invalid YAML does not establish a baseline");
  Check(!cache.Check().unchanged, "failed build remains pending");
  api->finalize();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 5 && std::string(argv[1]) == "--probe") {
    weasel::DeploymentCache cache(fs::u8path(argv[2]), fs::u8path(argv[3]),
                                  fs::u8path(argv[4]),
                                  rime_get_api()->get_version());
    const auto started = GetTickCount64();
    const auto check = cache.Check();
    std::uint64_t bytes = 0;
    for (const auto& file : check.snapshot.sources) {
      bytes += file.second.size;
    }
    for (const auto& file : check.snapshot.products) {
      bytes += file.second.size;
    }
    std::cout << "available=" << check.available
              << " sources=" << check.snapshot.sources.size()
              << " products=" << check.snapshot.products.size()
              << " bytes=" << bytes
              << " elapsed_ms=" << GetTickCount64() - started << '\n';
    return check.available ? 0 : 1;
  }
  const auto temporary = fs::current_path() / L"msbuild";
  fs::create_directories(temporary);
  const auto directory = temporary / (L"weasel-deployment-test-" +
                                      std::to_wstring(GetCurrentProcessId()) +
                                      L"-" + std::to_wstring(GetTickCount64()));
  if (!fs::create_directory(directory)) {
    return 1;
  }
  try {
    TestSnapshots(directory / L"unit");
    if (argc != 2 || std::string(argv[1]) != "--unit") {
      TestRime(directory / L"integration");
    }
  } catch (const std::exception& error) {
    Check(false, error.what());
  }
  // Remove only the unique directory created by this test process.
  if (directory.parent_path() == temporary &&
      directory.filename().wstring().find(L"weasel-deployment-test-") == 0) {
    std::error_code error;
    fs::remove_all(directory, error);
    Check(!error, "cleans up isolated test data");
  }
  if (!failures) {
    std::cout << "Deployment snapshot tests passed.\n";
  }
  return failures ? 1 : 0;
}
