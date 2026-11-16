// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_window.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/ozone/platform/axiom/axiom_pixels.h"
#include "ui/ozone/platform/axiom/axiom_window_abi.h"
#include "ui/platform_window/platform_window_delegate.h"

namespace ui {

namespace {

constexpr char kDevice[] = "/dev/axiom/window";

int ModifierFlags(int key) {
  int flags = 0;
  if (key & AXWIN_MOD_SHIFT) flags |= EF_SHIFT_DOWN;
  if (key & AXWIN_MOD_CTRL) flags |= EF_CONTROL_DOWN;
  if (key & AXWIN_MOD_ALT) flags |= EF_ALT_DOWN;
  if (key & AXWIN_MOD_SUPER) flags |= EF_COMMAND_DOWN;
  return flags;
}

// A printable character's US-layout key, and whether Shift makes it.
KeyboardCode CharacterKeyCode(char16_t c, bool* shifted) {
  *shifted = false;
  if (c >= 'a' && c <= 'z') return static_cast<KeyboardCode>(VKEY_A + (c - 'a'));
  if (c >= 'A' && c <= 'Z') {
    *shifted = true;
    return static_cast<KeyboardCode>(VKEY_A + (c - 'A'));
  }
  if (c >= '0' && c <= '9') return static_cast<KeyboardCode>(VKEY_0 + (c - '0'));
  static const struct { char ch; KeyboardCode code; bool shift; } kPunct[] = {
      {' ', VKEY_SPACE, false},      {'-', VKEY_OEM_MINUS, false},
      {'_', VKEY_OEM_MINUS, true},   {'=', VKEY_OEM_PLUS, false},
      {'+', VKEY_OEM_PLUS, true},    {'[', VKEY_OEM_4, false},
      {'{', VKEY_OEM_4, true},       {']', VKEY_OEM_6, false},
      {'}', VKEY_OEM_6, true},       {'\\', VKEY_OEM_5, false},
      {'|', VKEY_OEM_5, true},       {';', VKEY_OEM_1, false},
      {':', VKEY_OEM_1, true},       {'\'', VKEY_OEM_7, false},
      {'"', VKEY_OEM_7, true},       {',', VKEY_OEM_COMMA, false},
      {'<', VKEY_OEM_COMMA, true},   {'.', VKEY_OEM_PERIOD, false},
      {'>', VKEY_OEM_PERIOD, true},  {'/', VKEY_OEM_2, false},
      {'?', VKEY_OEM_2, true},       {'`', VKEY_OEM_3, false},
      {'~', VKEY_OEM_3, true},       {'!', VKEY_1, true},
      {'@', VKEY_2, true},           {'#', VKEY_3, true},
      {'$', VKEY_4, true},           {'%', VKEY_5, true},
      {'^', VKEY_6, true},           {'&', VKEY_7, true},
      {'*', VKEY_8, true},           {'(', VKEY_9, true},
      {')', VKEY_0, true},
  };
  for (const auto& p : kPunct) {
    if (p.ch == c) {
      *shifted = p.shift;
      return p.code;
    }
  }
  return VKEY_UNKNOWN;
}

}  // namespace

AxiomWindow::AxiomWindow(PlatformWindowDelegate* delegate,
                         HeadlessWindowManager* manager,
                         const gfx::Rect& bounds)
    : HeadlessWindow(delegate, manager, bounds) {}

AxiomWindow::~AxiomWindow() {
  watcher_.StopWatchingFileDescriptor();
  AxiomPixels::Get().Remove(widget());
  UnmapPixels();
  if (fd_ >= 0) {
    close(fd_);
  }
}

// static
gfx::Size AxiomWindow::ScreenSize() {
  int fd = open(kDevice, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return gfx::Size();
  }
  axwin_info info = {};
  int r = ioctl(fd, AXWIN_INFO, &info);
  close(fd);
  return r == 0 ? gfx::Size(info.screen_w, info.screen_h) : gfx::Size();
}

bool AxiomWindow::Open() {
  fd_ = open(kDevice, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd_ < 0) {
    PLOG(ERROR) << "AXIOM window: can't open " << kDevice;
    return false;
  }
  axwin_info screen = {};
  ioctl(fd_, AXWIN_INFO, &screen);
  // AXIOM centres new windows and doesn't let programs place them; keep the
  // size, within the screen (less the top bar, title bar and dock).
  gfx::Rect bounds = GetBoundsInPixels();
  axwin_create create = {};
  create.w = std::clamp(bounds.width(), 100, std::max(100, screen.screen_w - 20));
  create.h = std::clamp(bounds.height(), 60, std::max(60, screen.screen_h - 140));
  create.flags = AXWIN_RESIZABLE;
  base::strlcpy(base::span(create.title), title_);
  if (ioctl(fd_, AXWIN_CREATE, &create) < 0) {
    PLOG(ERROR) << "AXIOM window: can't create " << create.w << "x" << create.h;
    close(fd_);
    fd_ = -1;
    return false;
  }
  if (!MapPixels()) {
    return false;
  }
  base::CurrentUIThread::Get()->WatchFileDescriptor(
      fd_, true, base::MessagePumpForUI::WATCH_READ, &watcher_, this);
  return true;
}

bool AxiomWindow::MapPixels() {
  axwin_info info = {};
  if (ioctl(fd_, AXWIN_INFO, &info) < 0) {
    return false;
  }
  const size_t size = static_cast<size_t>(info.stride) * info.h;
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    PLOG(ERROR) << "AXIOM window: can't map its pixels";
    return false;
  }
  // SAFETY: mmap mapped `size` bytes at p.
  pixels_ = UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(p), size));
  AxiomPixels::Get().Set(widget(), fd_, pixels_, info.w, info.h, info.stride);
  // The window's size is AXIOM's to decide: tell Chromium what it is.
  gfx::Rect bounds = GetBoundsInPixels();
  if (bounds.width() != info.w || bounds.height() != info.h) {
    UpdateBounds(gfx::Rect(bounds.origin(), gfx::Size(info.w, info.h)));
  }
  return true;
}

void AxiomWindow::UnmapPixels() {
  if (!pixels_.empty()) {
    munmap(pixels_.data(), pixels_.size());
    pixels_ = {};
  }
}

void AxiomWindow::Show(bool inactive) {
  if (fd_ < 0) {
    Open();
  }
  HeadlessWindow::Show(inactive);
}

void AxiomWindow::Close() {
  watcher_.StopWatchingFileDescriptor();
  AxiomPixels::Get().Remove(widget());
  UnmapPixels();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  HeadlessWindow::Close();
}

bool AxiomWindow::ShouldUseNativeFrame() const {
  return true;
}

void AxiomWindow::SetTitle(const std::u16string& title) {
  title_ = base::UTF16ToUTF8(title);
  if (fd_ >= 0) {
    std::array<char, 64> t = {};
    base::strlcpy(base::span(t), title_);
    ioctl(fd_, AXWIN_TITLE, t.data());
  }
}

void AxiomWindow::OnFileCanReadWithoutBlocking(int fd) {
  std::array<axwin_event, 32> events;
  for (;;) {
    ssize_t n = HANDLE_EINTR(read(fd_, events.data(), sizeof(events)));
    if (n <= 0) {
      return;
    }
    for (const axwin_event& e : base::span(events).first(
             static_cast<size_t>(n) / sizeof(axwin_event))) {
      HandleEvent(e);
      if (fd_ < 0) {
        return;  // (closed by the event)
      }
    }
  }
}

void AxiomWindow::HandleEvent(const axwin_event& e) {
  base::TimeTicks now = base::TimeTicks::Now();
  gfx::PointF at(e.x, e.y);
  switch (e.type) {
    case AXWIN_EV_MOUSE_DOWN:
    case AXWIN_EV_MOUSE_UP: {
      int button = e.button == 2 ? EF_RIGHT_MOUSE_BUTTON : EF_LEFT_MOUSE_BUTTON;
      bool down = e.type == AXWIN_EV_MOUSE_DOWN;
      if (down) {
        buttons_ |= button;
      }
      MouseEvent event(down ? EventType::kMousePressed : EventType::kMouseReleased,
                       at, at, now, buttons_ | button, button);
      if (!down) {
        buttons_ &= ~button;
      }
      delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_MOUSE_MOVE: {
      MouseEvent event(buttons_ ? EventType::kMouseDragged : EventType::kMouseMoved,
                       at, at, now, buttons_, 0);
      delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_MOUSE_LEAVE: {
      MouseEvent event(EventType::kMouseExited, at, at, now, buttons_, 0);
      delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_WHEEL: {
      // AXIOM: steps, positive = down. Chromium: 120 per step, positive = up.
      MouseWheelEvent event(gfx::Vector2d(0, -e.key * MouseWheelEvent::kWheelDelta),
                            at, at, now, buttons_, 0);
      delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_KEY:
      DispatchKey(e.key);
      break;
    case AXWIN_EV_RESIZE:
      // Out of the registry first (frames are skipped until it's back), then
      // munmap (AXIOM refuses to resize while mapped), resize, map again.
      AxiomPixels::Get().Remove(widget());
      UnmapPixels();
      if (ioctl(fd_, AXWIN_RESIZE) < 0) {
        PLOG(ERROR) << "AXIOM window: resize";
      }
      MapPixels();
      break;
    case AXWIN_EV_CLOSE:
      delegate()->OnCloseRequest();
      break;
    case AXWIN_EV_FOCUS:
      delegate()->OnActivationChanged(true);
      break;
    case AXWIN_EV_BLUR:
      delegate()->OnActivationChanged(false);
      break;
  }
}

void AxiomWindow::DispatchKey(int key) {
  int code = key & 0xFFF;
  int flags = ModifierFlags(key);
  KeyboardCode key_code = VKEY_UNKNOWN;
  DomKey dom_key = DomKey::NONE;
  if (code == '\n' || code == '\r') {
    key_code = VKEY_RETURN;
    dom_key = DomKey::ENTER;
  } else if (code == '\b') {
    key_code = VKEY_BACK;
    dom_key = DomKey::BACKSPACE;
  } else if (code == '\t') {
    key_code = VKEY_TAB;
    dom_key = DomKey::TAB;
  } else if (code == 27) {
    key_code = VKEY_ESCAPE;
    dom_key = DomKey::ESCAPE;
  } else if (code >= 1 && code <= 26) {  // Ctrl+A..Z
    key_code = static_cast<KeyboardCode>(VKEY_A + code - 1);
    flags |= EF_CONTROL_DOWN;
    dom_key = DomKey::FromCharacter('a' + code - 1);
  } else if (code >= 32 && code < 0x100) {
    // Windows-1252; the Latin-1 part is Unicode's too.
    char16_t ch = static_cast<char16_t>(code);
    bool shifted;
    key_code = CharacterKeyCode(ch, &shifted);
    if (shifted) {
      flags |= EF_SHIFT_DOWN;
    }
    dom_key = DomKey::FromCharacter(ch);
  } else {
    static const struct { int axiom; KeyboardCode code; DomKey key; } kSpecial[] = {
        {AXWIN_KEY_UP, VKEY_UP, DomKey::ARROW_UP},
        {AXWIN_KEY_DOWN, VKEY_DOWN, DomKey::ARROW_DOWN},
        {AXWIN_KEY_LEFT, VKEY_LEFT, DomKey::ARROW_LEFT},
        {AXWIN_KEY_RIGHT, VKEY_RIGHT, DomKey::ARROW_RIGHT},
        {AXWIN_KEY_HOME, VKEY_HOME, DomKey::HOME},
        {AXWIN_KEY_END, VKEY_END, DomKey::END},
        {AXWIN_KEY_DEL, VKEY_DELETE, DomKey::DEL},
        {AXWIN_KEY_PGUP, VKEY_PRIOR, DomKey::PAGE_UP},
        {AXWIN_KEY_PGDN, VKEY_NEXT, DomKey::PAGE_DOWN},
        {AXWIN_KEY_INS, VKEY_INSERT, DomKey::INSERT},
        {AXWIN_KEY_PRTSC, VKEY_SNAPSHOT, DomKey::PRINT_SCREEN},
        {AXWIN_KEY_F1, VKEY_F1, DomKey::F1},   {AXWIN_KEY_F2, VKEY_F2, DomKey::F2},
        {AXWIN_KEY_F3, VKEY_F3, DomKey::F3},   {AXWIN_KEY_F4, VKEY_F4, DomKey::F4},
        {AXWIN_KEY_F5, VKEY_F5, DomKey::F5},   {AXWIN_KEY_F6, VKEY_F6, DomKey::F6},
        {AXWIN_KEY_F7, VKEY_F7, DomKey::F7},   {AXWIN_KEY_F8, VKEY_F8, DomKey::F8},
        {AXWIN_KEY_F9, VKEY_F9, DomKey::F9},   {AXWIN_KEY_F10, VKEY_F10, DomKey::F10},
        {AXWIN_KEY_F11, VKEY_F11, DomKey::F11}, {AXWIN_KEY_F12, VKEY_F12, DomKey::F12},
        {AXWIN_KEY_SUPER, VKEY_LWIN, DomKey::META},
    };
    for (const auto& s : kSpecial) {
      if (s.axiom == code) {
        key_code = s.code;
        dom_key = s.key;
      }
    }
  }
  if (key_code == VKEY_UNKNOWN && dom_key == DomKey::NONE) {
    return;
  }
  // AXIOM reports presses only: release right away.
  DomCode dom_code = UsLayoutKeyboardCodeToDomCode(key_code);
  base::TimeTicks now = base::TimeTicks::Now();
  KeyEvent press(EventType::kKeyPressed, key_code, dom_code, flags, dom_key, now);
  delegate()->DispatchEvent(&press);
  KeyEvent release(EventType::kKeyReleased, key_code, dom_code, flags, dom_key,
                   now);
  delegate()->DispatchEvent(&release);
}

}  // namespace ui
