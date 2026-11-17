// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_WINDOW_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_WINDOW_H_

#include <stdint.h>

#include <array>
#include <string>

#include "base/memory/raw_span.h"
#include "base/message_loop/message_pump_for_ui.h"
#include "ui/ozone/platform/headless/headless_window.h"
#include "ui/platform_window/platform_window_init_properties.h"

struct axwin_event;

namespace ui {

// An AXIOM window (/dev/axiom/window). Headless's window keeps the bounds and
// state; this adds the real window: it appears on Show(), its pixels are
// mapped for AxiomPixels, and its events (read on the UI thread) become
// Chromium events.
//
// Menus, dropdowns, the omnibox's suggestions, tooltips and bubbles are AXIOM
// popups: no frame, placed in screen coordinates, never focused. AXIOM has no
// pointer grabs, so capture is kept here, across this program's windows: while
// one has it, the others' mouse events go to it (in its coordinates), and when
// the program loses the focus (a click elsewhere) capture is lost, which closes
// menus as a grab would.
class AxiomWindow : public HeadlessWindow,
                    public base::MessagePumpForUI::FdWatcher {
 public:
  AxiomWindow(PlatformWindowDelegate* delegate,
              HeadlessWindowManager* manager,
              const PlatformWindowInitProperties& properties);
  AxiomWindow(const AxiomWindow&) = delete;
  AxiomWindow& operator=(const AxiomWindow&) = delete;
  ~AxiomWindow() override;

  // The AXIOM screen's size (0x0 if AXIOM windows aren't there).
  static gfx::Size ScreenSize();

  // PlatformWindow:
  void Show(bool inactive) override;
  void Hide() override;
  void Close() override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  void SetTitle(const std::u16string& title) override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool HasCapture() const override;
  void Maximize() override {}  // AXIOM's title bar does it (the size is AXIOM's)
  void Minimize() override;
  void Activate() override {}
  void SetCursor(scoped_refptr<PlatformCursor> cursor) override;
  // AXIOM draws the title bar and its buttons: views shouldn't draw its own.
  void SetUseNativeFrame(bool use_native_frame) override {}
  bool ShouldUseNativeFrame() const override;

  // base::MessagePumpForUI::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override {}

 private:
  bool Open();
  void CloseWindow();
  bool MapPixels();
  void UnmapPixels();
  void HandleEvent(const axwin_event& e);
  void DispatchMouse(const axwin_event& e);
  void DispatchKey(int key, int usage);
  void LoseCapture();

  const bool popup_;
  int fd_ = -1;
  bool shown_ = false;
  int cursor_shape_ = 0;
  base::raw_span<uint8_t> pixels_;  // mmap'd from the window
  int buttons_ = 0;  // EF_*_MOUSE_BUTTON held
  std::string title_ = "Vertex";
  base::MessagePumpForUI::FdWatchController watcher_{FROM_HERE};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_WINDOW_H_
