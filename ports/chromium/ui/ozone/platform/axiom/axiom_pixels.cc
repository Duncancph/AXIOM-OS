// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_pixels.h"

#include <sys/ioctl.h>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/ozone/platform/axiom/axiom_window_abi.h"

namespace ui {

// static
AxiomPixels& AxiomPixels::Get() {
  static base::NoDestructor<AxiomPixels> pixels;
  return *pixels;
}

void AxiomPixels::Set(gfx::AcceleratedWidget widget, int fd,
                      base::span<uint8_t> pixels, int width, int height,
                      int stride) {
  base::AutoLock hold(lock_);
  Entry* free_entry = nullptr;
  for (Entry& e : entries_) {
    if (e.widget == widget) {
      free_entry = &e;
      break;
    }
    if (!free_entry && e.widget == gfx::kNullAcceleratedWidget) {
      free_entry = &e;
    }
  }
  CHECK(free_entry);
  *free_entry = {widget, fd, pixels, width, height, stride};
}

void AxiomPixels::Remove(gfx::AcceleratedWidget widget) {
  base::AutoLock hold(lock_);
  for (Entry& e : entries_) {
    if (e.widget == widget) {
      e = Entry();
    }
  }
}

bool AxiomPixels::Present(gfx::AcceleratedWidget widget, const SkPixmap& frame,
                          const gfx::Rect& damage) {
  base::AutoLock hold(lock_);
  for (Entry& e : entries_) {
    if (e.widget != widget || e.pixels.empty()) {
      continue;
    }
    gfx::Rect r = damage;
    r.Intersect(gfx::Rect(0, 0, std::min(e.width, frame.width()),
                          std::min(e.height, frame.height())));
    if (r.IsEmpty()) {
      return true;
    }
    // Skia's N32 on little-endian Linux is BGRA in memory: the same bytes as
    // AXIOM's XRGB8888 (the alpha byte is ignored there).
    const size_t row_bytes = static_cast<size_t>(r.width()) * 4;
    for (int y = r.y(); y < r.bottom(); y++) {
      // SAFETY: (r.x(), y) and r.width() pixels after it are inside `frame`
      // (r was clipped to its size above).
      auto src = UNSAFE_BUFFERS(base::span(
          static_cast<const uint8_t*>(frame.addr(r.x(), y)), row_bytes));
      e.pixels
          .subspan(static_cast<size_t>(y) * e.stride +
                       static_cast<size_t>(r.x()) * 4,
                   row_bytes)
          .copy_from(src);
    }
    axwin_rect flush = {r.x(), r.y(), r.width(), r.height()};
    ioctl(e.fd, AXWIN_FLUSH, &flush);
    return true;
  }
  return false;
}

}  // namespace ui
