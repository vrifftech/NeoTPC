#pragma once

#include "NeoModulePanel.hpp"
#include "core/TextureDocument.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <filesystem>
#include <string>

namespace neotpc::ui {
inline constexpr unsigned kEditorApiVersion = 1;

class TextureEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path) = 0;
    virtual bool openResource(neoshared::ResourceDocument resource) = 0;
    virtual bool saveActiveAs(const std::filesystem::path& path) = 0;
    virtual bool activateResource(const std::string& identity) = 0;
    virtual std::size_t documentCount() const = 0;
    virtual TextureDocument* activeDocumentModel() = 0;
};

TextureEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context = {});
} // namespace neotpc::ui
