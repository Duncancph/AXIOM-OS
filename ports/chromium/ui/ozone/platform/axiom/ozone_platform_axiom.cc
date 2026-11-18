// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/ozone_platform_axiom.h"

#include <memory>

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "ui/base/cursor/cursor_factory.h"
#include "ui/base/ime/input_method_minimal.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/gfx/switches.h"
#include "ui/ozone/common/bitmap_cursor_factory.h"
#include "ui/ozone/common/stub_overlay_manager.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "ui/ozone/platform/axiom/axiom_clipboard.h"
#include "ui/ozone/platform/axiom/axiom_gpu_host.h"
#include "ui/ozone/platform/axiom/axiom_pixels.h"
#include "ui/ozone/platform/axiom/axiom_surface_factory.h"
#include "ui/ozone/platform/axiom/axiom_window.h"
#include "ui/ozone/platform/headless/headless_screen.h"
#include "ui/ozone/platform/headless/headless_window_manager.h"
#include "ui/ozone/platform/headless/ozone_platform_headless.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/input_controller.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/stub_input_controller.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

namespace {

class AxiomPlatformEventSource : public PlatformEventSource {
 public:
  AxiomPlatformEventSource() = default;
  ~AxiomPlatformEventSource() override = default;
};

// Built like headless (whose screen and window manager it uses, through
// OzonePlatformHeadless), with AXIOM windows and AXIOM's screen size.
class OzonePlatformAxiom : public OzonePlatformHeadless {
 public:
  OzonePlatformAxiom() = default;
  OzonePlatformAxiom(const OzonePlatformAxiom&) = delete;
  OzonePlatformAxiom& operator=(const OzonePlatformAxiom&) = delete;
  ~OzonePlatformAxiom() override = default;

  // OzonePlatformHeadless:
  HeadlessWindowManager* GetHeadlessWindowManager() override {
    return window_manager_.get();
  }

  // OzonePlatform:
  SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surface_factory_.get();
  }
  OverlayManagerOzone* GetOverlayManager() override {
    return overlay_manager_.get();
  }
  CursorFactory* GetCursorFactory() override { return cursor_factory_.get(); }
  InputController* GetInputController() override {
    return input_controller_.get();
  }
  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return gpu_host_.get();
  }
  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }
  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    return std::make_unique<AxiomWindow>(delegate, window_manager_.get(),
                                         gpu_host_.get(), properties);
  }
  // AXIOM shows windows' pixels as they are (no alpha): menus and bubbles are
  // drawn opaque and square, without shadows.
  bool IsWindowCompositingSupported() const override { return false; }
  PlatformClipboard* GetPlatformClipboard() override {
    if (!clipboard_) {
      clipboard_ = std::make_unique<AxiomClipboard>();
    }
    return clipboard_.get();
  }
  std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate()
      override {
    return nullptr;
  }
  std::unique_ptr<PlatformScreen> CreateScreen() override {
    // HeadlessScreen reads --screen-info; give it AXIOM's screen.
    base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
    gfx::Size size = AxiomWindow::ScreenSize();
    if (!cmd->HasSwitch(switches::kScreenInfo) && !size.IsEmpty()) {
      cmd->AppendSwitchASCII(
          switches::kScreenInfo,
          base::StringPrintf("{%dx%d}", size.width(), size.height()));
    }
    return std::make_unique<HeadlessScreen>();
  }
  void InitScreen(PlatformScreen* screen) override {}
  std::unique_ptr<InputMethod> CreateInputMethod(
      ImeKeyEventDispatcher* ime_key_event_dispatcher,
      gfx::AcceleratedWidget widget) override {
    return std::make_unique<InputMethodMinimal>(ime_key_event_dispatcher);
  }
  const PlatformProperties& GetPlatformProperties() override {
    static base::NoDestructor<OzonePlatform::PlatformProperties> properties;
    return *properties;
  }

  bool InitializeUI(const InitParams& params) override {
    window_manager_ = std::make_unique<HeadlessWindowManager>();
    surface_factory_ = std::make_unique<AxiomSurfaceFactory>();
    if (!PlatformEventSource::GetInstance()) {
      platform_event_source_ = std::make_unique<AxiomPlatformEventSource>();
    }
    keyboard_layout_engine_ = std::make_unique<StubKeyboardLayoutEngine>();
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(
        keyboard_layout_engine_.get());
    overlay_manager_ = std::make_unique<StubOverlayManager>();
    input_controller_ = std::make_unique<StubInputController>();
    cursor_factory_ = std::make_unique<BitmapCursorFactory>();
    gpu_host_ = std::make_unique<AxiomGpuHost>();
    return true;
  }

  void InitializeGPU(const InitParams& params) override {
    if (!surface_factory_) {
      surface_factory_ = std::make_unique<AxiomSurfaceFactory>();
    }
  }

  // The GPU service's end of AxiomGpuHost: the windows' files (on the GPU
  // service's main thread; the canvases read them under a lock).
  void AddInterfaces(mojo::BinderMap* binders) override {
    binders->Add<ozone::mojom::AxiomWindowPixels>(
        base::BindRepeating(
            [](mojo::PendingReceiver<ozone::mojom::AxiomWindowPixels> r) {
              AxiomPixels::Get().Bind(std::move(r));
            }),
        base::SingleThreadTaskRunner::GetCurrentDefault());
  }

 private:
  std::unique_ptr<KeyboardLayoutEngine> keyboard_layout_engine_;
  std::unique_ptr<HeadlessWindowManager> window_manager_;
  std::unique_ptr<AxiomSurfaceFactory> surface_factory_;
  std::unique_ptr<PlatformEventSource> platform_event_source_;
  std::unique_ptr<CursorFactory> cursor_factory_;
  std::unique_ptr<InputController> input_controller_;
  std::unique_ptr<AxiomGpuHost> gpu_host_;
  std::unique_ptr<OverlayManagerOzone> overlay_manager_;
  std::unique_ptr<AxiomClipboard> clipboard_;
};

}  // namespace

OzonePlatform* CreateOzonePlatformAxiom() {
  return new OzonePlatformAxiom();
}

}  // namespace ui
