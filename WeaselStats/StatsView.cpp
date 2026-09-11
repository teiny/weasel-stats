#include "StatsView.h"

#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <resource.h>

#include "StatsReport.h"
#include "StatsResources.h"
#include "WinSqlite.h"

namespace weasel::stats {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"WeaselStatsViewWindow";
constexpr wchar_t kWindowTitle[] = L"小狼毫输入统计";
constexpr wchar_t kAppOrigin[] = L"https://weasel.stats";
constexpr int kClientWidth = 1120;
constexpr int kClientHeight = 780;
constexpr DWORD kWindowStyle =
    WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

std::wstring ViewMutexName() {
  DWORD session_id = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  return L"Local\\WeaselStatsView-" + std::to_wstring(session_id);
}

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring result(length, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          length) != length) {
    return {};
  }
  return result;
}

int HexValue(wchar_t character) {
  if (character >= L'0' && character <= L'9') {
    return character - L'0';
  }
  if (character >= L'a' && character <= L'f') {
    return character - L'a' + 10;
  }
  if (character >= L'A' && character <= L'F') {
    return character - L'A' + 10;
  }
  return -1;
}

bool PercentDecode(std::wstring_view value, std::string& decoded) {
  decoded.clear();
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const wchar_t character = value[index];
    if (character == L'%') {
      if (index + 2 >= value.size()) {
        return false;
      }
      const int high = HexValue(value[index + 1]);
      const int low = HexValue(value[index + 2]);
      if (high < 0 || low < 0) {
        return false;
      }
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else if (character <= 0x7f) {
      decoded.push_back(static_cast<char>(character));
    } else {
      return false;
    }
  }
  return !decoded.empty();
}

bool ParseInteger(std::wstring_view value, int& parsed) {
  if (value.empty() || value.size() > 8) {
    return false;
  }
  int result = 0;
  for (const wchar_t character : value) {
    if (character < L'0' || character > L'9') {
      return false;
    }
    result = result * 10 + character - L'0';
  }
  parsed = result;
  return true;
}

bool ParseQuery(std::wstring_view message, ReportQuery& query) {
  if (message.size() > 8192 || message.rfind(L"query|", 0) != 0) {
    return false;
  }
  std::vector<std::wstring_view> parts;
  std::size_t start = 0;
  while (start <= message.size()) {
    const std::size_t separator = message.find(L'|', start);
    parts.push_back(message.substr(
        start, separator == std::wstring_view::npos ? message.size() - start
                                                    : separator - start));
    if (separator == std::wstring_view::npos) {
      break;
    }
    start = separator + 1;
  }
  if (parts.size() != 4 || parts[0] != L"query") {
    return false;
  }
  if (parts[1] == L"day") {
    query.granularity = ReportGranularity::kDay;
  } else if (parts[1] == L"month") {
    query.granularity = ReportGranularity::kMonth;
  } else if (parts[1] == L"year") {
    query.granularity = ReportGranularity::kYear;
  } else {
    return false;
  }
  if (!ParseInteger(parts[2], query.anchor)) {
    return false;
  }

  query.device_ids.clear();
  if (parts[3] == L"all") {
    return true;
  }
  std::size_t device_start = 0;
  while (device_start <= parts[3].size()) {
    const std::size_t separator = parts[3].find(L',', device_start);
    const std::wstring_view encoded = parts[3].substr(
        device_start,
        separator == std::wstring_view::npos ? parts[3].size() - device_start
                                             : separator - device_start);
    std::string device;
    if (!PercentDecode(encoded, device) || device.size() > 256) {
      return false;
    }
    query.device_ids.push_back(std::move(device));
    if (query.device_ids.size() > 32 ||
        separator == std::wstring_view::npos) {
      break;
    }
    device_start = separator + 1;
  }
  return !query.device_ids.empty() && query.device_ids.size() <= 32;
}

std::filesystem::path WebViewDataDirectory() {
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (!length) {
    return {};
  }
  std::vector<wchar_t> buffer(length);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), length) + 1 !=
      length) {
    return {};
  }
  return std::filesystem::path(buffer.data()) / L"Rime" / L"WeaselStats" /
         L"WebView2";
}

HRESULT ResourceStream(int resource_id, IStream** stream) {
  *stream = nullptr;
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
  if (!resource) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void* bytes = loaded ? LockResource(loaded) : nullptr;
  if (!bytes || !size) {
    return E_FAIL;
  }
  HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!copy) {
    return E_OUTOFMEMORY;
  }
  void* destination = GlobalLock(copy);
  if (!destination) {
    GlobalFree(copy);
    return E_OUTOFMEMORY;
  }
  memcpy(destination, bytes, size);
  GlobalUnlock(copy);
  const HRESULT result = CreateStreamOnHGlobal(copy, TRUE, stream);
  if (FAILED(result)) {
    GlobalFree(copy);
  }
  return result;
}

class StatsViewWindow {
 public:
  StatsViewWindow(HINSTANCE instance, std::filesystem::path database_path)
      : instance_(instance),
        report_(sqlite_, std::move(database_path)) {}

  ~StatsViewWindow() {
    if (controller_) {
      controller_->Close();
    }
  }

  bool Create(int show_command) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_WEASEL));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }

    RECT bounds{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&bounds, kWindowStyle, FALSE, 0);
    window_ = CreateWindowExW(
        0, kWindowClass, kWindowTitle, kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance_, this);
    if (!window_) {
      return false;
    }
    ShowWindow(window_, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    UpdateWindow(window_);
    sqlite_.Load();
    InitializeWebView();
    return true;
  }

  int Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) {
    StatsViewWindow* self = reinterpret_cast<StatsViewWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<StatsViewWindow*>(create->lpCreateParams);
      self->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
  }

  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_SIZE:
        ResizeWebView();
        return 0;
      case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_, nullptr, bounds->left, bounds->top,
                     bounds->right - bounds->left,
                     bounds->bottom - bounds->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
      }
      case WM_PAINT:
        PaintLoadingMessage();
        return 0;
      case WM_DESTROY:
        if (controller_) {
          controller_->Close();
          controller_.Reset();
          webview_.Reset();
          environment_.Reset();
        }
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
  }

  void PaintLoadingMessage() {
    PAINTSTRUCT paint{};
    HDC device = BeginPaint(window_, &paint);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(device, L"正在加载输入统计…", -1, &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(window_, &paint);
  }

  void ResizeWebView() {
    if (!controller_ || !window_) {
      return;
    }
    RECT bounds{};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
  }

  void ShowFailure(const wchar_t* message) {
    if (!IsWindow(window_) || failure_shown_) {
      return;
    }
    failure_shown_ = true;
    MessageBoxW(window_, message, kWindowTitle, MB_OK | MB_ICONWARNING);
    DestroyWindow(window_);
  }

  void InitializeWebView() {
    std::filesystem::path user_data = WebViewDataDirectory();
    if (user_data.empty()) {
      ShowFailure(L"无法创建统计视图的数据目录，输入统计仍会继续运行。");
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(user_data, error);
    if (error) {
      ShowFailure(L"无法创建统计视图的数据目录，输入统计仍会继续运行。");
      return;
    }

    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT status,
                   ICoreWebView2Environment* environment) -> HRESULT {
              if (FAILED(status) || !environment || !IsWindow(window_)) {
                ShowFailure(
                    L"无法启动 WebView2 统计视图。请确认已安装 Microsoft "
                    L"Edge WebView2 Runtime；输入统计仍会继续运行。");
                return S_OK;
              }
              environment_ = environment;
              const HRESULT create_result = environment_->CreateCoreWebView2Controller(
                  window_,
                  Callback<
                      ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                      [this](HRESULT controller_status,
                             ICoreWebView2Controller* controller) -> HRESULT {
                        return OnControllerCreated(controller_status,
                                                   controller);
                      })
                      .Get());
              if (FAILED(create_result)) {
                ShowFailure(
                    L"无法创建统计视图，输入统计仍会继续运行。");
              }
              return S_OK;
            })
            .Get());
    if (FAILED(result)) {
      ShowFailure(
          L"无法启动 WebView2 统计视图。请确认已安装 Microsoft Edge "
          L"WebView2 Runtime；输入统计仍会继续运行。");
    }
  }

  HRESULT OnControllerCreated(HRESULT status,
                              ICoreWebView2Controller* controller) {
    if (FAILED(status) || !controller || !IsWindow(window_)) {
      ShowFailure(L"无法创建统计视图，输入统计仍会继续运行。");
      return S_OK;
    }
    controller_ = controller;
    if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
      ShowFailure(L"无法创建统计视图，输入统计仍会继续运行。");
      return S_OK;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
      settings->put_IsScriptEnabled(TRUE);
      settings->put_IsWebMessageEnabled(TRUE);
      settings->put_AreDefaultScriptDialogsEnabled(FALSE);
      settings->put_AreDefaultContextMenusEnabled(FALSE);
      settings->put_AreDevToolsEnabled(FALSE);
      settings->put_IsStatusBarEnabled(FALSE);
      settings->put_IsZoomControlEnabled(FALSE);
    }

    ResizeWebView();
    webview_->AddWebResourceRequestedFilter(
        L"https://weasel.stats/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview_->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebResourceRequestedEventArgs* arguments) {
              return HandleResourceRequest(arguments);
            })
            .Get(),
        &resource_token_);
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2NavigationStartingEventArgs* arguments) {
              LPWSTR uri = nullptr;
              if (SUCCEEDED(arguments->get_Uri(&uri)) && uri) {
                const std::wstring_view value(uri);
                const std::wstring_view origin(kAppOrigin);
                const bool local =
                    value == origin ||
                    (value.size() > origin.size() &&
                     value[origin.size()] == L'/' &&
                     value.rfind(origin, 0) == 0);
                if (!local) {
                  arguments->put_Cancel(TRUE);
                }
              }
              CoTaskMemFree(uri);
              return S_OK;
            })
            .Get(),
        &navigation_token_);
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebMessageReceivedEventArgs* arguments) {
              return HandleWebMessage(arguments);
            })
            .Get(),
        &message_token_);
    return webview_->Navigate(L"https://weasel.stats/index.html");
  }

  HRESULT HandleResourceRequest(
      ICoreWebView2WebResourceRequestedEventArgs* arguments) {
    if (!environment_ || !arguments) {
      return E_FAIL;
    }
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(arguments->get_Request(&request)) || !request) {
      return E_FAIL;
    }
    LPWSTR raw_uri = nullptr;
    if (FAILED(request->get_Uri(&raw_uri)) || !raw_uri) {
      CoTaskMemFree(raw_uri);
      return E_FAIL;
    }
    std::wstring uri(raw_uri);
    CoTaskMemFree(raw_uri);
    const std::size_t query = uri.find_first_of(L"?#");
    if (query != std::wstring::npos) {
      uri.resize(query);
    }

    int resource_id = 0;
    const wchar_t* content_type = L"text/plain; charset=utf-8";
    if (uri == L"https://weasel.stats/index.html" ||
        uri == L"https://weasel.stats/") {
      resource_id = IDR_STATS_INDEX_HTML;
      content_type = L"text/html; charset=utf-8";
    } else if (uri == L"https://weasel.stats/styles.css") {
      resource_id = IDR_STATS_STYLES_CSS;
      content_type = L"text/css; charset=utf-8";
    } else if (uri == L"https://weasel.stats/app.js") {
      resource_id = IDR_STATS_APP_JS;
      content_type = L"application/javascript; charset=utf-8";
    } else if (uri == L"https://weasel.stats/echarts.min.js") {
      resource_id = IDR_STATS_ECHARTS_JS;
      content_type = L"application/javascript; charset=utf-8";
    }

    ComPtr<IStream> stream;
    int status = 200;
    const wchar_t* reason = L"OK";
    if (!resource_id || FAILED(ResourceStream(resource_id, &stream))) {
      status = 404;
      reason = L"Not Found";
      static constexpr char kNotFound[] = "Not Found";
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(kNotFound) - 1);
      if (!memory) {
        return E_OUTOFMEMORY;
      }
      void* destination = GlobalLock(memory);
      if (!destination) {
        GlobalFree(memory);
        return E_OUTOFMEMORY;
      }
      memcpy(destination, kNotFound, sizeof(kNotFound) - 1);
      GlobalUnlock(memory);
      if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return E_FAIL;
      }
    }

    const std::wstring headers =
        std::wstring(L"Content-Type: ") + content_type +
        L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff";
    ComPtr<ICoreWebView2WebResourceResponse> response;
    const HRESULT result = environment_->CreateWebResourceResponse(
        stream.Get(), status, reason, headers.c_str(), &response);
    if (FAILED(result)) {
      return result;
    }
    return arguments->put_Response(response.Get());
  }

  HRESULT HandleWebMessage(
      ICoreWebView2WebMessageReceivedEventArgs* arguments) {
    if (!arguments) {
      return E_INVALIDARG;
    }
    LPWSTR raw_message = nullptr;
    if (FAILED(arguments->TryGetWebMessageAsString(&raw_message)) ||
        !raw_message) {
      CoTaskMemFree(raw_message);
      return S_OK;
    }
    const std::wstring message(raw_message);
    CoTaskMemFree(raw_message);

    ReportQuery query;
    if (message == L"ready") {
      SendReport(query);
    } else if (ParseQuery(message, query)) {
      SendReport(query);
    }
    return S_OK;
  }

  void SendReport(const ReportQuery& query) {
    if (!webview_) {
      return;
    }
    std::string json;
    if (!report_.BuildJson(query, json)) {
      json =
          "{\"type\":\"report\",\"available\":false,"
          "\"granularity\":\"day\",\"anchor\":0,"
          "\"previousAnchor\":0,\"nextAnchor\":0,"
          "\"weekdayOffset\":0,\"title\":\"统计数据暂不可用\","
          "\"allDevices\":true,\"devices\":[],\"points\":[]}";
    }
    const std::wstring wide_json = Utf8ToWide(json);
    if (!wide_json.empty()) {
      webview_->PostWebMessageAsJson(wide_json.c_str());
    }
  }

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  bool failure_shown_ = false;
  WinSqlite sqlite_;
  StatsReport report_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  EventRegistrationToken resource_token_{};
  EventRegistrationToken navigation_token_{};
  EventRegistrationToken message_token_{};
};

}  // namespace

int RunStatsView(HINSTANCE instance,
                 const std::filesystem::path& data_directory) noexcept {
  HANDLE mutex = nullptr;
  bool uninitialize_com = false;
  try {
    mutex = CreateMutexW(nullptr, TRUE, ViewMutexName().c_str());
    if (!mutex) {
      return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
      }
      CloseHandle(mutex);
      return 0;
    }

    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
      return 1;
    }
    uninitialize_com = true;

    int result = 1;
    {
      StatsViewWindow view(
          instance, data_directory / L"weasel-input-statistics.sqlite3");
      if (view.Create(SW_SHOWNORMAL)) {
        result = view.Run();
      }
    }
    CoUninitialize();
    uninitialize_com = false;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
  } catch (...) {
    if (uninitialize_com) {
      CoUninitialize();
    }
    if (mutex) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
    }
    return 1;
  }
}

}  // namespace weasel::stats
