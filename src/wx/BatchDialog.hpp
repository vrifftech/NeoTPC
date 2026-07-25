// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <wx/dialog.h>

class wxCheckBox;
class wxChoice;
class wxDirPickerCtrl;
class wxTextCtrl;

namespace neotpc {

class EncodingOptionsPanel;

class BatchDialog final : public wxDialog {
public:
    BatchDialog(wxWindow* parent, const wxString& initialDirectory, bool darkMode);

private:
    void onConvert();

    wxDirPickerCtrl* inputDirectory_ = nullptr;
    wxDirPickerCtrl* outputDirectory_ = nullptr;
    wxChoice* format_ = nullptr;
    wxCheckBox* recursive_ = nullptr;
    wxCheckBox* overwrite_ = nullptr;
    EncodingOptionsPanel* options_ = nullptr;
    wxTextCtrl* report_ = nullptr;
};

} // namespace neotpc
