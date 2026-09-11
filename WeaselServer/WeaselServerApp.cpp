#include "stdafx.h"
#include "WeaselServerApp.h"
#include <filesystem>

WeaselServerApp::WeaselServerApp()
    : tray_icon(m_ui),
      m_handler(std::make_unique<RimeWithWeaselHandler>(&m_ui)) {
  // m_handler.reset(new RimeWithWeaselHandler(&m_ui));
  m_server.SetRequestHandler(m_handler.get());
  try {
    tray_icon.SetStatisticsProvider(
        [this]() { return m_statistics.GetSummary(); });
    m_server.SetStatisticsSummaryProvider([this](DWORD& overview_units) {
      return m_statistics.TryGetTodayOverview(overview_units);
    });
    m_server.SetStatisticsSyncHandler([this]() {
      return m_statistics.TryEnqueueSync(m_handler->GetSyncDir());
    });
  } catch (...) {
    // Statistics UI must not prevent the input service from starting.
  }
  SetupMenuHandlers();
}

WeaselServerApp::~WeaselServerApp() {}

int WeaselServerApp::Run() {
  if (!m_server.Start())
    return -1;

  // win_sparkle_set_appcast_url("http://localhost:8000/weasel/update/appcast.xml");
  win_sparkle_set_registry_path("Software\\Rime\\Weasel\\Updates");
  if (GetThreadUILanguage() ==
      MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
    win_sparkle_set_lang("zh-TW");
  else if (GetThreadUILanguage() ==
           MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
    win_sparkle_set_lang("zh-CN");
  else
    win_sparkle_set_lang("en");
  win_sparkle_init();
  m_ui.Create(m_server.GetHWnd());

  m_handler->Initialize();
  m_handler->OnUpdateUI([this]() { tray_icon.RequestRefresh(); });

  tray_icon.Create(m_server.GetHWnd());
  m_server.SetTrayRefreshCallback([this]() {
    tray_icon.ApplyRefresh();
    if (m_statistics.ConsumeFailureNotification()) {
      tray_icon.ShowStatisticsFailure();
    } else if (m_statistics.ConsumeSyncFailureNotification()) {
      tray_icon.ShowStatisticsSyncFailure();
    }
  });
  tray_icon.RequestRefresh();
  try {
    m_handler->OnCommit(
        [this](const char* text) { m_statistics.TryEnqueueCommit(text); });
    m_handler->OnCorrection([this](std::uint32_t backspaces,
                                   std::uint32_t deleted_ascii_letters) {
      m_statistics.TryEnqueueCorrection(backspaces, deleted_ascii_letters);
    });
    m_statistics.Start(m_handler->GetUserId(), install_dir(),
                       m_server.GetHWnd(),
                       WM_WEASEL_SERVICE_NOTIFY);
  } catch (...) {
    tray_icon.ShowStatisticsFailure();
  }

  int ret = m_server.Run();

  m_statistics.Stop();
  tray_icon.DisableRefresh();
  m_handler->Finalize();
  m_ui.Destroy();
  tray_icon.RemoveIcon();
  win_sparkle_cleanup();

  return ret;
}

void WeaselServerApp::SetupMenuHandlers() {
  std::filesystem::path dir = install_dir();
  m_server.AddMenuHandler(ID_WEASELTRAY_QUIT,
                          [this] { return m_server.Stop() == 0; });
  m_server.AddMenuHandler(ID_WEASELTRAY_DEPLOY,
                          std::bind(execute, dir / L"WeaselDeployer.exe",
                                    std::wstring(L"/deploy")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SETTINGS,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring()));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_DICT_MANAGEMENT,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/dict")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SYNC,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/sync")));
  m_server.AddMenuHandler(ID_WEASELTRAY_STATS_SUMMARY, [this, dir]() {
    try {
      if (execute(dir / L"WeaselStats.exe", L"--view")) {
        return true;
      }
    } catch (...) {
    }
    tray_icon.ShowStatisticsViewFailure();
    return false;
  });
  m_server.AddMenuHandler(ID_WEASELTRAY_WIKI,
                          std::bind(open, L"https://rime.im/docs/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_HOMEPAGE,
                          std::bind(open, L"https://rime.im/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_FORUM,
                          std::bind(open, L"https://rime.im/discuss/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_CHECKUPDATE, check_update);
  m_server.AddMenuHandler(ID_WEASELTRAY_INSTALLDIR, std::bind(explore, dir));
  m_server.AddMenuHandler(ID_WEASELTRAY_USERCONFIG,
                          std::bind(explore, WeaselUserDataPath()));
  m_server.AddMenuHandler(ID_WEASELTRAY_LOGDIR,
                          std::bind(explore, WeaselLogPath()));
}
