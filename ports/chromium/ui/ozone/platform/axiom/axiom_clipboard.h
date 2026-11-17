// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_AXIOM_AXIOM_CLIPBOARD_H_
#define UI_OZONE_PLATFORM_AXIOM_AXIOM_CLIPBOARD_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/timer/timer.h"
#include "ui/ozone/public/platform_clipboard.h"

namespace ui {

// The clipboard. What Vertex copies is kept here in every format it offers
// (text, HTML, images...); its text also goes to AXIOM's shared clipboard
// (AXWIN_CLIP_SET / AXWIN_CLIP_GET on a /dev/axiom/window fd opened for this
// alone, with no window), and text another app copied comes from there. The
// clipboard's sequence number tells whose it is: Vertex's own data while it's
// the one Vertex set, else AXIOM's text.
class AxiomClipboard : public PlatformClipboard {
 public:
  AxiomClipboard();
  AxiomClipboard(const AxiomClipboard&) = delete;
  AxiomClipboard& operator=(const AxiomClipboard&) = delete;
  ~AxiomClipboard() override;

  // PlatformClipboard:
  void OfferClipboardData(ClipboardBuffer buffer,
                          const DataMap& data_map) override;
  void RequestClipboardData(ClipboardBuffer buffer,
                            const std::string& mime_type,
                            RequestDataClosure callback) override;
  void GetAvailableMimeTypes(ClipboardBuffer buffer,
                             GetMimeTypesClosure callback) override;
  void IsSelectionOwner(ClipboardBuffer buffer,
                        IsSelectionOwnerClosure callback) override;
  void SetClipboardDataChangedCallback(
      ClipboardDataChangedCallback callback) override;
  bool IsSelectionBufferAvailable() const override;

 private:
  int fd() const;  // (opened when first needed; -1 if AXIOM isn't there)
  // AXIOM's change counter (nullopt: no AXIOM clipboard).
  std::optional<uint64_t> Sequence() const;
  // Whether what's on the clipboard is ours (always, without AXIOM's).
  bool IsOwner() const;
  std::optional<std::string> ReadSystemText() const;
  void Poll();

  mutable int fd_ = -2;              // -2: not opened yet
  DataMap data_;                     // what Vertex offered last
  std::optional<uint64_t> our_seq_;  // the counter after our last copy
  uint64_t last_seq_ = 0;            // the counter last seen (for Poll)
  ClipboardDataChangedCallback changed_;
  base::RepeatingTimer poll_timer_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_AXIOM_AXIOM_CLIPBOARD_H_
