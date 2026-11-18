// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_gpu_host.h"

#include <fcntl.h>

#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "mojo/public/cpp/platform/platform_handle.h"

namespace ui {

namespace {

base::ScopedFD DupFd(int fd) {
  return base::ScopedFD(HANDLE_EINTR(fcntl(fd, F_DUPFD_CLOEXEC, 0)));
}

}  // namespace

AxiomGpuHost::AxiomGpuHost() = default;
AxiomGpuHost::~AxiomGpuHost() = default;

void AxiomGpuHost::AddWindow(gfx::AcceleratedWidget widget, int fd) {
  base::ScopedFD own = DupFd(fd);
  if (!own.is_valid()) {
    PLOG(ERROR) << "AXIOM window: can't duplicate its file";
    return;
  }
  windows_[widget] = std::move(own);
  Send(widget, fd);
}

void AxiomGpuHost::RemoveWindow(gfx::AcceleratedWidget widget) {
  if (windows_.erase(widget) && gpu_) {
    gpu_->RemoveWindow(widget);
  }
}

void AxiomGpuHost::OnChannelDestroyed(int host_id) {
  if (host_id == host_id_) {
    gpu_.reset();
  }
}

void AxiomGpuHost::OnGpuServiceLaunched(
    int host_id,
    GpuHostBindInterfaceCallback binder,
    GpuHostTerminateCallback terminate_callback) {
  host_id_ = host_id;
  gpu_.reset();
  binder.Run(ozone::mojom::AxiomWindowPixels::Name_,
             gpu_.BindNewPipeAndPassReceiver().PassPipe());
  for (const auto& [widget, fd] : windows_) {
    Send(widget, fd.get());
  }
}

void AxiomGpuHost::Send(gfx::AcceleratedWidget widget, int fd) {
  if (!gpu_) {
    return;  // (sent when the GPU service is launched)
  }
  base::ScopedFD copy = DupFd(fd);
  if (copy.is_valid()) {
    gpu_->SetWindow(widget, mojo::PlatformHandle(std::move(copy)));
  }
}

}  // namespace ui
