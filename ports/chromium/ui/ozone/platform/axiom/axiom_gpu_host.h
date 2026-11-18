// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_GPU_HOST_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_GPU_HOST_H_

#include "base/containers/flat_map.h"
#include "base/files/scoped_file.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/axiom/mojom/axiom_window_pixels.mojom.h"
#include "ui/ozone/public/gpu_platform_support_host.h"

namespace ui {

// The browser's side (UI thread): sends each open AXIOM window's file to the
// GPU service, which draws it (AxiomPixels), again to a GPU process that was
// launched again.
class AxiomGpuHost : public GpuPlatformSupportHost {
 public:
  AxiomGpuHost();
  AxiomGpuHost(const AxiomGpuHost&) = delete;
  AxiomGpuHost& operator=(const AxiomGpuHost&) = delete;
  ~AxiomGpuHost() override;

  void AddWindow(gfx::AcceleratedWidget widget, int fd);
  void RemoveWindow(gfx::AcceleratedWidget widget);

  // GpuPlatformSupportHost:
  void OnChannelDestroyed(int host_id) override;
  void OnGpuServiceLaunched(
      int host_id,
      GpuHostBindInterfaceCallback binder,
      GpuHostTerminateCallback terminate_callback) override;

 private:
  void Send(gfx::AcceleratedWidget widget, int fd);

  int host_id_ = -1;
  mojo::Remote<ozone::mojom::AxiomWindowPixels> gpu_;
  base::flat_map<gfx::AcceleratedWidget, base::ScopedFD> windows_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_GPU_HOST_H_
