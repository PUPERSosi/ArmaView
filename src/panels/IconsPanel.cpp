#include "IconsPanel.h"
#include "ArmaConfig.h"
#include "ImageFiles.h"
#include "Lang.h"
#include "Log.h"
#include "RvMaterial.h"
#include "Gltf.h"
#include "Text.h"
#include "Unit.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
const char* const kRoots[] = { "CfgWeapons", "CfgVehicles", "CfgGlasses", "CfgMagazines" };

// only the source formats are read: anything else (a binarized model) gets the standard message
bool SourceModel(const fs::path& file)
{
    if (IsGltfFile(file)) return true;
    char sig[4] = {};
    std::ifstream(file, std::ios::binary).read(sig, 4);
    return std::string_view(sig, 4) == "MLOD";
}

bool InheritsAny(const CfgClass* c, std::initializer_list<std::string_view> names)
{
    for (const CfgClass* k : c->Chain())
        for (auto n : names)
            if (IEquals(k->name, n) || IEquals(k->baseName, n)) return true;
    return false;
}
} // namespace

std::vector<IconsPanel::ClassItem> IconsPanel::ConfigClasses(bool modelOnly) const
{
    std::vector<ClassItem> out;
    const ArmaConfig* cfg = s_.modCfg ? s_.modCfg.get() : nullptr;
    if (!cfg) return out;
    const std::string want = s_.hasModel ? NormalizeArmaPath(ArmaPathOf(s_.model.path, s_.projectRoot)) : std::string();
    for (const char* rootName : kRoots)
    {
        const CfgClass* root = cfg->Root().FindClass(rootName, false);
        if (!root) continue;
        for (const auto& k : root->classes)
        {
            if (k->external || k->GetNumber("scope", 0) != 2) continue;
            const std::string model = k->GetString("model");
            if (model.empty()) continue;
            IconSubject sub = IconSubjectOf(*cfg, rootName, k->name, s_.projectRoot);
            if (modelOnly && NormalizeArmaPath(sub.modelArma) != want) continue;
            if (sub.model.empty()) continue; // its model is not on disk
            ClassItem it;
            it.root = rootName;
            it.cls = k->name;
            it.model = sub.model;
            it.skin = std::move(sub.skin);
            it.category = SuggestIconCategory(*cfg, rootName, k->name, &it.reason);
            it.preset = it.category != IconCategory::Count ? CategoryInfo(it.category).preset : IconPreset::Custom;
            it.man = IEquals(rootName, "CfgVehicles") && InheritsAny(k.get(), { "CAManBase" });
            it.vehicle = it.preset == IconPreset::VehiclePicture;
            it.binarized = !SourceModel(it.model);
            out.push_back(std::move(it));
        }
    }
    return out;
}

std::string IconsPanel::SqfForClasses(const std::vector<ClassItem>& items)
{
    std::vector<std::string> cls;
    for (const auto& i : items)
        if (i.selected && IEquals(i.root, "CfgVehicles")) cls.push_back(i.cls);
    return cls.empty() ? std::string() : EditorPreviewSqf(cls);
}

const std::vector<IconExample>& IconsPanel::Examples(IconCategory c)
{
    const ArmaConfig* game = s_.game.get();
    if (game != examplesOf_)
    {
        examplesOf_ = game;
        examples_.clear();
    }
    auto it = examples_.find(int(c));
    if (it == examples_.end())
        it = examples_.emplace(int(c), game ? BiExamples(*game, c, s_.projectRoot.empty() ? fs::path(L"P:\\") : s_.projectRoot) : std::vector<IconExample>{}).first;
    return it->second;
}

IconCategory IconsPanel::Category() const
{
    if (cls_ < 0 || cls_ >= int(classes_.size())) return IconCategory::Custom;
    const ClassItem& c = classes_[size_t(cls_)];
    if (auto it = saved.find(c.cls); it != saved.end())
    {
        const size_t k = it->second.find("cat=");
        if (k != std::string::npos)
            if (const IconCategory m = IconCategoryById(it->second.substr(k + 4, it->second.find(';', k) - k - 4)); m != IconCategory::Count) return m;
    }
    return c.category == IconCategory::Count ? IconCategory::Custom : c.category;
}

namespace
{
float Num(const std::string& s, const char* key, float def)
{
    const std::string k = std::string(key) + "=";
    size_t p = s.find(k);
    while (p != std::string::npos && p > 0 && s[p - 1] != ';') p = s.find(k, p + 1);
    return p == std::string::npos ? def : float(std::atof(s.c_str() + p + k.size()));
}
} // namespace

void IconsPanel::SaveClass()
{
    if (cls_ < 0 || cls_ >= int(classes_.size())) return;
    const ClassItem& it = classes_[size_t(cls_)];
    std::string v;
    if (cat_ != it.category) v += std::format("cat={};", CategoryInfo(cat_).id);
    const IconSettings d = DefaultIconSettings(set_.preset, it.vehicle || it.man, it.man);
    auto put = [&](const char* k, float x, float def) { if (std::abs(x - def) > 1e-4f) v += std::format("{}={:g};", k, x); };
    put("yaw", set_.yawDeg, d.yawDeg); put("pitch", set_.pitchDeg, d.pitchDeg); put("roll", set_.rollDeg, d.rollDeg);
    put("fov", set_.fovDeg, d.fovDeg); put("zoom", set_.zoom, d.zoom);
    put("ox", set_.offsetX, 0); put("oy", set_.offsetY, 0); put("oz", set_.offsetZ, 0);
    put("fit", set_.fitToBorder ? 1.0f : 0.0f, 1); put("ev", set_.exposure, 0); put("sat", set_.saturation, 1); put("con", set_.contrast, 1);
    put("outline", float(set_.outline), 0); put("white", set_.whiteSilhouette ? 1.0f : 0.0f, d.whiteSilhouette ? 1.0f : 0.0f);
    put("ml", set_.marginL, d.marginL); put("mr", set_.marginR, d.marginR); put("mt", set_.marginT, d.marginT); put("mb", set_.marginB, d.marginB);
    {
        const auto h = it.cls == hidingCls_ && hidingActive_ && hooks_.visibility ? hooks_.visibility->Take()
                       : iconHiding_.count(it.cls) ? iconHiding_[it.cls] : ModelVisibility::Snapshot{};
        if (const std::string t = ModelVisibility::SnapshotText(h); !t.empty()) v += "vis=" + t + ";";
    }
    if (v.empty()) saved.erase(it.cls);
    else saved[it.cls] = v;
}

void IconsPanel::LoadClass()
{
    if (cls_ < 0 || cls_ >= int(classes_.size())) return;
    const auto f = saved.find(classes_[size_t(cls_)].cls);
    if (f == saved.end()) return;
    const std::string& v = f->second;
    if (const size_t p = v.find("vis="); p != std::string::npos)
        iconHiding_[classes_[size_t(cls_)].cls] = ModelVisibility::SnapshotFromText(v.substr(p + 4, v.find(';', p) - p - 4));
    set_.yawDeg = Num(v, "yaw", set_.yawDeg); set_.pitchDeg = Num(v, "pitch", set_.pitchDeg); set_.rollDeg = Num(v, "roll", set_.rollDeg);
    set_.fovDeg = Num(v, "fov", set_.fovDeg); set_.zoom = Num(v, "zoom", set_.zoom);
    set_.offsetX = Num(v, "ox", 0); set_.offsetY = Num(v, "oy", 0); set_.offsetZ = Num(v, "oz", 0);
    set_.fitToBorder = Num(v, "fit", 1) != 0; set_.exposure = Num(v, "ev", 0); set_.saturation = Num(v, "sat", 1); set_.contrast = Num(v, "con", 1);
    set_.outline = int(Num(v, "outline", 0)); set_.whiteSilhouette = Num(v, "white", set_.whiteSilhouette ? 1.0f : 0.0f) != 0;
    set_.marginL = Num(v, "ml", set_.marginL); set_.marginR = Num(v, "mr", set_.marginR); set_.marginT = Num(v, "mt", set_.marginT); set_.marginB = Num(v, "mb", set_.marginB);
    dirty_ = true;
}

void IconsPanel::ResetCamera()
{
    const ClassItem* c = cls_ >= 0 && cls_ < int(classes_.size()) ? &classes_[size_t(cls_)] : nullptr;
    const IconSettings d = DefaultIconSettings(set_.preset, !c || c->vehicle || c->man, c && c->man);
    set_.yawDeg = d.yawDeg; set_.pitchDeg = d.pitchDeg; set_.rollDeg = d.rollDeg; set_.fovDeg = d.fovDeg;
    set_.zoom = d.zoom; set_.offsetX = set_.offsetY = set_.offsetZ = 0; set_.fitToBorder = true;
    dirty_ = true;
    SaveClass();
}

void IconsPanel::ApplyCategory(IconCategory c, bool manual)
{
    cat_ = c;
    example_ = 0;
    if (manual && cls_ >= 0 && cls_ < int(classes_.size()))
    {
        const ClassItem& it = classes_[size_t(cls_)];
        if (c == it.category) saved.erase(it.cls); // back to the automatic one
        else saved[it.cls] = std::format("cat={};", CategoryInfo(c).id);
    }
    ApplyPreset(CategoryInfo(c).preset);
}

void IconsPanel::ApplyPreset(IconPreset p)
{
    const ClassItem* c = cls_ >= 0 && cls_ < int(classes_.size()) ? &classes_[size_t(cls_)] : nullptr;
    set_ = DefaultIconSettings(p, !c || c->vehicle || c->man, c && c->man);
    dirty_ = true;
}

void IconsPanel::EnterHiding()
{
    ModelVisibility* vis = hooks_.visibility;
    if (!vis || !s_.hasModel || cls_ < 0 || cls_ >= int(classes_.size())) return;
    std::error_code ec;
    if (hidingActive_)
    {
        if (hidingCls_ == classes_[size_t(cls_)].cls && fs::equivalent(hidingModel_, s_.model.path, ec) && !vis->persist) return;
        hidingActive_ = false; // the model was loaded again meanwhile: its own hiding is in place now
    }
    hidingCls_ = classes_[size_t(cls_)].cls;
    hidingModel_ = s_.model.path;
    modelHiding_ = vis->Take();
    vis->persist = false;
    const auto f = iconHiding_.find(hidingCls_);
    vis->Restore(f != iconHiding_.end() ? f->second : ModelVisibility::Snapshot{});
    hidingActive_ = true;
    host_.RebuildGeometry();
    dirty_ = true;
}

void IconsPanel::LeaveHiding()
{
    ModelVisibility* vis = hooks_.visibility;
    if (!hidingActive_ || !vis) return;
    iconHiding_[hidingCls_] = vis->Take();
    vis->Restore(modelHiding_);
    vis->persist = true;
    hidingActive_ = false;
    // that class's kept line: its hiding replaced (the window may already show another class)
    std::string& line = saved[hidingCls_];
    if (const size_t p = line.find("vis="); p != std::string::npos) line.erase(p, line.find(';', p) + 1 - p);
    if (const std::string t = ModelVisibility::SnapshotText(iconHiding_[hidingCls_]); !t.empty()) line += "vis=" + t + ";";
    if (line.empty()) saved.erase(hidingCls_);
    host_.RebuildGeometry();
}

bool IconsPanel::HideForIcon(const std::string& name)
{
    EnterHiding();
    if (!hidingActive_ || !hooks_.visibility) return false;
    if (name == "reset") { ResetHiding(); return true; }
    LodVisibility& v = hooks_.visibility->Lod(s_.model.lods[size_t(s_.lod)].resolution);
    v.hidden.insert(ToLower(name));
    host_.RebuildGeometry();
    Changed();
    return true;
}

void IconsPanel::ResetHiding()
{
    if (!hooks_.visibility || !hidingActive_) return;
    hooks_.visibility->Restore({});
    iconHiding_.erase(hidingCls_);
    SaveClass();
    host_.RebuildGeometry();
    dirty_ = true;
}

void IconsPanel::ShowClass(const ClassItem& it)
{
    LeaveHiding();
    msg_.clear();
    if (it.binarized)
    {
        msg_ = std::format("{}: {}", PathToUtf8(it.model.filename()), kBinarizedNotSupported);
        return;
    }
    std::error_code ec;
    if (!s_.hasModel || !fs::equivalent(s_.model.path, it.model, ec))
        if (!hooks_.loadModel || !hooks_.loadModel(it.model)) { msg_ = "could not open " + PathToUtf8(it.model); return; }
    // the class's re-texture through the config skins (as the Materials window; "" hides a selection)
    int idx = -1;
    for (int k = 0; k < int(s_.configSkins.size()); ++k)
        if (IEquals(s_.configSkins[size_t(k)].className, it.skin.className) && IEquals(s_.configSkins[size_t(k)].cfgRoot, it.skin.cfgRoot)) idx = k;
    if (idx < 0 && !it.skin.selections.empty())
    {
        s_.configSkins.push_back(it.skin);
        idx = int(s_.configSkins.size()) - 1;
    }
    s_.activeSkin = idx;
    host_.RebuildGeometry();
}

bool IconsPanel::SelectClass(const std::string& cls)
{
    if (classesOf_ != s_.configCpp || classes_.empty())
    {
        classesOf_ = s_.configCpp;
        classes_ = ConfigClasses(false);
    }
    for (int k = 0; k < int(classes_.size()); ++k)
        if (IEquals(classes_[size_t(k)].cls, cls))
        {
            PickClass(k);
            return true;
        }
    return false;
}

void IconsPanel::PickClass(int i)
{
    cls_ = i;
    if (i >= 0 && i < int(classes_.size())) ShowClass(classes_[size_t(i)]);
    const std::string n = i >= 0 && i < int(classes_.size()) ? classes_[size_t(i)].cls : PathToUtf8(s_.model.path.stem());
    strncpy_s(name_, n.c_str(), _TRUNCATE);
    ApplyCategory(Category(), false);
    LoadClass();
    EnterHiding();
}

fs::path IconsPanel::OutputFile() const
{
    const fs::path base = !s_.configCpp.empty() ? s_.configCpp.parent_path() : s_.model.path.parent_path();
    const IconStandard& st = Standard(set_.preset);
    return base / st.folder / IconFileName(set_.preset, name_);
}

bool IconsPanel::RenderToFile(IconPreset preset, const fs::path& file, std::string& message, RgbaImage* picture)
{
    return RenderToFile(set_.preset == preset ? set_ : DefaultIconSettings(preset), file, message, picture);
}

bool IconsPanel::RenderToFile(const IconSettings& s, const fs::path& file, std::string& message, RgbaImage* picture)
{
    RgbaImage img;
    if (!r_.Render(s, img, message)) return false;
    std::string ext = ToLower(PathToUtf8(file.extension()));
    fs::path png = file;
    if (ext == ".paa") png.replace_extension(".png");
    if (!WriteImageFile(png, img, message)) return false;
    if (ext == ".paa")
    {
        if (!PaaSizeOk(img.w, img.h)) { message = std::format("{} x {}: PAA needs powers of two", img.w, img.h); return false; }
        if (!ConvertToPaa(fs::path(Utf8ToWide(imageToPaa)), png, file, message)) return false;
    }
    message = std::format("{} ({} x {})", PathToUtf8(file), img.w, img.h);
    if (picture) *picture = std::move(img);
    return true;
}

bool IconsPanel::Save(std::string& message)
{
    fs::path f = OutputFile();
    const bool jpg = set_.preset == IconPreset::EditorPreview;
    if (!jpg && !paa_) f.replace_extension(".png");
    const bool ok = RenderToFile(set_.preset, f, message);
    if (ok) Log::Get().Info("Icons: saved {}", message);
    else Log::Get().Warn("Icons: {}", message);
    return ok;
}

void IconsPanel::AfterSceneDraw()
{
    if (batchRunning_)
    {
        if (batchNext_ >= batch_.size())
        {
            batchRunning_ = false;
            msg_ = std::format("batch: {} picture(s) written, {} failed", batchDone_, batchFailed_);
            Log::Get().Info("Icons: {}", msg_);
            return;
        }
        ClassItem& it = batch_[batchNext_];
        if (batchPhase_ == 0 && it.binarized)
        {
            ++batchFailed_;
            Log::Get().Warn("Icons: {}: {}: {}", it.cls, PathToUtf8(it.model.filename()), kBinarizedNotSupported);
            ++batchNext_;
            return;
        }
        if (batchPhase_ == 0)
        {
            if (it.man && hooks_.assembleUnit) hooks_.assembleUnit(it.cls);
            else if (hooks_.loadModel) hooks_.loadModel(it.model);
            batchPhase_ = 1;
            batchWait_ = 0;
            return;
        }
        if (batchPhase_ == 1)
        {
            batchWait_ = hooks_.ready && hooks_.ready() ? batchWait_ + 1 : 0; // a few calm frames: textures in place
            if (batchWait_ >= 4) batchPhase_ = 2;
            return;
        }
        // the pictures of this class
        std::vector<IconPreset> kinds;
        if (it.preset != IconPreset::EditorPreview && it.preset != IconPreset::Count) kinds.push_back(it.preset);
        if (it.vehicle && batchMapIcons_) kinds.push_back(IconPreset::VehicleMapIcon);
        if (batchPreviews_ && IEquals(it.root, "CfgVehicles") && it.preset != IconPreset::Gear) kinds.push_back(IconPreset::EditorPreview);
        const fs::path base = !s_.configCpp.empty() ? s_.configCpp.parent_path() : it.model.parent_path();
        for (IconPreset k : kinds)
        {
            const IconStandard& st = Standard(k);
            IconSettings keep = set_;
            set_ = DefaultIconSettings(k, it.vehicle || it.man, it.man);
            fs::path f = base / st.folder / IconFileName(k, it.cls);
            if (k != IconPreset::EditorPreview && !paa_) f.replace_extension(".png");
            std::string m;
            if (RenderToFile(k, f, m)) ++batchDone_;
            else { ++batchFailed_; Log::Get().Warn("Icons: {} {}: {}", it.cls, st.id, m); }
            set_ = keep;
        }
        ++batchNext_;
        batchPhase_ = 0;
        return;
    }
    // textures still streaming in: the preview again once they are there
    const bool ready = !hooks_.ready || hooks_.ready();
    if (!ready) previewStale_ = true;
    else if (previewStale_) { previewStale_ = false; dirty_ = true; }
    if (!dirty_ || !s_.hasModel) return;
    dirty_ = false;
    if (cls_ >= 0 && cls_ < int(classes_.size()) && classes_[size_t(cls_)].binarized)
    {
        // the class's model is not read: no picture of another model in its place (msg_ says why)
        srv_ = nullptr;
        picture_ = {};
        return;
    }
    std::string err;
    IconSettings s = set_;
    s.supersample = std::min(s.supersample, 2); // the preview: quicker; the saved picture uses the full supersampling
    if (r_.Render(s, picture_, err)) { srv_ = r_.Upload(picture_); msg_.clear(); }
    else { srv_ = nullptr; msg_ = err; }
}

void IconsPanel::Draw(const std::string& title)
{
    if (showStandards_) DrawStandards();
    if (!ui::Begin(title.c_str())) { ImGui::End(); return; }
    const float dpi = host_.Dpi();
    if (!s_.hasModel)
    {
        ui::EmptyState("Open a model (MLOD or glTF): its pictures are made here by BI's standards.");
        ImGui::End();
        return;
    }
    if (classesOf_ != s_.configCpp)
    {
        // every class of the mod's config with a model on disk; first the one of the open model
        classesOf_ = s_.configCpp;
        classes_ = ConfigClasses(false);
        int first = -1;
        std::error_code ec;
        for (int k = 0; k < int(classes_.size()) && first < 0; ++k)
            if (fs::equivalent(classes_[size_t(k)].model, s_.model.path, ec)) first = k;
        PickClass(first);
    }
    // class
    ui::SeparatorText("Class");
    const std::string cur = cls_ >= 0 ? classes_[size_t(cls_)].root + "::" + classes_[size_t(cls_)].cls : std::string(T("(no class of this model in the config)"));
    ImGui::SetNextItemWidth(-1);
    if (ui::BeginCombo("##iconclass", cur.c_str()))
    {
        for (int i = 0; i < int(classes_.size()); ++i)
            if (ui::SelectableRaw((classes_[size_t(i)].root + "::" + classes_[size_t(i)].cls + (classes_[size_t(i)].binarized ? " (binarized)" : "")).c_str(), i == cls_))
                PickClass(i);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(200 * dpi);
    ui::InputText("Name in the file", name_, sizeof(name_));
    if (cls_ >= 0 && classes_[size_t(cls_)].man)
    {
        ImGui::SameLine();
        if (ui::SmallButton("Dress as this unit") && hooks_.assembleUnit) { hooks_.assembleUnit(classes_[size_t(cls_)].cls); dirty_ = true; }
    }
    // a dressed unit: the parts the picture does not show, and why (a binarized part: the standard message)
    if (s_.unit.Active())
        for (const UnitPart& p : s_.unit.Parts())
            if (p.enabled && !p.Shown())
            {
                const std::string why = !p.problem.empty() ? p.problem : p.model ? p.model->message : std::string("not loaded");
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", TF("not in the picture: {} {}: {}", UnitSlotName(p.slot), p.cls, why).c_str());
                ImGui::PopTextWrapPos();
            }
    // preset
    ui::SeparatorText("Category");
    {
        const ClassItem* ci = cls_ >= 0 && cls_ < int(classes_.size()) ? &classes_[size_t(cls_)] : nullptr;
        const bool manual = ci && saved.count(ci->cls) && saved[ci->cls].find("cat=") != std::string::npos;
        std::string shown = T(CategoryInfo(cat_).name);
        if (!manual && ci) shown += " - " + TF("auto (by inheritance: {})", ci->reason);
        ImGui::SetNextItemWidth(-1);
        if (ui::BeginCombo("##iconcategory", shown.c_str()))
        {
            if (ci && ui::SelectableRaw(TF("auto (by inheritance: {}): {}", ci->reason, T(CategoryInfo(ci->category).name)).c_str(), !manual))
                ApplyCategory(ci->category, true);
            for (int i = 0; i < int(IconCategory::Count); ++i)
                if (ui::SelectableRaw(std::format("{}##iconcat{}", T(CategoryInfo(IconCategory(i)).name), i).c_str(), manual && cat_ == IconCategory(i)))
                    ApplyCategory(IconCategory(i), true);
            ImGui::EndCombo();
        }
        ui::TextDisabled("%s", TF("preset: {}", T(Standard(set_.preset).name)).c_str());
        ImGui::SameLine();
        if (ui::SmallButton("(?)")) showStandards_ = !showStandards_;
        if (ImGui::IsItemHovered())
        {
            const IconStandard& st = Standard(set_.preset);
            const IconNameParts n = IconNameOf(set_.preset, "<Class>");
            ui::SetTooltip("%s", TF("{}: {} x {}, {}, {}, border L{:.0f} R{:.0f} T{:.0f} B{:.0f} %, {} in {}", st.configKey, st.width, st.height,
                                    T(IconBackgroundText(set_.preset)), T(IconViewText(set_.preset)), st.marginL * 100, st.marginR * 100, st.marginT * 100,
                                    st.marginB * 100, n.file, st.folder).c_str());
        }
        ImGui::SameLine();
        if (ui::SmallButton("BI standards")) showStandards_ = !showStandards_;
    }
    DrawConfigPictures();
    EnterHiding(); // the icon's hiding while the window is shown
    if (ui::Button("Model parts...")) showParts = !showParts;
    if (ImGui::IsItemHovered()) ui::SetTooltip("Hide selections, proxies or LODs for this picture only (the outliner of the Model mode; H, Shift+H, Alt+H)");
    ImGui::SameLine();
    if (ui::Button("Reset hiding")) ResetHiding();
    // preview + BI reference
    ui::SeparatorText("Preview");
    const float boxW = std::max(120.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    auto box = [&](ID3D11ShaderResourceView* srv, int w, int h, const char* caption) {
        ImGui::BeginGroup();
        const float bh = boxW * float(std::max(1, h)) / float(std::max(1, w));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float c = 8 * dpi; // checker: the transparent parts
        for (float y = 0; y < bh; y += c)
            for (float x = 0; x < boxW; x += c)
                dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(boxW, x + c), p.y + std::min(bh, y + c)),
                                  (int(x / c) + int(y / c)) % 2 ? IM_COL32(90, 90, 90, 255) : IM_COL32(70, 70, 70, 255));
        if (srv) ImGui::Image(ImTextureID(srv), ImVec2(boxW, bh));
        else ImGui::Dummy(ImVec2(boxW, bh));
        if (srv == srv_ && ImGui::IsItemHovered())
        {
            // the picture's camera with the mouse: left drag turns, right / middle drag moves the target, wheel zooms
            const ImGuiIO& io = ImGui::GetIO();
            bool moved = false;
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
            {
                set_.yawDeg = std::remainder(set_.yawDeg + io.MouseDelta.x * 0.5f, 360.0f);
                set_.pitchDeg = std::clamp(set_.pitchDeg + io.MouseDelta.y * 0.5f, -89.5f, 89.5f);
                moved = true;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
            {
                const float k = r_.lastSize / std::max(1.0f, boxW) / std::max(0.05f, set_.zoom);
                const glm::vec3 d = (-r_.lastRight * io.MouseDelta.x + r_.lastUp * io.MouseDelta.y) * k;
                set_.offsetX += d.x; set_.offsetY += d.y; set_.offsetZ += d.z;
                set_.fitToBorder = false;
                moved = true;
            }
            if (io.MouseWheel != 0)
            {
                set_.zoom = std::clamp(set_.zoom * std::pow(1.1f, io.MouseWheel), 0.05f, 50.0f);
                set_.fitToBorder = false;
                moved = true;
            }
            if (moved) { dirty_ = true; SaveClass(); }
        }
        ImGui::TextDisabled("%s", caption);
        ImGui::EndGroup();
    };
    box(srv_, set_.width, set_.height, std::format("{} x {}", set_.width, set_.height).c_str());
    ImGui::SameLine();
    const IconStandard& st = Standard(set_.preset);
    const std::vector<IconExample>& examples = Examples(cat_);
    if (examples.empty())
    {
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boxW);
        ui::TextDisabled("%s", T("BI's pictures of this category are read from the game data on P: (unpacked by Arma 3 Tools); none found."));
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    }
    else
    {
        example_ = std::clamp(example_, 0, int(examples.size()) - 1);
        const IconExample& ex = examples[size_t(example_)];
        const TextureEntry& ref = tex_.Get(PathToUtf8(ex.file));
        ImGui::BeginGroup();
        box(ref.srv.Get(), ex.width ? ex.width : st.width, ex.height ? ex.height : st.height,
            std::format("BI {} / {}: {}", example_ + 1, examples.size(), ex.cls).c_str());
        if (ui::SmallButton("<##iconex")) example_ = (example_ + int(examples.size()) - 1) % int(examples.size());
        ImGui::SameLine();
        if (ui::SmallButton(">##iconex")) example_ = (example_ + 1) % int(examples.size());
        ImGui::SameLine();
        ui::TextDisabled("%s", std::format("{} x {}", ex.width, ex.height).c_str());
        ui::TextDisabled("%s", PathToUtf8(ex.file.filename()).c_str());
        if (!ex.config.empty() && ui::SmallButton("Open its config line")) host_.OpenInEditor(ex.config, ex.line);
        ImGui::EndGroup();
    }
    if (!msg_.empty()) ui::TextWrapped("%s", msg_.c_str());
    if (!picture_.Empty())
    {
        const ImageStats m = MeasureImage(picture_);
        ui::TextDisabled("fill %.0f x %.0f %%, border L %.0f R %.0f T %.0f B %.0f %%", m.fillW * 100, m.fillH * 100, m.marginL * 100, m.marginR * 100,
                         m.marginT * 100, m.marginB * 100);
    }
    // fitting
    ui::SeparatorText("Fitting");
    bool ch = false;
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Exposure (EV)", &set_.exposure, -2.0f, 2.0f, "%.1f");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Saturation", &set_.saturation, 0.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Contrast", &set_.contrast, 0.5f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderInt("Outline (px)", &set_.outline, 0, 8);
    if (set_.background == IconBackground::Transparent)
    {
        float m[4] = { set_.marginL * 100, set_.marginR * 100, set_.marginT * 100, set_.marginB * 100 };
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::DragFloat4("Border L R T B (%)", m, 0.2f, 0.0f, 40.0f, "%.1f"))
        {
            set_.marginL = m[0] / 100; set_.marginR = m[1] / 100; set_.marginT = m[2] / 100; set_.marginB = m[3] / 100;
            ch = true;
        }
        ch |= ui::Checkbox("White silhouette", &set_.whiteSilhouette);
    }
    ui::SeparatorText("Camera");
    ui::TextDisabled("In the preview: left drag turns, right / middle drag moves, wheel zooms (Ctrl+click a slider: type)");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Camera yaw", &set_.yawDeg, -180.0f, 180.0f, "%.1f deg");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Camera pitch", &set_.pitchDeg, -89.5f, 89.5f, "%.1f deg");
    ImGui::SetNextItemWidth(160 * dpi);
    ch |= ui::SliderFloat("Camera roll", &set_.rollDeg, -180.0f, 180.0f, "%.1f deg");
    {
        bool framed = false;
        ImGui::SetNextItemWidth(160 * dpi);
        framed |= ui::SliderFloat("Zoom", &set_.zoom, 0.2f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        float off[3] = { set_.offsetX, set_.offsetY, set_.offsetZ };
        const float range = std::max(0.1f, r_.lastSize);
        ImGui::SetNextItemWidth(220 * dpi);
        if (ui::SliderFloat3("Target X Y Z (m)", off, -range, range, "%.3f")) { set_.offsetX = off[0]; set_.offsetY = off[1]; set_.offsetZ = off[2]; framed = true; }
        if (framed) set_.fitToBorder = false;
        ch |= framed;
        if (set_.background == IconBackground::Transparent) ch |= ui::Checkbox("Fit into the BI border", &set_.fitToBorder);
    }
    if (set_.background == IconBackground::Transparent)
    {
        ImGui::SetNextItemWidth(160 * dpi);
        ch |= ui::SliderFloat("Field of view", &set_.fovDeg, 2.0f, 60.0f, "%.0f deg");
    }
    if (set_.preset == IconPreset::Custom)
    {
        int wh[2] = { set_.width, set_.height };
        ImGui::SetNextItemWidth(160 * dpi);
        if (ui::InputInt2("Size", wh)) { set_.width = std::clamp(wh[0], 16, 2048); set_.height = std::clamp(wh[1], 16, 2048); ch = true; }
    }
    if (ui::Button("Fit by BI")) { ApplyPreset(set_.preset); SaveClass(); }
    ImGui::SameLine();
    if (ui::Button("Reset camera")) ResetCamera();
    ImGui::SameLine();
    if (ui::Button("Refresh preview")) dirty_ = true;
    if (ch) SaveClass();
    dirty_ |= ch;
    // output
    ui::SeparatorText("Output");
    fs::path out = OutputFile();
    const bool jpg = set_.preset == IconPreset::EditorPreview;
    if (!jpg)
    {
        ui::Checkbox("PAA (ImageToPAA), else PNG", &paa_);
        if (!paa_) out.replace_extension(".png");
    }
    {
        const IconNameParts n = IconNameOf(set_.preset, name_);
        const std::string ext = jpg || paa_ ? n.ext : std::string(".png");
        ui::TextWrapped("%s", TF("File name: {}{}{}{}", n.prefix, n.stem, n.suffix, ext).c_str());
        ui::TextDisabled("%s", TF("= prefix \"{}\" + class \"{}\" + ending \"{}\" + \"{}\"", n.prefix, n.stem, n.suffix, ext).c_str());
        ImGui::PushTextWrapPos(0);
        ui::TextDisabled("%s", T(n.why.c_str()));
        ImGui::PopTextWrapPos();
        ui::TextWrapped("%s", TF("In the mod: {}", ArmaPathOf(out, s_.projectRoot)).c_str());
    }
    ui::TextWrapped("%s", PathToUtf8(out).c_str());
    char tool[400];
    strncpy_s(tool, imageToPaa.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(-90 * dpi);
    if (ui::InputText("##imagetopaa", tool, sizeof(tool))) imageToPaa = tool;
    ImGui::SameLine();
    if (ui::SmallButton("Find")) imageToPaa = PathToUtf8(FindImageToPaa());
    if (ImGui::IsItemHovered()) ui::SetTooltip("ImageToPAA.exe of the Arma 3 Tools (Steam libraries of this computer)");
    const std::string line = IconConfigLine(set_.preset, ArmaPathOf(out, s_.projectRoot));
    ui::TextWrapped("%s", line.c_str());
    if (ui::SmallButton("Copy config line")) ImGui::SetClipboardText(line.c_str());
    if (ui::Button("Save picture"))
    {
        std::string m;
        Save(m);
        msg_ = m;
    }
    // batch
    ui::SeparatorText("Batch (the mod's classes)");
    if (!batchListed_ || ui::SmallButton("List again"))
    {
        batch_ = ConfigClasses(false);
        batchListed_ = true;
    }
    ui::TextDisabled("%zu class(es) with a model on disk in the mod's config", batch_.size());
    if (ImGui::BeginChild("##iconbatch", ImVec2(0, 120 * dpi), ImGuiChildFlags_Borders))
    {
        for (auto& it : batch_)
            ImGui::Checkbox(std::format("{}::{} ({})##b{}", it.root, it.cls, it.preset < IconPreset::Count ? Standard(it.preset).id : "-", it.cls).c_str(), &it.selected);
    }
    ImGui::EndChild();
    ui::Checkbox("Editor previews (CfgVehicles)", &batchPreviews_);
    ImGui::SameLine();
    ui::Checkbox("Map icons (vehicles)", &batchMapIcons_);
    if (batchRunning_)
    {
        ImGui::ProgressBar(batch_.empty() ? 1.0f : float(batchNext_) / float(batch_.size()), ImVec2(200 * dpi, 0));
        ImGui::SameLine();
        if (ui::SmallButton("Stop##iconbatch")) batchRunning_ = false;
    }
    else if (ui::Button("Make all"))
    {
        std::erase_if(batch_, [](const ClassItem& i) { return !i.selected; });
        batchRunning_ = !batch_.empty();
        batchNext_ = 0;
        batchPhase_ = batchDone_ = batchFailed_ = 0;
    }
    ImGui::SameLine();
    if (ui::Button("SQF for the game"))
    {
        sqf_ = SqfForClasses(batch_);
        if (!sqf_.empty()) ImGui::SetClipboardText(sqf_.c_str());
    }
    if (ImGui::IsItemHovered())
        ui::SetTooltip("BIS_fnc_exportEditorPreviews for the selected CfgVehicles classes only (copied): run it in the game; the shots go to Screenshots\\EditorPreviews");
    if (!sqf_.empty()) ui::TextWrapped("%s", sqf_.c_str());
    ImGui::End();
}

void IconsPanel::DrawStandards()
{
    const float dpi = ImGui::GetFontSize() / 13.0f;
    ImGui::SetNextWindowSize(ImVec2(1100 * dpi, 420 * dpi), ImGuiCond_FirstUseEver);
    if (!ui::Begin(std::format("{}###bistandards", T("BI standards")).c_str(), &showStandards_)) { ImGui::End(); return; }
    ui::TextDisabled("%s", T("Medians of BI's own pictures of the category (docs/icons.md); the example is read from the game data on P: while the viewer runs"));
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##bistd", 10, flags))
    {
        for (const char* h : { "Category", "Config entry", "Size", "Background / alpha", "View", "Border (L R T B)", "Format", "File name", "Folder", "BI example" })
            ui::TableSetupColumn(h);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(IconCategory::Count); ++i)
        {
            const IconCategory c = IconCategory(i);
            const IconPreset p = CategoryInfo(c).preset;
            const IconStandard& st = Standard(p);
            const IconNameParts n = IconNameOf(p, "<Class>");
            ImGui::TableNextRow();
            if (c == cat_) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderActive));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(T(CategoryInfo(c).name));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p == IconPreset::VehicleMapIcon ? "icon" : st.configKey);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(std::format("{} x {}", st.width, st.height).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(T(IconBackgroundText(p)));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(T(IconViewText(p)));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(std::format("{:.0f} {:.0f} {:.0f} {:.0f} %", st.marginL * 100, st.marginR * 100, st.marginT * 100, st.marginB * 100).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.ext == ".jpg" ? "JPG" : "PAA (DXT5)");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.file.c_str());
            if (ImGui::IsItemHovered()) ui::SetTooltip("%s", T(n.why.c_str()));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(st.folder);
            ImGui::TableNextColumn();
            const auto& ex = Examples(c);
            if (!ex.empty())
            {
                const IconExample& e = ex.front();
                const std::string where = std::format("{}:{}", ArmaPathOf(e.config, s_.projectRoot), e.line);
                if (ui::SmallButtonRaw(std::format("{}##bistdex{}", where, i).c_str())) host_.OpenInEditor(e.config, e.line);
                if (ImGui::IsItemHovered()) ui::SetTooltip("%s", std::format("{}\n{} ({} x {})", e.cls, PathToUtf8(e.file), e.width, e.height).c_str());
            }
            else ui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void IconsPanel::DrawConfigPictures()
{
    const ArmaConfig* cfg = s_.modCfg ? s_.modCfg.get() : nullptr;
    if (!cfg || cls_ < 0 || cls_ >= int(classes_.size())) return;
    const ClassItem& ci = classes_[size_t(cls_)];
    const std::string key = std::format("{}|{}|{}", ci.cls, static_cast<const void*>(cfg), int(cat_));
    if (key != issuesKey_)
    {
        issuesKey_ = key;
        issues_ = CheckConfigPictures(*cfg, s_.projectRoot, ci.cls, [this](const std::string&) { return Category(); });
    }
    if (issues_.empty()) return;
    for (size_t i = 0; i < issues_.size(); ++i)
    {
        const IconPictureIssue& is = issues_[i];
        if (ui::SmallButton(TF("open##icpic{}", i).c_str())) host_.OpenInEditor(is.config, is.line);
        if (ImGui::IsItemHovered()) ui::SetTooltip("%s:%d", PathToUtf8(is.config).c_str(), is.line);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, is.error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ui::TextWrapped("%s", std::format("{} = \"{}\": {}", is.key, is.value, T(is.problem.c_str())).c_str());
        ImGui::PopStyleColor();
    }
}
