// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_surface_factory.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <algorithm>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/memory/raw_span.h"
#include "skia/ext/legacy_display_globals.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/ozone/platform/axiom/axiom_pixels.h"
#include "ui/ozone/platform/axiom/axiom_window_abi.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

// Draws into its own surface; PresentCanvas copies the damage into the
// window's pixels, which it maps itself from the file the browser sent
// (AxiomPixels), in whichever process the compositor runs. It also resizes
// the window to the size it draws at: AXIOM won't resize pixels that are
// mapped, and only this side knows when they aren't in use.
class AxiomCanvas : public SurfaceOzoneCanvas {
 public:
  explicit AxiomCanvas(gfx::AcceleratedWidget widget) : widget_(widget) {}
  ~AxiomCanvas() override { Unmap(); }

  // SurfaceOzoneCanvas:
  void ResizeCanvas(const gfx::Size& viewport_size, float scale) override {
    SkSurfaceProps props = skia::LegacyDisplayGlobals::GetSkSurfaceProps();
    surface_ = SkSurfaces::Raster(
        SkImageInfo::MakeN32Premul(viewport_size.width(),
                                   viewport_size.height()),
        &props);
    size_ = viewport_size;
    if (fd_.is_valid() && (width_ != size_.width() || height_ != size_.height())) {
      Map(std::move(fd_));
    }
  }
  SkCanvas* GetCanvas() override { return surface_->getCanvas(); }
  void PresentCanvas(const gfx::Rect& damage) override {
    SkPixmap frame;
    if (!surface_ || !surface_->peekPixels(&frame)) {
      return;
    }
    if (AxiomPixels::Get().Generation(widget_) != generation_) {
      uint64_t generation;
      base::ScopedFD fd = AxiomPixels::Get().Dup(widget_, &generation);
      generation_ = generation;
      Map(std::move(fd));
    } else if (fd_.is_valid() && !fixed_size_ && !size_.IsEmpty() &&
               (width_ != size_.width() || height_ != size_.height())) {
      Map(std::move(fd_));  // (the last resize didn't happen: try again)
    }
    if (pixels_.empty()) {
      return;  // (no window yet, or gone)
    }
    gfx::Rect r = damage;
    r.Intersect(gfx::Rect(0, 0, std::min(width_, frame.width()),
                          std::min(height_, frame.height())));
    if (r.IsEmpty()) {
      return;
    }
    // Skia's N32 on little-endian Linux is BGRA in memory: the same bytes as
    // AXIOM's XRGB8888 (the alpha byte is ignored there).
    const size_t row_bytes = static_cast<size_t>(r.width()) * 4;
    for (int y = r.y(); y < r.bottom(); y++) {
      // SAFETY: (r.x(), y) and r.width() pixels after it are inside `frame`
      // (r was clipped to its size above).
      auto src = UNSAFE_BUFFERS(base::span(
          static_cast<const uint8_t*>(frame.addr(r.x(), y)), row_bytes));
      pixels_
          .subspan(static_cast<size_t>(y) * stride_ +
                       static_cast<size_t>(r.x()) * 4,
                   row_bytes)
          .copy_from(src);
    }
    axwin_rect flush = {r.x(), r.y(), r.width(), r.height()};
    ioctl(fd_.get(), AXWIN_FLUSH, &flush);
  }
  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return nullptr;
  }

 private:
  // Maps `fd`'s window (none if it's invalid), first making it the size
  // drawn at (a popup's size is fixed: that fails, and it's drawn clipped).
  void Map(base::ScopedFD fd) {
    Unmap();
    fd_ = std::move(fd);
    if (!fd_.is_valid()) {
      return;
    }
    if (!size_.IsEmpty()) {
      axwin_rect want = {0, 0, size_.width(), size_.height()};
      fixed_size_ = ioctl(fd_.get(), AXWIN_RESIZE_TO, &want) < 0 &&
                    errno == EINVAL;
    }
    axwin_info info = {};
    if (ioctl(fd_.get(), AXWIN_INFO, &info) < 0 || info.w <= 0) {
      return;
    }
    const size_t size = static_cast<size_t>(info.stride) * info.h;
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd_.get(), 0);
    if (p == MAP_FAILED) {
      PLOG(ERROR) << "AXIOM window: can't map its pixels";
      return;
    }
    // SAFETY: mmap mapped `size` bytes at p.
    pixels_ = UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(p), size));
    width_ = info.w;
    height_ = info.h;
    stride_ = info.stride;
  }
  void Unmap() {
    if (!pixels_.empty()) {
      munmap(pixels_.data(), pixels_.size());
      pixels_ = {};
    }
    width_ = height_ = stride_ = 0;
  }

  const gfx::AcceleratedWidget widget_;
  sk_sp<SkSurface> surface_;
  gfx::Size size_;
  uint64_t generation_ = 0;
  bool fixed_size_ = false;  // a popup: AXIOM won't resize it
  base::ScopedFD fd_;
  base::raw_span<uint8_t> pixels_;  // mmap'd from the window
  int width_ = 0, height_ = 0, stride_ = 0;
};

}  // namespace

AxiomSurfaceFactory::AxiomSurfaceFactory()
    : HeadlessSurfaceFactory(base::FilePath()) {}

AxiomSurfaceFactory::~AxiomSurfaceFactory() = default;

std::vector<gl::GLImplementationParts>
AxiomSurfaceFactory::GetAllowedGLImplementations() {
  return {};
}

GLOzone* AxiomSurfaceFactory::GetGLOzone(
    const gl::GLImplementationParts& implementation) {
  return nullptr;
}

std::unique_ptr<SurfaceOzoneCanvas> AxiomSurfaceFactory::CreateCanvasForWidget(
    gfx::AcceleratedWidget widget) {
  return std::make_unique<AxiomCanvas>(widget);
}

}  // namespace ui
