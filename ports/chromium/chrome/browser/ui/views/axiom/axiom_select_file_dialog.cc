// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/axiom/axiom_select_file_dialog.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "ui/base/models/table_model.h"
#include "ui/base/models/table_model_observer.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/table/table_view.h"
#include "ui/views/controls/table/table_view_observer.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/view.h"

namespace {

struct Entry {
  std::string name;
  bool is_dir = false;
  int64_t size = 0;
};

// A folder's contents, folders first, then by name (hidden files left out).
// Runs on a thread that may block.
std::optional<std::vector<Entry>> ListFolder(const base::FilePath& dir) {
  if (!base::DirectoryExists(dir)) {
    return std::nullopt;
  }
  std::vector<Entry> out;
  base::FileEnumerator e(dir, /*recursive=*/false,
                         base::FileEnumerator::FILES |
                             base::FileEnumerator::DIRECTORIES);
  for (base::FilePath p = e.Next(); !p.empty(); p = e.Next()) {
    std::string name = p.BaseName().value();
    if (name.empty() || name[0] == '.') {
      continue;
    }
    base::FileEnumerator::FileInfo info = e.GetInfo();
    out.push_back({name, info.IsDirectory(), info.GetSize()});
  }
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.is_dir != b.is_dir) {
      return a.is_dir;
    }
    return base::CompareCaseInsensitiveASCII(a.name, b.name) < 0;
  });
  return out;
}

bool IsFolderType(ui::SelectFileDialog::Type type) {
  return type == ui::SelectFileDialog::SELECT_FOLDER ||
         type == ui::SelectFileDialog::SELECT_UPLOAD_FOLDER ||
         type == ui::SelectFileDialog::SELECT_EXISTING_FOLDER;
}

enum Column { kName, kSize };

// The dialog's contents: the folder's path (editable) with an Up button, its
// contents, and for saving, the file's name.
class FileDialogContents : public views::View,
                           public ui::TableModel,
                           public views::TableViewObserver,
                           public views::TextfieldController {
 public:
  FileDialogContents(ui::SelectFileDialog::Type type,
                     const base::FilePath& dir,
                     const std::string& name,
                     std::vector<std::string> extensions,
                     const std::string& default_extension,
                     base::RepeatingClosure accept)
      : type_(type),
        extensions_(std::move(extensions)),
        default_extension_(default_extension),
        accept_(std::move(accept)) {
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 10));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    auto* row = AddChildView(std::make_unique<views::BoxLayoutView>());
    row->SetBetweenChildSpacing(8);
    row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&FileDialogContents::Up, base::Unretained(this)),
        u"Up"));
    path_field_ = row->AddChildView(std::make_unique<views::Textfield>());
    path_field_->set_controller(this);
    path_field_->GetViewAccessibility().SetName(u"Folder");
    row->SetFlexForView(path_field_, 1);

    std::vector<ui::TableColumn> columns(2);
    columns[0].id = kName;
    columns[0].title = u"Name";
    columns[0].percent = 0.75f;
    columns[1].id = kSize;
    columns[1].title = u"Size";
    columns[1].percent = 0.25f;
    columns[1].alignment = ui::TableColumn::RIGHT;
    auto table = std::make_unique<views::TableView>(
        this, columns, views::TableType::kTextOnly, /*single_selection=*/true);
    table_ = table.get();
    table_->set_observer(this);
    scroll_ = AddChildView(
        views::TableView::CreateScrollViewWithTable(std::move(table)));
    scroll_->SetPreferredSize(gfx::Size(560, 320));
    layout->SetFlexForView(scroll_, 1);

    status_ = AddChildView(std::make_unique<views::Label>());
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);

    if (type_ == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
      auto* name_row = AddChildView(std::make_unique<views::BoxLayoutView>());
      name_row->SetBetweenChildSpacing(8);
      name_row->AddChildView(std::make_unique<views::Label>(u"Name:"));
      name_field_ = name_row->AddChildView(std::make_unique<views::Textfield>());
      name_field_->SetText(base::UTF8ToUTF16(name));
      name_field_->set_controller(this);
      name_field_->GetViewAccessibility().SetName(u"File name");
      name_row->SetFlexForView(name_field_, 1);
    }
    Navigate(dir);
  }
  FileDialogContents(const FileDialogContents&) = delete;
  FileDialogContents& operator=(const FileDialogContents&) = delete;
  ~FileDialogContents() override {
    // The table goes first: it lets go of its model (this) as it's destroyed.
    table_ = nullptr;
    RemoveChildViewT(scroll_.get());
  }

  views::View* initially_focused() {
    return name_field_ ? static_cast<views::View*>(name_field_) : table_.get();
  }

  // The OK button: true and the path when the dialog is done; false to stay
  // (a folder was opened, or nothing is chosen yet).
  bool Accept(base::FilePath* out) {
    std::optional<size_t> row = table_->GetFirstSelectedRow();
    if (IsFolderType(type_)) {
      *out = row && entries_[*row].is_dir ? dir_.Append(entries_[*row].name)
                                          : dir_;
      return true;
    }
    if (name_field_) {
      std::string name = base::UTF16ToUTF8(name_field_->GetText());
      if (name.empty()) {
        return false;
      }
      base::FilePath path =
          name[0] == '/' ? base::FilePath(name) : dir_.Append(name);
      if (path.Extension().empty() && !default_extension_.empty()) {
        path = path.AddExtension(default_extension_);
      }
      *out = path;
      return true;
    }
    if (!row) {
      return false;
    }
    if (entries_[*row].is_dir) {
      Navigate(dir_.Append(entries_[*row].name));
      return false;
    }
    *out = dir_.Append(entries_[*row].name);
    return true;
  }

  // ui::TableModel:
  size_t RowCount() const override { return entries_.size(); }
  std::u16string GetText(size_t row, int column_id) const override {
    const Entry& e = entries_[row];
    if (column_id == kName) {
      return base::UTF8ToUTF16(e.name) + (e.is_dir ? u"/" : u"");
    }
    return e.is_dir ? u"Folder"
                    : ui::FormatBytes(base::ByteSize(
                          static_cast<uint64_t>(std::max<int64_t>(e.size, 0))));
  }
  void SetObserver(ui::TableModelObserver* observer) override {
    observer_ = observer;
  }

  // views::TableViewObserver:
  void OnSelectionChanged() override {
    std::optional<size_t> row = table_->GetFirstSelectedRow();
    if (name_field_ && row && !entries_[*row].is_dir) {
      name_field_->SetText(base::UTF8ToUTF16(entries_[*row].name));
    }
  }
  void OnDoubleClick() override {
    std::optional<size_t> row = table_->GetFirstSelectedRow();
    if (!row) {
      return;
    }
    if (entries_[*row].is_dir) {
      Navigate(dir_.Append(entries_[*row].name));
    } else if (!IsFolderType(type_)) {
      accept_.Run();
    }
  }

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override {
    if (key_event.type() != ui::EventType::kKeyPressed ||
        key_event.key_code() != ui::VKEY_RETURN) {
      return false;
    }
    if (sender == path_field_) {
      Navigate(base::FilePath(base::UTF16ToUTF8(path_field_->GetText())));
      return true;
    }
    accept_.Run();
    return true;
  }

 private:
  void Up() {
    if (dir_.DirName() != dir_) {
      Navigate(dir_.DirName());
    }
  }

  void Navigate(const base::FilePath& dir) {
    pending_dir_ = dir;
    status_->SetText(u"Reading the folder...");
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&ListFolder, dir),
        base::BindOnce(&FileDialogContents::OnListed,
                       weak_factory_.GetWeakPtr(), dir));
  }

  void OnListed(const base::FilePath& dir,
                std::optional<std::vector<Entry>> entries) {
    if (dir != pending_dir_) {
      return;  // (another folder was asked for since)
    }
    if (!entries) {
      status_->SetText(u"That folder can't be opened.");
      path_field_->SetText(base::UTF8ToUTF16(dir_.value()));
      return;
    }
    dir_ = dir;
    path_field_->SetText(base::UTF8ToUTF16(dir_.value()));
    entries_.clear();
    for (Entry& e : *entries) {
      if (e.is_dir || Wanted(e.name)) {
        entries_.push_back(std::move(e));
      }
    }
    status_->SetText(entries_.empty() ? u"(empty)" : u"");
    if (observer_) {
      observer_->OnModelChanged();
    }
  }

  // Whether a file has one of the extensions asked for (folders: none).
  bool Wanted(const std::string& name) const {
    if (IsFolderType(type_)) {
      return false;
    }
    if (extensions_.empty() ||
        type_ == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
      return true;
    }
    std::string ext = base::ToLowerASCII(base::FilePath(name).Extension());
    for (const std::string& want : extensions_) {
      if (ext == "." + base::ToLowerASCII(want)) {
        return true;
      }
    }
    return false;
  }

  const ui::SelectFileDialog::Type type_;
  const std::vector<std::string> extensions_;
  const std::string default_extension_;
  base::RepeatingClosure accept_;
  base::FilePath dir_, pending_dir_;
  std::vector<Entry> entries_;
  raw_ptr<ui::TableModelObserver> observer_ = nullptr;
  raw_ptr<views::TableView> table_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::Textfield> path_field_ = nullptr;
  raw_ptr<views::Textfield> name_field_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  base::WeakPtrFactory<FileDialogContents> weak_factory_{this};
};

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET): every way the
// widget closes comes through OnClose.
class AxiomSelectFileDialog : public ui::SelectFileDialog {
 public:
  AxiomSelectFileDialog(Listener* listener,
                        std::unique_ptr<ui::SelectFilePolicy> policy)
      : ui::SelectFileDialog(listener, std::move(policy)) {}
  AxiomSelectFileDialog(const AxiomSelectFileDialog&) = delete;
  AxiomSelectFileDialog& operator=(const AxiomSelectFileDialog&) = delete;

  // ui::BaseShellDialog:
  bool IsRunning(gfx::NativeWindow owning_window) const override {
    return widget_ && owner_ == owning_window;
  }
  void ListenerDestroyed() override {
    listener_ = nullptr;
    if (widget_) {
      Finish(std::nullopt);
    }
  }

 protected:
  // ui::SelectFileDialog:
  void SelectFileImpl(Type type,
                      const std::u16string& title,
                      const base::FilePath& default_path,
                      const FileTypeInfo* file_types,
                      int file_type_index,
                      const base::FilePath::StringType& default_extension,
                      gfx::NativeWindow owning_window,
                      const GURL* caller) override {
    if (widget_) {
      Finish(std::nullopt);
    }
    owner_ = owning_window;
    // Where to start: the folder given (or the file's folder), else home.
    base::FilePath dir = default_path, home;
    std::string name;
    if (!dir.empty() && !dir.EndsWithSeparator() &&
        (type == SELECT_SAVEAS_FILE || type == SELECT_OPEN_FILE ||
         type == SELECT_OPEN_MULTI_FILE) &&
        !dir.Extension().empty()) {
      name = dir.BaseName().value();
      dir = dir.DirName();
    }
    if (!dir.IsAbsolute()) {
      base::PathService::Get(base::DIR_HOME, &home);
      dir = dir.empty() || dir.value() == "." ? home : home.Append(dir);
    }
    std::vector<std::string> extensions;
    if (file_types && !file_types->include_all_files) {
      for (size_t i = 0; i < file_types->extensions.size(); i++) {
        if (file_type_index > 0 &&
            i + 1 != static_cast<size_t>(file_type_index)) {
          continue;
        }
        for (const auto& ext : file_types->extensions[i]) {
          extensions.push_back(ext);
        }
      }
    }
    const bool save = type == SELECT_SAVEAS_FILE;
    const bool folder = IsFolderType(type);
    delegate_ = std::make_unique<views::DialogDelegate>();
    delegate_->SetTitle(!title.empty() ? title
                        : save         ? u"Save File"
                        : folder       ? u"Select Folder"
                                       : u"Open File");
    delegate_->SetButtonLabel(ui::mojom::DialogButton::kOk,
                              save     ? u"Save"
                              : folder ? u"Select"
                                       : u"Open");
    delegate_->SetModalType(ui::mojom::ModalType::kWindow);
    contents_ = delegate_->SetContentsView(std::make_unique<FileDialogContents>(
        type, dir, name, std::move(extensions), default_extension,
        base::BindRepeating(&AxiomSelectFileDialog::OnClose,
                            base::Unretained(this),
                            views::Widget::ClosedReason::kAcceptButtonClicked)));
    delegate_->SetInitiallyFocusedView(contents_->initially_focused());
    views::Widget::InitParams params =
        views::DialogDelegate::GetDialogWidgetInitParams(
            delegate_.get(), /*context=*/nullptr, owning_window, gfx::Rect());
    params.ownership = views::Widget::InitParams::CLIENT_OWNS_WIDGET;
    widget_ = std::make_unique<views::Widget>();
    widget_->Init(std::move(params));
    WatchClose();
    keep_alive_ = this;  // (until it's done)
    widget_->Show();
  }
  bool HasMultipleFileTypeChoicesImpl() override { return false; }

 private:
  ~AxiomSelectFileDialog() override = default;

  void WatchClose() {
    widget_->MakeCloseSynchronous(base::BindOnce(
        &AxiomSelectFileDialog::OnClose, base::Unretained(this)));
  }

  void OnClose(views::Widget::ClosedReason reason) {
    if (!widget_) {
      return;
    }
    if (reason != views::Widget::ClosedReason::kAcceptButtonClicked) {
      Finish(std::nullopt);
      return;
    }
    base::FilePath path;
    if (!contents_->Accept(&path)) {
      WatchClose();  // (it stays open: a folder was opened)
      return;
    }
    Finish(path);
  }

  void Finish(std::optional<base::FilePath> path) {
    scoped_refptr<AxiomSelectFileDialog> self = std::move(keep_alive_);
    contents_ = nullptr;
    widget_.reset();
    delegate_.reset();
    if (!listener_) {
      return;
    }
    if (path) {
      listener_->FileSelected(ui::SelectedFileInfo(*path), 0);
    } else {
      listener_->FileSelectionCanceled();
    }
  }

  gfx::NativeWindow owner_ = gfx::NativeWindow();
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<FileDialogContents> contents_ = nullptr;
  scoped_refptr<AxiomSelectFileDialog> keep_alive_;
};

}  // namespace

AxiomSelectFileDialogFactory::AxiomSelectFileDialogFactory() = default;
AxiomSelectFileDialogFactory::~AxiomSelectFileDialogFactory() = default;

// static
void AxiomSelectFileDialogFactory::InstallIfAxiom() {
  if (ui::OzonePlatform::GetPlatformNameForTest() == "axiom") {
    ui::SelectFileDialog::SetFactory(
        std::make_unique<AxiomSelectFileDialogFactory>());
  }
}

ui::SelectFileDialog* AxiomSelectFileDialogFactory::Create(
    ui::SelectFileDialog::Listener* listener,
    std::unique_ptr<ui::SelectFilePolicy> policy) {
  return new AxiomSelectFileDialog(listener, std::move(policy));
}
