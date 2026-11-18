// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_SURFACE_FACTORY_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_SURFACE_FACTORY_H_

#include <memory>
#include <vector>

#include "ui/ozone/platform/headless/headless_surface_factory.h"

namespace ui {

// Headless's surface factory (no real GPU), except that software frames go to
// the AXIOM window instead of nowhere.
class AxiomSurfaceFactory : public HeadlessSurfaceFactory {
 public:
  AxiomSurfaceFactory();
  AxiomSurfaceFactory(const AxiomSurfaceFactory&) = delete;
  AxiomSurfaceFactory& operator=(const AxiomSurfaceFactory&) = delete;
  ~AxiomSurfaceFactory() override;

  // SurfaceFactoryOzone:
  // AXIOM has no GL (Vertex ships no EGL, GLES or SwiftShader libraries): no
  // implementations and no GLOzone, so the GPU code never tries ANGLE, which
  // headless's factory would, and draws with software instead.
  std::vector<gl::GLImplementationParts> GetAllowedGLImplementations() override;
  GLOzone* GetGLOzone(const gl::GLImplementationParts& implementation) override;
  std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(
      gfx::AcceleratedWidget widget) override;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_SURFACE_FACTORY_H_
