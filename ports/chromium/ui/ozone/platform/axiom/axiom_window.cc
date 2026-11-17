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
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/ozone/common/bitmap_cursor.h"
#include "ui/ozone/platform/axiom/axiom_pixels.h"
#include "ui/ozone/platform/axiom/axiom_window_abi.h"
#include "ui/platform_window/platform_window_delegate.h"

namespace ui {

namespace {

constexpr char kDevice[] = "/dev/axiom/window";

// The window holding capture (see the header), if any.
AxiomWindow* g_capture = nullptr;

int ModifierFlags(int key) {
  int flags = 0;
  if (key & AXWIN_MOD_SHIFT) flags |= EF_SHIFT_DOWN;
  if (key & AXWIN_MOD_CTRL) flags |= EF_CONTROL_DOWN;
  if (key & AXWIN_MOD_ALT) flags |= EF_ALT_DOWN;
  if (key & AXWIN_MOD_SUPER) flags |= EF_COMMAND_DOWN;
  return flags;
}

// AXIOM's characters are Windows-1252: Latin-1, except 0x80..0x9F.
char16_t Cp1252ToUnicode(int c) {
  static constexpr char16_t k80[32] = {
      0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
  if (c >= 0x80 && c < 0xA0) {
    return UNSAFE_BUFFERS(k80[c - 0x80]);
  }
  return static_cast<char16_t>(c);
}

// A character's key on a US layout, and whether Shift makes it there (what
// web pages expect in keyCode for the character typed, whatever the layout).
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

int CursorShape(mojom::CursorType type) {
  using T = mojom::CursorType;
  switch (type) {
    case T::kIBeam:
    case T::kVerticalText:
      return AXWIN_CURSOR_TEXT;
    case T::kHand:
      return AXWIN_CURSOR_HAND;
    case T::kWait:
    case T::kProgress:
      return AXWIN_CURSOR_WAIT;
    case T::kCross:
    case T::kCell:
      return AXWIN_CURSOR_CROSSHAIR;
    case T::kMove:
    case T::kGrab:
    case T::kGrabbing:
    case T::kMiddlePanning:
    case T::kMiddlePanningVertical:
    case T::kMiddlePanningHorizontal:
    case T::kDndMove:
      return AXWIN_CURSOR_MOVE;
    case T::kEastResize:
    case T::kWestResize:
    case T::kEastWestResize:
    case T::kColumnResize:
    case T::kEastWestNoResize:
    case T::kEastPanning:
    case T::kWestPanning:
      return AXWIN_CURSOR_RESIZE_EW;
    case T::kNorthResize:
    case T::kSouthResize:
    case T::kNorthSouthResize:
    case T::kRowResize:
    case T::kNorthSouthNoResize:
    case T::kNorthPanning:
    case T::kSouthPanning:
      return AXWIN_CURSOR_RESIZE_NS;
    case T::kNorthWestResize:
    case T::kSouthEastResize:
    case T::kNorthWestSouthEastResize:
    case T::kNorthWestSouthEastNoResize:
    case T::kNorthWestPanning:
    case T::kSouthEastPanning:
      return AXWIN_CURSOR_RESIZE_NWSE;
    case T::kNorthEastResize:
    case T::kSouthWestResize:
    case T::kNorthEastSouthWestResize:
    case T::kNorthEastSouthWestNoResize:
    case T::kNorthEastPanning:
    case T::kSouthWestPanning:
      return AXWIN_CURSOR_RESIZE_NESW;
    case T::kNotAllowed:
    case T::kNoDrop:
    case T::kDndNone:
      return AXWIN_CURSOR_NOT_ALLOWED;
    case T::kNone:
      return AXWIN_CURSOR_NONE;
    default:
      return AXWIN_CURSOR_ARROW;
  }
}

bool IsPopup(PlatformWindowType type) {
  return type != PlatformWindowType::kWindow;
}

}  // namespace

AxiomWindow::AxiomWindow(PlatformWindowDelegate* delegate,
                         HeadlessWindowManager* manager,
                         const PlatformWindowInitProperties& properties)
    : HeadlessWindow(delegate, manager, properties.bounds),
      popup_(IsPopup(properties.type)) {}

AxiomWindow::~AxiomWindow() {
  if (g_capture == this) {
    g_capture = nullptr;
  }
  CloseWindow();
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
  gfx::Rect bounds = GetBoundsInPixels();
  axwin_create create = {};
  if (popup_) {
    // Exactly where views put it, in screen coordinates.
    axwin_rect at = {bounds.x(), bounds.y(), 0, 0};
    ioctl(fd_, AXWIN_MOVE, &at);
    create.w = std::clamp(bounds.width(), 1, std::max(1, screen.screen_w));
    create.h = std::clamp(bounds.height(), 1, std::max(1, screen.screen_h));
    create.flags = AXWIN_POPUP;
  } else {
    // AXIOM places new windows; keep the size, within the screen (less the
    // top bar, title bar and dock).
    create.w = std::clamp(bounds.width(), 100, std::max(100, screen.screen_w - 20));
    create.h = std::clamp(bounds.height(), 60, std::max(60, screen.screen_h - 140));
    create.flags = AXWIN_RESIZABLE | AXWIN_MOVE_EVENTS;
  }
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
  if (!popup_) {
    axwin_rect pos = {};
    if (ioctl(fd_, AXWIN_POS, &pos) == 0) {
      UpdateBounds(gfx::Rect(pos.x, pos.y, pos.w, pos.h));
    }
  }
  if (cursor_shape_) {
    ioctl(fd_, AXWIN_CURSOR, &cursor_shape_);
  }
  shown_ = true;
  base::CurrentUIThread::Get()->WatchFileDescriptor(
      fd_, true, base::MessagePumpForUI::WATCH_READ, &watcher_, this);
  return true;
}

void AxiomWindow::CloseWindow() {
  watcher_.StopWatchingFileDescriptor();
  AxiomPixels::Get().Remove(widget());
  UnmapPixels();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  shown_ = false;
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
  } else if (!shown_) {
    int show = 1;
    ioctl(fd_, AXWIN_SHOW, &show);
    shown_ = true;
  }
  HeadlessWindow::Show(inactive);
}

void AxiomWindow::Hide() {
  if (fd_ >= 0 && shown_) {
    int show = 0;
    ioctl(fd_, AXWIN_SHOW, &show);
    shown_ = false;
  }
  if (g_capture == this) {
    LoseCapture();
  }
  HeadlessWindow::Hide();
}

void AxiomWindow::Close() {
  if (g_capture == this) {
    g_capture = nullptr;
  }
  CloseWindow();
  HeadlessWindow::Close();
}

void AxiomWindow::Minimize() {
  if (fd_ >= 0) {
    int show = 0;
    ioctl(fd_, AXWIN_SHOW, &show);  // AXIOM minimises it; the dock brings it back
  }
}

void AxiomWindow::SetBoundsInPixels(const gfx::Rect& bounds) {
  const gfx::Rect old = GetBoundsInPixels();
  if (popup_) {
    HeadlessWindow::SetBoundsInPixels(bounds);
    if (fd_ < 0) {
      return;
    }
    if (bounds.size() != old.size()) {
      // A popup's size is its own; AXIOM resizes only on request, so make it
      // again at the new size (menus and suggestion lists change size rarely).
      bool was_shown = shown_;
      CloseWindow();
      if (was_shown) {
        Open();
      }
    } else if (bounds.origin() != old.origin()) {
      axwin_rect at = {bounds.x(), bounds.y(), 0, 0};
      ioctl(fd_, AXWIN_MOVE, &at);
    }
    return;
  }
  if (fd_ < 0) {
    HeadlessWindow::SetBoundsInPixels(bounds);  // (Open() uses the size)
    return;
  }
  // A window's size is AXIOM's (its frame resizes it); it can be moved.
  if (bounds.origin() != old.origin()) {
    axwin_rect at = {bounds.x(), bounds.y(), 0, 0};
    ioctl(fd_, AXWIN_MOVE, &at);
  }
  axwin_rect pos = {};
  if (ioctl(fd_, AXWIN_POS, &pos) == 0) {
    UpdateBounds(gfx::Rect(pos.x, pos.y, pos.w, pos.h));
  }
}

bool AxiomWindow::ShouldUseNativeFrame() const {
  return true;
}

void AxiomWindow::SetTitle(const std::u16string& title) {
  title_ = base::UTF16ToUTF8(title);
  if (fd_ >= 0 && !popup_) {
    std::array<char, 64> t = {};
    base::strlcpy(base::span(t), title_);
    ioctl(fd_, AXWIN_TITLE, t.data());
  }
}

void AxiomWindow::SetCapture() {
  if (g_capture == this) {
    return;
  }
  if (g_capture) {
    g_capture->LoseCapture();
  }
  g_capture = this;
}

void AxiomWindow::ReleaseCapture() {
  if (g_capture == this) {
    g_capture = nullptr;
  }
}

bool AxiomWindow::HasCapture() const {
  return g_capture == this;
}

void AxiomWindow::LoseCapture() {
  if (g_capture != this) {
    return;
  }
  g_capture = nullptr;
  delegate()->OnLostCapture();
}

void AxiomWindow::SetCursor(scoped_refptr<PlatformCursor> cursor) {
  scoped_refptr<BitmapCursor> bitmap = BitmapCursor::FromPlatformCursor(cursor);
  int shape = bitmap ? CursorShape(bitmap->type()) : AXWIN_CURSOR_ARROW;
  if (shape == cursor_shape_) {
    return;
  }
  cursor_shape_ = shape;
  if (fd_ >= 0) {
    ioctl(fd_, AXWIN_CURSOR, &shape);
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
  switch (e.type) {
    case AXWIN_EV_MOUSE_DOWN:
    case AXWIN_EV_MOUSE_UP:
    case AXWIN_EV_MOUSE_MOVE:
    case AXWIN_EV_MOUSE_LEAVE:
    case AXWIN_EV_WHEEL:
      DispatchMouse(e);
      break;
    case AXWIN_EV_KEY:
      DispatchKey(e.key, e.button);
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
    case AXWIN_EV_MOVE:
      UpdateBounds(gfx::Rect(gfx::Point(e.x, e.y), GetBoundsInPixels().size()));
      break;
    case AXWIN_EV_CLOSE:
      delegate()->OnCloseRequest();
      break;
    case AXWIN_EV_FOCUS:
      delegate()->OnActivationChanged(true);
      break;
    case AXWIN_EV_BLUR:
      // Something else was clicked: as a pointer grab would, end capture (a
      // menu that's open closes).
      if (g_capture) {
        g_capture->LoseCapture();
      }
      delegate()->OnActivationChanged(false);
      break;
  }
}

void AxiomWindow::DispatchMouse(const axwin_event& e) {
  // While a window of ours has capture, the mouse is its, wherever it is.
  AxiomWindow* target = this;
  gfx::PointF at(e.x, e.y);
  if (g_capture && g_capture != this) {
    if (e.type == AXWIN_EV_MOUSE_LEAVE) {
      return;
    }
    target = g_capture;
    at += gfx::Vector2dF(GetBoundsInPixels().origin() -
                         target->GetBoundsInPixels().origin());
  }
  base::TimeTicks now = base::TimeTicks::Now();
  switch (e.type) {
    case AXWIN_EV_MOUSE_DOWN:
    case AXWIN_EV_MOUSE_UP: {
      int button = e.button == 2 ? EF_RIGHT_MOUSE_BUTTON : EF_LEFT_MOUSE_BUTTON;
      bool down = e.type == AXWIN_EV_MOUSE_DOWN;
      if (down) {
        target->buttons_ |= button;
      }
      MouseEvent event(down ? EventType::kMousePressed : EventType::kMouseReleased,
                       at, at, now, target->buttons_ | button, button);
      if (!down) {
        target->buttons_ &= ~button;
      }
      target->delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_MOUSE_MOVE: {
      MouseEvent event(
          target->buttons_ ? EventType::kMouseDragged : EventType::kMouseMoved,
          at, at, now, target->buttons_, 0);
      target->delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_MOUSE_LEAVE: {
      MouseEvent event(EventType::kMouseExited, at, at, now, buttons_, 0);
      delegate()->DispatchEvent(&event);
      break;
    }
    case AXWIN_EV_WHEEL: {
      // AXIOM: steps, positive = down. Chromium: 120 per step, positive = up.
      MouseWheelEvent event(
          gfx::Vector2d(0, -e.key * MouseWheelEvent::kWheelDelta), at, at, now,
          target->buttons_, 0);
      target->delegate()->DispatchEvent(&event);
      break;
    }
  }
}

void AxiomWindow::DispatchKey(int key, int usage) {
  int code = key & 0xFFF;
  int flags = ModifierFlags(key);
  KeyboardCode key_code = VKEY_UNKNOWN;
  DomKey dom_key = DomKey::NONE;
  // The physical key, from its USB usage code (AXIOM applies the layout to
  // the character; the key's place is the same on every layout).
  DomCode dom_code = usage ? KeycodeConverter::UsbKeycodeToDomCode(0x70000 | usage)
                           : DomCode::NONE;
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
    char16_t ch = Cp1252ToUnicode(code);
    if (!ch) {
      return;
    }
    bool shifted;
    key_code = CharacterKeyCode(ch, &shifted);
    if (shifted) {
      flags |= EF_SHIFT_DOWN;
    }
    if (key_code == VKEY_UNKNOWN && dom_code != DomCode::NONE) {
      // A letter a US keyboard doesn't have (é, ü, ß...): the key it's on.
      key_code = DomCodeToUsLayoutKeyboardCode(dom_code);
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
  if (dom_code == DomCode::NONE) {
    dom_code = UsLayoutKeyboardCodeToDomCode(key_code);
  }
  // AXIOM reports presses only: release right away.
  base::TimeTicks now = base::TimeTicks::Now();
  KeyEvent press(EventType::kKeyPressed, key_code, dom_code, flags, dom_key, now);
  delegate()->DispatchEvent(&press);
  KeyEvent release(EventType::kKeyReleased, key_code, dom_code, flags, dom_key,
                   now);
  delegate()->DispatchEvent(&release);
}

}  // namespace ui
