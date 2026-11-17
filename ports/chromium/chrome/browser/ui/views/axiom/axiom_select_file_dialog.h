// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_AXIOM_AXIOM_SELECT_FILE_DIALOG_H_
#define CHROME_BROWSER_UI_VIEWS_AXIOM_AXIOM_SELECT_FILE_DIALOG_H_

#include <memory>

#include "ui/shell_dialogs/select_file_dialog_factory.h"

// File open / save dialogs on AXIOM (ozone_platform=axiom), where there's no
// GTK, Qt or desktop portal: a dialog drawn by views, showing AXIOM's folders
// (as Linux programs see them: /home, /usb0...).
class AxiomSelectFileDialogFactory : public ui::SelectFileDialogFactory {
 public:
  AxiomSelectFileDialogFactory();
  ~AxiomSelectFileDialogFactory() override;

  // Installs it when the Ozone platform is AXIOM's.
  static void InstallIfAxiom();

  // ui::SelectFileDialogFactory:
  ui::SelectFileDialog* Create(
      ui::SelectFileDialog::Listener* listener,
      std::unique_ptr<ui::SelectFilePolicy> policy) override;
};

#endif  // CHROME_BROWSER_UI_VIEWS_AXIOM_AXIOM_SELECT_FILE_DIALOG_H_
