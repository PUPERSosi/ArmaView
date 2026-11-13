#include "LayoutStore.h"
#include "Log.h"
#include "Text.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace {
constexpr const char* kPrefix = "layout ";
} // namespace

void LayoutStore::LoadInitial()
{
    const std::string t = settings_.Block("imgui");
    if (!t.empty()) ImGui::LoadIniSettingsFromMemory(t.c_str(), t.size());
}

std::vector<std::string> LayoutStore::Names() const { return settings_.BlocksWithPrefix(kPrefix); }

void LayoutStore::StoreCurrent()
{
    size_t n = 0;
    const char* t = ImGui::SaveIniSettingsToMemory(&n);
    settings_.SetBlock("imgui", std::string(t, n));
    ui_.Store(settings_);
}

void LayoutStore::SaveAs(const std::string& name)
{
    if (name.empty()) return;
    size_t n = 0;
    const char* t = ImGui::SaveIniSettingsToMemory(&n);
    settings_.SetBlock(kPrefix + name, std::string(t, n));
    StoreCurrent();
    settings_.Save();
    Log::Get().Info("Layout '{}' saved", name);
}

bool LayoutStore::Use(const std::string& name)
{
    if (!settings_.Has(kPrefix + name)) return false;
    pending_ = settings_.Block(kPrefix + name);
    Log::Get().Info("Layout '{}' applied", name);
    return true;
}

void LayoutStore::Remove(const std::string& name)
{
    settings_.RemoveBlock(kPrefix + name);
    settings_.Save();
}

bool LayoutStore::Export(const std::filesystem::path& file)
{
    StoreCurrent();
    const bool ok = settings_.SaveTo(file);
    if (ok) Log::Get().Info("Settings exported to {}", PathToUtf8(file));
    return ok;
}

bool LayoutStore::Import(const std::filesystem::path& file)
{
    if (!settings_.LoadFrom(file))
    {
        Log::Get().Warn("Settings import: {} is not an ArmaView settings file", PathToUtf8(file));
        return false;
    }
    pending_ = settings_.Block("imgui");
    pendingUi_ = true;
    settings_.Save();
    Log::Get().Info("Settings imported from {}", PathToUtf8(file));
    return true;
}

void LayoutStore::BeforeNewFrame()
{
    if (pendingUi_)
    {
        ui_.Load(settings_);
        pendingUi_ = false;
    }
    if (pending_.empty()) return;
    ImGui::ClearIniSettings(); // windows and dock nodes as the layout says, not merged with the current ones
    ImGui::LoadIniSettingsFromMemory(pending_.c_str(), pending_.size());
    pending_.clear();
    ImGui::MarkIniSettingsDirty();
}

void LayoutStore::SaveIfWanted()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantSaveIniSettings) return;
    io.WantSaveIniSettings = false;
    StoreCurrent();
    settings_.Save();
}

void LayoutStore::SaveNow()
{
    StoreCurrent();
    settings_.Save();
}
