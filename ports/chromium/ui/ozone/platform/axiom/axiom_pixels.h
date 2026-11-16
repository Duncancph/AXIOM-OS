// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "base/memory/raw_span.h"
#include "base/synchronization/lock.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"

class SkPixmap;

namespace ui {

// The AXIOM windows' mapped pixels, by widget. The window (UI thread) maps,
// remaps on resize and unmaps them, taking them out of here first; the canvas
// (the compositor's thread) copies finished frames into them under the same
// lock, so a frame never lands in unmapped pixels. (Single-process: one
// address space.)
class AxiomPixels {
 public:
  static AxiomPixels& Get();

  void Set(gfx::AcceleratedWidget widget, int fd, base::span<uint8_t> pixels,
           int width, int height, int stride);
  void Remove(gfx::AcceleratedWidget widget);

  // Copies `damage` of `frame` (clipped to both sizes) into the window and
  // shows it. False if the widget has no window (yet).
  bool Present(gfx::AcceleratedWidget widget, const SkPixmap& frame,
               const gfx::Rect& damage);

 private:
  struct Entry {
    gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
    int fd = -1;
    base::raw_span<uint8_t> pixels;
    int width = 0, height = 0, stride = 0;
  };
  static constexpr int kMaxWindows = 16;

  base::Lock lock_;
  Entry entries_[kMaxWindows] GUARDED_BY(lock_);
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_
