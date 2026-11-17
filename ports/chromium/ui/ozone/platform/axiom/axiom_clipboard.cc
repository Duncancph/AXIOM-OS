// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/axiom/axiom_clipboard.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/memory/ref_counted_memory.h"
#include "base/posix/eintr_wrapper.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/ozone/platform/axiom/axiom_window_abi.h"

namespace ui {

namespace {

// The ways Chromium names plain text.
constexpr const char* kTextTypes[] = {"text/plain;charset=utf-8", "text/plain",
                                      "UTF8_STRING", "TEXT", "STRING"};

bool IsText(const std::string& mime_type) {
  for (const char* t : kTextTypes) {
    if (mime_type == t) {
      return true;
    }
  }
  return false;
}

}  // namespace

AxiomClipboard::AxiomClipboard() = default;

AxiomClipboard::~AxiomClipboard() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

int AxiomClipboard::fd() const {
  if (fd_ == -2) {
    fd_ = HANDLE_EINTR(open("/dev/axiom/window", O_RDWR | O_CLOEXEC));
  }
  return fd_;
}

std::optional<uint64_t> AxiomClipboard::Sequence() const {
  axwin_clip c = {};
  if (fd() < 0 || ioctl(fd(), AXWIN_CLIP_GET, &c) != 0) {
    return std::nullopt;
  }
  return c.seq;
}

bool AxiomClipboard::IsOwner() const {
  std::optional<uint64_t> seq = Sequence();
  return !seq || (our_seq_ && *seq == *our_seq_);
}

std::optional<std::string> AxiomClipboard::ReadSystemText() const {
  axwin_clip c = {};
  if (fd() < 0 || ioctl(fd(), AXWIN_CLIP_GET, &c) != 0) {
    return std::nullopt;
  }
  // Its length first, then the text (again if it grew in between).
  for (int tries = 0; tries < 3; tries++) {
    std::string text(c.len, '\0');
    axwin_clip get = {};
    get.data = reinterpret_cast<uintptr_t>(text.data());
    get.len = text.size();
    if (ioctl(fd(), AXWIN_CLIP_GET, &get) != 0) {
      return std::nullopt;
    }
    if (get.len <= text.size()) {
      text.resize(get.len);
      return text;
    }
    c.len = get.len;
  }
  return std::nullopt;
}

void AxiomClipboard::OfferClipboardData(ClipboardBuffer buffer,
                                        const DataMap& data_map) {
  if (buffer != ClipboardBuffer::kCopyPaste) {
    return;
  }
  data_ = data_map;
  // The text, for AXIOM's other apps (at most 1 MiB).
  for (const char* t : kTextTypes) {
    auto it = data_.find(t);
    if (it == data_.end() || !it->second || fd() < 0) {
      continue;
    }
    base::span<const uint8_t> bytes = it->second->as_vector();
    axwin_clip c = {};
    c.data = reinterpret_cast<uintptr_t>(bytes.data());
    c.len = std::min<size_t>(bytes.size(), 1u << 20);
    if (ioctl(fd(), AXWIN_CLIP_SET, &c) == 0) {
      our_seq_ = c.seq;
      last_seq_ = c.seq;
    }
    break;
  }
  if (changed_) {
    changed_.Run(buffer);
  }
}

void AxiomClipboard::RequestClipboardData(ClipboardBuffer buffer,
                                          const std::string& mime_type,
                                          RequestDataClosure callback) {
  if (buffer != ClipboardBuffer::kCopyPaste) {
    std::move(callback).Run(nullptr);
    return;
  }
  if (IsOwner()) {
    auto it = data_.find(mime_type);
    std::move(callback).Run(it != data_.end() ? it->second : nullptr);
    return;
  }
  // Another app's: text only.
  std::optional<std::string> text;
  if (IsText(mime_type)) {
    text = ReadSystemText();
  }
  if (!text) {
    std::move(callback).Run(nullptr);
    return;
  }
  std::move(callback).Run(base::MakeRefCounted<base::RefCountedBytes>(
      base::as_byte_span(*text)));
}

void AxiomClipboard::GetAvailableMimeTypes(ClipboardBuffer buffer,
                                           GetMimeTypesClosure callback) {
  std::vector<std::string> types;
  if (buffer == ClipboardBuffer::kCopyPaste) {
    if (IsOwner()) {
      for (const auto& [type, data] : data_) {
        types.push_back(type);
      }
    } else {
      axwin_clip c = {};
      if (fd() >= 0 && ioctl(fd(), AXWIN_CLIP_GET, &c) == 0 && c.len) {
        types = {"text/plain;charset=utf-8", "text/plain"};
      }
    }
  }
  std::move(callback).Run(types);
}

void AxiomClipboard::IsSelectionOwner(ClipboardBuffer buffer,
                                      IsSelectionOwnerClosure callback) {
  std::move(callback).Run(buffer == ClipboardBuffer::kCopyPaste && IsOwner());
}

void AxiomClipboard::SetClipboardDataChangedCallback(
    ClipboardDataChangedCallback callback) {
  changed_ = std::move(callback);
  // Another app copying something: AXIOM only counts, so look now and then.
  if (std::optional<uint64_t> seq = Sequence()) {
    last_seq_ = *seq;
    poll_timer_.Start(FROM_HERE, base::Seconds(1), this, &AxiomClipboard::Poll);
  }
}

void AxiomClipboard::Poll() {
  std::optional<uint64_t> seq = Sequence();
  if (!seq || *seq == last_seq_) {
    return;
  }
  last_seq_ = *seq;
  if (changed_) {
    changed_.Run(ClipboardBuffer::kCopyPaste);
  }
}

bool AxiomClipboard::IsSelectionBufferAvailable() const {
  return false;
}

}  // namespace ui
