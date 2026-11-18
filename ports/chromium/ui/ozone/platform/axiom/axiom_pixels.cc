// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_pixels.h"

#include <fcntl.h>

#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "mojo/public/cpp/bindings/receiver_set.h"

namespace ui {

namespace {

mojo::ReceiverSet<ozone::mojom::AxiomWindowPixels>& Receivers() {
  static base::NoDestructor<mojo::ReceiverSet<ozone::mojom::AxiomWindowPixels>>
      receivers;
  return *receivers;
}

}  // namespace

// static
AxiomPixels& AxiomPixels::Get() {
  static base::NoDestructor<AxiomPixels> pixels;
  return *pixels;
}

AxiomPixels::AxiomPixels() = default;
AxiomPixels::~AxiomPixels() = default;

void AxiomPixels::Bind(
    mojo::PendingReceiver<ozone::mojom::AxiomWindowPixels> receiver) {
  Receivers().Add(this, std::move(receiver));
}

base::ScopedFD AxiomPixels::Dup(gfx::AcceleratedWidget widget,
                                uint64_t* generation) {
  base::AutoLock hold(lock_);
  auto it = windows_.find(widget);
  if (it == windows_.end()) {
    *generation = 0;
    return base::ScopedFD();
  }
  *generation = it->second.generation;
  return base::ScopedFD(
      HANDLE_EINTR(fcntl(it->second.fd.get(), F_DUPFD_CLOEXEC, 0)));
}

uint64_t AxiomPixels::Generation(gfx::AcceleratedWidget widget) {
  base::AutoLock hold(lock_);
  auto it = windows_.find(widget);
  return it == windows_.end() ? 0 : it->second.generation;
}

void AxiomPixels::SetWindow(uint64_t widget, mojo::PlatformHandle window) {
  base::AutoLock hold(lock_);
  Entry& e = windows_[static_cast<gfx::AcceleratedWidget>(widget)];
  e.fd = window.TakeFD();
  e.generation = next_generation_++;
}

void AxiomPixels::RemoveWindow(uint64_t widget) {
  base::AutoLock hold(lock_);
  windows_.erase(static_cast<gfx::AcceleratedWidget>(widget));
}

}  // namespace ui
