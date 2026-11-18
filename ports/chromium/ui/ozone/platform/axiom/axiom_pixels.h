// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_

#include <stdint.h>

#include "base/containers/flat_map.h"
#include "base/files/scoped_file.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/axiom/mojom/axiom_window_pixels.mojom.h"

namespace ui {

// The GPU service's side: the AXIOM windows' files, by widget, as the browser
// sent them (AxiomGpuHost). The canvas drawing a widget (on the compositor's
// thread) takes its own duplicate, maps the window's pixels and notices by
// the generation when the window is replaced or gone. In single-process and
// --in-process-gpu Vertex the same mojo messages arrive in the same process.
class AxiomPixels : public ozone::mojom::AxiomWindowPixels {
 public:
  static AxiomPixels& Get();

  AxiomPixels();
  AxiomPixels(const AxiomPixels&) = delete;
  AxiomPixels& operator=(const AxiomPixels&) = delete;
  ~AxiomPixels() override;

  void Bind(mojo::PendingReceiver<ozone::mojom::AxiomWindowPixels> receiver);

  // A duplicate of the widget's window file (invalid if there's none) and its
  // generation (0 if none).
  base::ScopedFD Dup(gfx::AcceleratedWidget widget, uint64_t* generation);
  // Changes whenever the widget's window is set or removed; 0: there's none.
  uint64_t Generation(gfx::AcceleratedWidget widget);

  // ozone::mojom::AxiomWindowPixels:
  void SetWindow(uint64_t widget, mojo::PlatformHandle window) override;
  void RemoveWindow(uint64_t widget) override;

 private:
  struct Entry {
    base::ScopedFD fd;
    uint64_t generation = 0;
  };

  base::Lock lock_;
  base::flat_map<gfx::AcceleratedWidget, Entry> windows_ GUARDED_BY(lock_);
  uint64_t next_generation_ GUARDED_BY(lock_) = 1;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_PIXELS_H_
