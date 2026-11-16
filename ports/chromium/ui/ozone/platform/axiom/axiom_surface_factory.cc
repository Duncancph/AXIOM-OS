// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_surface_factory.h"

#include "base/files/file_path.h"
#include "skia/ext/legacy_display_globals.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/ozone/platform/axiom/axiom_pixels.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

// Draws into its own surface; PresentCanvas copies the damage to the window.
// (Its own surface, not the window's pixels: the window can resize them while
// a frame is being drawn.)
class AxiomCanvas : public SurfaceOzoneCanvas {
 public:
  explicit AxiomCanvas(gfx::AcceleratedWidget widget) : widget_(widget) {}
  ~AxiomCanvas() override = default;

  // SurfaceOzoneCanvas:
  void ResizeCanvas(const gfx::Size& viewport_size, float scale) override {
    SkSurfaceProps props = skia::LegacyDisplayGlobals::GetSkSurfaceProps();
    surface_ = SkSurfaces::Raster(
        SkImageInfo::MakeN32Premul(viewport_size.width(),
                                   viewport_size.height()),
        &props);
  }
  SkCanvas* GetCanvas() override { return surface_->getCanvas(); }
  void PresentCanvas(const gfx::Rect& damage) override {
    SkPixmap frame;
    if (surface_ && surface_->peekPixels(&frame)) {
      AxiomPixels::Get().Present(widget_, frame, damage);
    }
  }
  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return nullptr;
  }

 private:
  const gfx::AcceleratedWidget widget_;
  sk_sp<SkSurface> surface_;
};

}  // namespace

AxiomSurfaceFactory::AxiomSurfaceFactory()
    : HeadlessSurfaceFactory(base::FilePath()) {}

AxiomSurfaceFactory::~AxiomSurfaceFactory() = default;

std::unique_ptr<SurfaceOzoneCanvas> AxiomSurfaceFactory::CreateCanvasForWidget(
    gfx::AcceleratedWidget widget) {
  return std::make_unique<AxiomCanvas>(widget);
}

}  // namespace ui
