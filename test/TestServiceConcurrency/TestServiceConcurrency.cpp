#include "../../WeaselIPCServer/WeaselServerImpl.cpp"
#include <RimeWithWeasel.h>
RimeApi* TestRimeGetApi();
#define rime_get_api TestRimeGetApi
#include "../../RimeWithWeasel/RimeWithWeasel.cpp"
#undef rime_get_api
#include <WeaselUI.h>
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <future>
#include <fstream>
#include <iostream>
#include <thread>

CAppModule _Module;

static fs::path test_user;
static fs::path test_shared;
fs::path WeaselUserDataPath() {
  return test_user;
}
fs::path WeaselSharedDataPath() {
  return test_shared;
}
std::string GetCustomResource(const char*, const char*) {
  return {};
}

static RimeApi* original_api = nullptr;
static RimeApi isolated_api{};
RimeApi* TestRimeGetApi() {
  return &isolated_api;
}

namespace {
constexpr UINT kBlockWindow = WM_APP + 1100;
constexpr UINT kStopWindow = WM_APP + 1101;
int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct Handler : weasel::RequestHandler {
  std::atomic<int> maintenance_checks{0};
  std::atomic<int> sessions{0};
  std::atomic<bool> running{false};
  void CheckMaintenance() override { ++maintenance_checks; }
  bool IsMaintenanceRunning() const override { return running; }
  DWORD AddSession(LPWSTR, EatLine) override {
    ++sessions;
    return 123;
  }
};

// Uses the production server, window handler and renderer on a private pipe.
// It never claims the installed service's singleton or opens real user data.
struct WindowFixture {
  Handler handler;
  weasel::UI ui;
  weasel::ServerImpl* server = nullptr;
  HWND window = nullptr;
  std::wstring pipe_name = L"\\\\.\\pipe\\weasel-window-concurrency-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64());
  HANDLE notification = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  HANDLE blocked = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  std::thread owner;

  static LRESULT CALLBACK Subclass(HWND window,
                                   UINT message,
                                   WPARAM wp,
                                   LPARAM lp,
                                   UINT_PTR,
                                   DWORD_PTR data) {
    auto& self = *reinterpret_cast<WindowFixture*>(data);
    if (message == WM_WEASEL_SERVICE_NOTIFY ||
        message == WM_WEASEL_MAINTENANCE_READY) {
      SetEvent(self.notification);
    } else if (message == kBlockWindow) {
      SetEvent(self.blocked);
      WaitForSingleObject(self.release, 2000);
      return 0;
    } else if (message == kStopWindow) {
      PostQuitMessage(0);
      return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
  }

  WindowFixture() {
    std::promise<HWND> started;
    auto ready = started.get_future();
    owner = std::thread([this, started = std::move(started)]() mutable {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      weasel::ServerImpl instance{std::wstring(pipe_name)};
      server = &instance;
      server->SetRequestHandler(&handler);
      HWND handle = server->Create(HWND_MESSAGE);
      SetWindowSubclass(handle, Subclass, 1, reinterpret_cast<DWORD_PTR>(this));
      ui.InServer() = true;
      ui.Create(handle);
      started.set_value(handle);
      server->Run();
      ui.Destroy(true);
      RemoveWindowSubclass(handle, Subclass, 1);
      if (server->IsWindow())
        server->DestroyWindow();
      CoUninitialize();
    });
    window = ready.get();
  }

  ~WindowFixture() {
    SetEvent(release);
    PostMessage(window, kStopWindow, 0, 0);
    owner.join();
    CloseHandle(notification);
    CloseHandle(blocked);
    CloseHandle(release);
  }
};

void TestOrdinaryNotification() {
  WindowFixture fixture;
  bool responsive = false;
  {
    std::lock_guard lock(g_api_mutex);
    PostMessage(fixture.window, WM_WEASEL_SERVICE_NOTIFY, 0, 0);
    Check(WaitForSingleObject(fixture.notification, 2000) == WAIT_OBJECT_0,
          "ordinary notification reaches the window thread");
    DWORD_PTR response = 0;
    responsive =
        SendMessageTimeout(fixture.window, WM_NULL, 0, 0,
                           SMTO_BLOCK | SMTO_ABORTIFHUNG, 200, &response) != 0;
  }
  Check(responsive, "ordinary notifications never wait for the input API lock");
  SendMessage(fixture.window, WM_NULL, 0, 0);
  Check(fixture.handler.maintenance_checks == 0,
        "ordinary notifications do not run maintenance checks");
}

void TestRenderingDoesNotWaitForWindow() {
  WindowFixture fixture;
  PostMessage(fixture.window, kBlockWindow, 0, 0);
  Check(WaitForSingleObject(fixture.blocked, 2000) == WAIT_OBJECT_0,
        "isolated window is deliberately busy");
  std::atomic<bool> returned{false};
  std::thread input([&] {
    fixture.ui.Refresh();
    fixture.ui.UpdateInputPosition(RECT{10, 10, 11, 30});
    fixture.ui.Hide();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const bool nonblocking = returned.load();
  SetEvent(fixture.release);
  input.join();
  Check(nonblocking, "server rendering never waits for a busy window thread");
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

void TestMaintenanceNotificationRetries() {
  WindowFixture fixture;
  {
    std::lock_guard lock(g_api_mutex);
    PostMessage(fixture.window, WM_WEASEL_MAINTENANCE_READY, 0, 0);
    Check(WaitForSingleObject(fixture.notification, 2000) == WAIT_OBJECT_0,
          "maintenance completion reaches the window thread");
    DWORD_PTR response = 0;
    Check(
        SendMessageTimeout(fixture.window, WM_NULL, 0, 0,
                           SMTO_BLOCK | SMTO_ABORTIFHUNG, 200, &response) != 0,
        "maintenance completion does not block the window on a busy API");
    Check(fixture.handler.maintenance_checks == 0,
          "maintenance completion is deferred while the API is busy");
  }
  Check(WaitUntil([&] { return fixture.handler.maintenance_checks == 1; }),
        "timer retries the deferred maintenance completion");
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  Check(fixture.handler.maintenance_checks == 1,
        "successful completion stops the retry timer");
}

void TestQueuedInputDuringMaintenance() {
  Handler handler;
  std::promise<void> started;
  std::promise<bool> finished;
  auto started_future = started.get_future();
  auto finished_future = finished.get_future();
  std::unique_lock busy(g_api_mutex);
  std::thread request([&] {
    started.set_value();
    auto lock = AcquireApiLock(&handler, WEASEL_IPC_PROCESS_KEY_EVENT);
    finished.set_value(!lock.owns_lock());
  });
  started_future.wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  handler.running = true;
  const bool fast = finished_future.wait_for(std::chrono::milliseconds(150)) ==
                    std::future_status::ready;
  busy.unlock();
  request.join();
  Check(fast && finished_future.get(),
        "already queued input returns unready when maintenance starts");
  auto control = AcquireApiLock(&handler, WEASEL_IPC_END_MAINTENANCE);
  Check(control.owns_lock(), "maintenance control requests remain available");
}

void TestIsolatedPipeDuringMaintenance() {
  WindowFixture fixture;
  fixture.handler.running = true;
  std::promise<void> connected;
  auto connected_future = connected.get_future();
  std::promise<DWORD> response;
  auto response_future = response.get_future();
  std::unique_lock busy(g_api_mutex);
  std::thread client([&] {
    weasel::PipeChannel<weasel::PipeMessage, DWORD> pipe{
        std::wstring(fixture.pipe_name)};
    const bool ready = WaitUntil([&] { return pipe.Connect(); });
    connected.set_value();
    if (ready) {
      weasel::PipeMessage request{WEASEL_IPC_START_SESSION, 0, 0};
      response.set_value(pipe.Transact(request));
      pipe.Disconnect();
    } else {
      response.set_value(999);
    }
  });
  connected_future.wait();
  const bool fast = response_future.wait_for(std::chrono::milliseconds(200)) ==
                    std::future_status::ready;
  busy.unlock();
  client.join();
  Check(fast && response_future.get() == 0,
        "isolated production pipe protocol returns unready while the API is "
        "busy");
}

void TestSlowClientDoesNotHoldApiLock() {
  WindowFixture fixture;
  HANDLE stalled = INVALID_HANDLE_VALUE;
  Check(WaitUntil([&] {
          stalled = CreateFile(fixture.pipe_name.c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
          return stalled != INVALID_HANDLE_VALUE;
        }),
        "slow client connects to its isolated pipe");
  if (stalled == INVALID_HANDLE_VALUE)
    return;
  weasel::PipeMessage request{WEASEL_IPC_START_SESSION, 0, 0};
  DWORD written = 0;
  Check(
      WriteFile(stalled, &request, sizeof(request), &written, nullptr) != FALSE,
      "slow client submits a request without reading the reply");
  Check(WaitUntil([&] { return fixture.handler.sessions == 1; }),
        "server processes the slow client's request");
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::promise<DWORD> response;
  auto response_future = response.get_future();
  std::thread client([&] {
    weasel::PipeChannel<weasel::PipeMessage, DWORD> pipe{
        std::wstring(fixture.pipe_name)};
    const bool connected = WaitUntil([&] { return pipe.Connect(); });
    response.set_value(connected ? pipe.Transact(request) : 999);
    pipe.Disconnect();
  });
  const bool fast = response_future.wait_for(std::chrono::milliseconds(200)) ==
                    std::future_status::ready;
  DWORD reply = 0, read = 0;
  ReadFile(stalled, &reply, sizeof(reply), &read, nullptr);
  CloseHandle(stalled);
  client.join();
  Check(fast && response_future.get() == 123,
        "a client not reading its reply cannot block other applications");
}

void TestShutdownWithConnectedClient(bool unread_reply) {
  auto fixture = std::make_unique<WindowFixture>();
  HANDLE client = INVALID_HANDLE_VALUE;
  Check(WaitUntil([&] {
          client = CreateFile(fixture->pipe_name.c_str(),
                              GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
          return client != INVALID_HANDLE_VALUE;
        }),
        "shutdown client connects to its isolated pipe");
  if (client == INVALID_HANDLE_VALUE)
    return;
  if (unread_reply) {
    weasel::PipeMessage request{WEASEL_IPC_START_SESSION, 0, 0};
    DWORD written = 0;
    WriteFile(client, &request, sizeof(request), &written, nullptr);
    Check(WaitUntil([&] { return fixture->handler.sessions == 1; }),
          "shutdown client leaves a reply unread");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::atomic<HANDLE> client_handle{client};
  std::atomic<bool> forced{false};
  HANDLE finished = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  // The watchdog bounds a regression without leaving a hung test process.
  std::thread watchdog([&] {
    if (WaitForSingleObject(finished, 500) == WAIT_TIMEOUT) {
      forced = true;
      CloseHandle(client_handle.exchange(INVALID_HANDLE_VALUE));
    }
  });
  fixture.reset();
  SetEvent(finished);
  watchdog.join();
  const HANDLE remaining = client_handle.exchange(INVALID_HANDLE_VALUE);
  if (remaining != INVALID_HANDLE_VALUE)
    CloseHandle(remaining);
  CloseHandle(finished);
  Check(!forced, unread_reply ? "shutdown cancels a blocked pipe reply"
                              : "shutdown cancels an idle pipe read");
}

HANDLE join_entered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
HANDLE allow_join = CreateEvent(nullptr, TRUE, FALSE, nullptr);
std::atomic<int> joins{0};
std::atomic<bool> fail_initialize{true};

void InitializeIsolated(RimeTraits* traits) {
  if (fail_initialize.exchange(false))
    throw std::runtime_error("isolated initialization failure");
  original_api->initialize(traits);
}

void SetupIsolated(RimeTraits* traits) {
  traits->log_dir = "";
  traits->min_log_level = 2;
  original_api->setup(traits);
}

void JoinIsolated() {
  ++joins;
  SetEvent(join_entered);
  WaitForSingleObject(allow_join, 3000);
  original_api->join_maintenance_thread();
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void TestRealBackgroundMaintenance() {
  const auto root =
      fs::current_path() / L"msbuild" /
      (L"service-concurrency-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  test_user = root / L"user";
  test_shared = root / L"shared";
  fs::create_directories(test_user);
  Write(test_shared / L"default.yaml",
        "config_version: '1'\nschema_list:\n  - schema: test\nmenu:\n  "
        "page_size: 3\n");
  Write(test_shared / L"weasel.yaml",
        "config_version: '1'\nstyle:\n  font_point: 12\n");
  Write(
      test_shared / L"test.schema.yaml",
      "schema:\n  schema_id: test\n  name: Test\n  version: '1'\n"
      "engine:\n  processors: [speller, selector, navigator, express_editor]\n"
      "  segmentors: [abc_segmentor]\n  translators: [script_translator]\n"
      "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n"
      "translator:\n  dictionary: test\n");
  Write(test_shared / L"test.dict.yaml",
        u8"---\nname: test\nversion: '1'\nsort: "
        u8"by_weight\n...\n甲\tjia\t1\n乙\tyi\t1\n");
  WindowFixture fixture;
  original_api = rime_get_api();
  isolated_api = *original_api;
  isolated_api.setup = SetupIsolated;
  isolated_api.initialize = InitializeIsolated;
  isolated_api.join_maintenance_thread = JoinIsolated;
  {
    RimeWithWeaselHandler handler(&fixture.ui);
    fixture.server->SetRequestHandler(&handler);
    handler.OnMaintenanceReady([window = fixture.window] {
      PostMessage(window, WM_WEASEL_MAINTENANCE_READY, 0, 0);
    });
    const auto started = std::chrono::steady_clock::now();
    handler.Initialize();
    Check(std::chrono::steady_clock::now() - started <
              std::chrono::milliseconds(200),
          "initialization returns without waiting on the window thread");
    Check(WaitUntil([&] { return !handler.IsMaintenanceRunning(); }),
          "failed background initialization releases the busy state");
    Check(!fs::exists(test_user / L"build" / L".weasel-deployment-crc32"),
          "failed initialization cannot record a deployment baseline");
    const auto retry_started = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(g_api_mutex);
      handler.EndMaintenance();
    }
    Check(std::chrono::steady_clock::now() - retry_started <
              std::chrono::milliseconds(200),
          "retry returns before real dictionary compilation finishes");
    Check(WaitForSingleObject(join_entered, 5000) == WAIT_OBJECT_0,
          "real Rime deployment reaches the background maintenance wait");
    Check(handler.IsMaintenanceRunning(),
          "service remains unready during deployment");
    auto input = AcquireApiLock(&handler, WEASEL_IPC_START_SESSION);
    Check(!input.owns_lock(),
          "new sessions return unready during real deployment");
    DWORD_PTR response = 0;
    Check(
        SendMessageTimeout(fixture.window, WM_NULL, 0, 0,
                           SMTO_BLOCK | SMTO_ABORTIFHUNG, 200, &response) != 0,
        "window remains responsive during real deployment");
    SetEvent(allow_join);
    Check(WaitUntil([&] { return !handler.IsMaintenanceRunning(); }),
          "completion notification automatically restores the service");
    const auto prism = test_user / L"build" / L"test.prism.bin";
    Check(fs::is_regular_file(prism),
          "background deployment produces a real spelling index");
    Check(
        fs::is_regular_file(test_user / L"build" / L".weasel-deployment-crc32"),
        "successful background deployment records the CRC32 baseline");
    if (fs::exists(prism)) {
      const auto built_time = fs::last_write_time(prism);
      const int compiled = joins;
      {
        auto lock = AcquireApiLock(&handler, WEASEL_IPC_START_SESSION);
        wchar_t client[] =
            L"session.client_app=isolated-test.exe\nsession.client_type=tsf\n."
            L"\n";
        const DWORD session = handler.AddSession(client);
        Check(session != 0,
              "real sessions are available after background deployment");
        handler.RemoveSession(session);
        handler.StartMaintenance();
        handler.EndMaintenance();
      }
      Check(WaitUntil([&] { return !handler.IsMaintenanceRunning(); }),
            "unchanged synchronization automatically resumes");
      Check(joins == compiled && fs::last_write_time(prism) == built_time,
            "unchanged content resumes without recompiling dictionaries");
      const auto custom = test_user / L"default.custom.yaml";
      Write(custom, "patch:\n  menu/page_size: 5\n");
      {
        std::lock_guard lock(g_api_mutex);
        handler.StartMaintenance();
        handler.EndMaintenance();
      }
      Check(WaitUntil([&] { return !handler.IsMaintenanceRunning(); }),
            "changed content finishes background deployment");
      Check(joins > compiled,
            "changed content triggers real background maintenance");
      RimeConfig config{};
      int page_size = 0;
      if (original_api->config_open("default", &config)) {
        original_api->config_get_int(&config, "menu/page_size", &page_size);
        original_api->config_close(&config);
      }
      Check(page_size == 5,
            "changed configuration is actually compiled and loaded");

      const auto vocabulary_session = original_api->create_session();
      Check(vocabulary_session &&
                original_api->select_schema(vocabulary_session, "test") &&
                original_api->simulate_key_sequence(vocabulary_session, "jia "),
            "isolated Rime records personal vocabulary");
      Check(original_api->sync_user_data(),
            "isolated personal vocabulary synchronization starts");
      original_api->join_maintenance_thread();
      original_api->destroy_session(vocabulary_session);
      const int after_sync = joins;
      {
        std::lock_guard lock(g_api_mutex);
        handler.StartMaintenance();
        handler.EndMaintenance();
      }
      Check(
          WaitUntil([&] { return !handler.IsMaintenanceRunning(); }) &&
              joins == after_sync,
          "personal vocabulary sync resumes without dictionary recompilation");

      ResetEvent(join_entered);
      ResetEvent(allow_join);
      Write(custom, "patch:\n  menu/page_size: 7\n");
      {
        std::lock_guard lock(g_api_mutex);
        handler.StartMaintenance();
        handler.EndMaintenance();
      }
      Check(WaitForSingleObject(join_entered, 5000) == WAIT_OBJECT_0,
            "shutdown test reaches an active real maintenance worker");
      std::thread release_worker([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SetEvent(allow_join);
      });
      {
        std::lock_guard lock(g_api_mutex);
        handler.Finalize();
      }
      release_worker.join();
      Check(!handler.IsMaintenanceRunning(),
            "shutdown cancels and joins active maintenance");
    }
    {
      std::lock_guard lock(g_api_mutex);
      handler.Finalize();
      fixture.server->SetRequestHandler(&fixture.handler);
    }
    Check(!handler.IsMaintenanceRunning(),
          "finalization joins the maintenance worker");
  }
  const auto resolved = fs::weakly_canonical(root);
  if (resolved.parent_path() !=
          fs::canonical(fs::current_path()) / L"msbuild" ||
      resolved.filename().wstring().find(L"service-concurrency-") != 0) {
    throw std::runtime_error("unsafe fixture cleanup path");
  }
  fs::remove_all(resolved);
}
}  // namespace

int main() {
  _Module.Init(nullptr, GetModuleHandle(nullptr));
  TestOrdinaryNotification();
  TestRenderingDoesNotWaitForWindow();
  TestMaintenanceNotificationRetries();
  TestQueuedInputDuringMaintenance();
  TestIsolatedPipeDuringMaintenance();
  TestSlowClientDoesNotHoldApiLock();
  TestShutdownWithConnectedClient(false);
  TestShutdownWithConnectedClient(true);
  TestRealBackgroundMaintenance();
  CloseHandle(join_entered);
  CloseHandle(allow_join);
  _Module.Term();
  if (!failures) {
    std::cout << "Service concurrency tests passed.\n";
  }
  return failures ? 1 : 0;
}
