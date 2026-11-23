#include "stdafx.h"
#include <WeaselUI.h>
#include "WeaselPanel.h"
#include <commctrl.h>
#include <atomic>
#include <mutex>

#pragma comment(lib, "comctl32.lib")

using namespace weasel;

class weasel::UIImpl {
 public:
  enum class Visibility { Unchanged, Show, Hide, Timed };
  struct PendingUpdate {
    Context context;
    Status status;
    UIStyle style;
    RECT position{};
    Visibility visibility = Visibility::Unchanged;
    size_t timeout = 0;
    bool refresh = false;
    bool move = false;
    bool posted = false;
  };

  // Server API threads own the source model. Only the window thread reads or
  // writes this separate rendering model; no window call waits on the API lock.
  std::unique_ptr<UI> rendering;
  WeaselPanel panel;

  static std::unique_ptr<UI> RenderingModel(UI& source) {
    if (!source.InServer())
      return nullptr;
    auto model = std::make_unique<UI>();
    model->InServer() = true;
    model->ctx() = source.ctx();
    model->status() = source.status();
    model->style() = source.style();
    model->SetUICallBack(source.uiCallback());
    return model;
  }

  UIImpl(weasel::UI& ui)
      : rendering(RenderingModel(ui)), panel(rendering ? *rendering : ui) {}
  ~UIImpl() {}

  void Queue(UI& source,
             bool refresh = false,
             const RECT* position = nullptr,
             Visibility visibility = Visibility::Unchanged,
             size_t timeout = 0) {
    std::lock_guard lock(pending_mutex);
    if (!accepting)
      return;
    pending.context = source.ctx();
    pending.status = source.status();
    pending.style = source.style();
    pending.refresh = pending.refresh || refresh;
    if (position) {
      pending.position = *position;
      pending.move = true;
    }
    if (visibility != Visibility::Unchanged) {
      pending.visibility = visibility;
      pending.timeout = timeout;
    }
    if (!pending.posted) {
      pending.posted = true;
      if (!panel.PostMessage(kRenderMessage))
        pending.posted = false;
    }
  }

  void ApplyPending() {
    PendingUpdate update;
    {
      std::lock_guard lock(pending_mutex);
      if (!accepting || !pending.posted)
        return;
      update = std::move(pending);
      pending = PendingUpdate{};
    }
    rendering->ctx() = std::move(update.context);
    rendering->status() = std::move(update.status);
    rendering->style() = std::move(update.style);
    if (update.refresh)
      Refresh();
    if (update.move)
      panel.MoveTo(update.position);
    reposition = panel.GetIsReposition();
    switch (update.visibility) {
      case Visibility::Show:
        Show();
        break;
      case Visibility::Hide:
        Hide();
        break;
      case Visibility::Timed:
        ShowWithTimeout(update.timeout);
        break;
      default:
        break;
    }
  }

  static LRESULT CALLBACK Dispatch(HWND window,
                                   UINT message,
                                   WPARAM wp,
                                   LPARAM lp,
                                   UINT_PTR,
                                   DWORD_PTR data) {
    if (message == kRenderMessage) {
      reinterpret_cast<UIImpl*>(data)->ApplyPending();
      return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
  }

  void StartDispatch() {
    accepting = true;
    SetProp(panel.m_hWnd, L"WeaselUI.Owner", this);
    if (rendering)
      SetWindowSubclass(panel.m_hWnd, Dispatch, 1,
                        reinterpret_cast<DWORD_PTR>(this));
  }

  void StopDispatch() {
    {
      std::lock_guard lock(pending_mutex);
      accepting = false;
      pending = PendingUpdate{};
    }
    if (rendering)
      RemoveWindowSubclass(panel.m_hWnd, Dispatch, 1);
    RemoveProp(panel.m_hWnd, L"WeaselUI.Owner");
    Hide();
  }
  void Refresh() {
    if (!panel.IsWindow())
      return;
    if (timer) {
      Hide();
    }
    panel.Refresh();
  }
  void Show();
  void Hide();
  void ShowWithTimeout(size_t millisec);
  bool IsShown() const { return shown; }

  static VOID CALLBACK OnTimer(_In_ HWND hwnd,
                               _In_ UINT uMsg,
                               _In_ UINT_PTR idEvent,
                               _In_ DWORD dwTime);
  static constexpr UINT kRenderMessage = WM_APP + 1042;
  UINT_PTR timer = 0;
  std::atomic<bool> shown{false};
  std::atomic<bool> counting{false};
  std::atomic<bool> reposition{false};
  std::mutex pending_mutex;
  PendingUpdate pending;
  bool accepting = true;
};

void UIImpl::Show() {
  if (!panel.IsWindow())
    return;
  panel.ShowWindow(SW_SHOWNA);
  shown = true;
  if (timer) {
    KillTimer(panel.m_hWnd, timer);
    timer = 0;
  }
  counting = false;
}

void UIImpl::Hide() {
  if (!panel.IsWindow())
    return;
  panel.ShowWindow(SW_HIDE);
  shown = false;
  if (timer) {
    KillTimer(panel.m_hWnd, timer);
    timer = 0;
  }
  counting = false;
}

void UIImpl::ShowWithTimeout(size_t millisec) {
  if (!panel.IsWindow())
    return;
  DLOG(INFO) << "ShowWithTimeout: " << millisec;
  panel.ShowWindow(SW_SHOWNA);
  shown = true;
  timer = SetTimer(panel.m_hWnd, reinterpret_cast<UINT_PTR>(this),
                   static_cast<UINT>(millisec), &UIImpl::OnTimer);
  counting = timer != 0;
}
VOID CALLBACK UIImpl::OnTimer(_In_ HWND hwnd,
                              _In_ UINT uMsg,
                              _In_ UINT_PTR idEvent,
                              _In_ DWORD dwTime) {
  DLOG(INFO) << "OnTimer:";
  KillTimer(hwnd, idEvent);
  UIImpl* self = reinterpret_cast<UIImpl*>(GetProp(hwnd, L"WeaselUI.Owner"));
  if (self && self->timer == idEvent) {
    self->Hide();
  }
}

bool UI::Create(HWND parent) {
  if (pimpl_) {
    pimpl_->panel.Create(
        parent, 0, 0, WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        0U, 0);
    pimpl_->StartDispatch();
    return true;
  }

  pimpl_ = new UIImpl(*this);
  if (!pimpl_)
    return false;

  pimpl_->panel.Create(
      parent, 0, 0, WS_POPUP,
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      0U, 0);
  pimpl_->StartDispatch();
  return true;
}

void UI::Destroy(bool full) {
  if (pimpl_) {
    // destroy panel
    if (pimpl_->panel.IsWindow()) {
      pimpl_->StopDispatch();
      pimpl_->panel.DestroyWindow();
    }
    if (full) {
      delete pimpl_;
      pimpl_ = 0;
      pDWR.reset();
    }
  }
}

bool UI::GetIsReposition() {
  if (pimpl_)
    return pimpl_->rendering ? pimpl_->reposition.load()
                             : pimpl_->panel.GetIsReposition();
  else
    return false;
}

void UI::Show() {
  if (pimpl_) {
    if (pimpl_->rendering)
      pimpl_->Queue(*this, false, nullptr, UIImpl::Visibility::Show);
    else
      pimpl_->Show();
  }
}

void UI::Hide() {
  if (pimpl_) {
    if (pimpl_->rendering)
      pimpl_->Queue(*this, false, nullptr, UIImpl::Visibility::Hide);
    else
      pimpl_->Hide();
  }
}

void UI::ShowWithTimeout(size_t millisec) {
  if (pimpl_) {
    if (pimpl_->rendering)
      pimpl_->Queue(*this, false, nullptr, UIImpl::Visibility::Timed, millisec);
    else
      pimpl_->ShowWithTimeout(millisec);
  }
}

bool UI::IsCountingDown() const {
  return pimpl_ && pimpl_->counting;
}

bool UI::IsShown() const {
  return pimpl_ && pimpl_->IsShown();
}

void UI::Refresh() {
  if (pimpl_) {
    if (pimpl_->rendering)
      pimpl_->Queue(*this, true);
    else
      pimpl_->Refresh();
  }
}

void UI::UpdateInputPosition(RECT const& rc) {
  if (pimpl_ && pimpl_->panel.IsWindow()) {
    if (pimpl_->rendering)
      pimpl_->Queue(*this, false, &rc);
    else
      pimpl_->panel.MoveTo(rc);
  }
}

void UI::Update(const Context& ctx, const Status& status) {
  if (ctx_ == ctx && status_ == status)
    return;
  ctx_ = ctx;
  status_ = status;
  if (style_.candidate_abbreviate_length > 0) {
    for (auto& c : ctx_.cinfo.candies) {
      if (c.str.length() > (size_t)style_.candidate_abbreviate_length) {
        c.str =
            c.str.substr(0, (size_t)style_.candidate_abbreviate_length - 1) +
            L"..." + c.str.substr(c.str.length() - 1);
      }
    }
  }
  Refresh();
}
