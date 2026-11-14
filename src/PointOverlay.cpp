#include "PointOverlay.h"
#include "Lang.h"
#include "RvMaterial.h" // FindVehicleClass, NormalizeArmaPath
#include "Log.h"
#include "Material.h" // NormalizeArmaPath

#include <algorithm>
#include <format>
#include <set>

// ============================================================================================ config

namespace {

// Child classes of `cls` merged along its inheritance chain: the most derived definition of each name wins.
// An external declaration ("class HitHull;") stands for its resolved base when that is known.
struct Child { const CfgClass* cls; bool unresolved; };
std::vector<Child> MergedChildren(const CfgClass* cls)
{
    std::vector<Child> out;
    std::set<std::string> seen;
    for (const CfgClass* c : cls->Chain())
        for (const auto& k : c->classes)
        {
            if (!seen.insert(ToLower(k->name)).second) continue;
            out.push_back({ k->external && k->base ? k->base : k.get(), k->external && !k->base });
        }
    return out;
}

void CollectHitPoints(const ArmaConfig& cfg, const CfgClass& owner, const std::string& turret, std::vector<HitPointDef>& out)
{
    if (const CfgClass* hp = owner.FindClass("HitPoints"))
        for (const auto& [h, unresolved] : MergedChildren(hp))
        {
            HitPointDef d;
            d.unresolved = unresolved;
            d.cls = h->name;
            d.turret = turret;
            d.selection = h->GetString("name");
            d.radius = h->HasNumber("radius") ? float(h->GetNumber("radius")) : -1.0f;
            d.armor = h->HasNumber("armor") ? float(h->GetNumber("armor")) : -1.0f;
            d.where = cfg.Where(h->file, h->line);
            out.push_back(std::move(d));
        }
    if (const CfgClass* tu = owner.FindClass("Turrets"))
        for (const auto& [t, unresolved] : MergedChildren(tu))
            CollectHitPoints(cfg, *t, turret.empty() ? t->name : turret + "/" + t->name, out);
}

} // namespace

std::vector<HitPointDef> FindHitPoints(const ArmaConfig& cfg, const std::string& modelPath, std::string& vehicleClass)
{
    vehicleClass.clear();
    const CfgClass* found = FindVehicleClass(cfg, modelPath);
    std::vector<HitPointDef> out;
    if (!found) return out;
    vehicleClass = found->name;
    CollectHitPoints(cfg, *found, "", out);
    return out;
}

// ============================================================================================ overlay

namespace {

int FindHitLod(const P3DModel& m)
{
    for (int i = 0; i < int(m.lods.size()); ++i)
        if (LodResolutionName(m.lods[i].resolution) == "Hit-points") return i;
    return -1;
}

ImU32 HueColor(size_t i, int alpha)
{
    float h = std::fmod(float(i) * 0.61803f, 1.0f), r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.65f, 1.0f, r, g, b);
    return IM_COL32(int(r * 255), int(g * 255), int(b * 255), alpha);
}

} // namespace

void PointOverlay::SetHitPoints(std::vector<HitPointDef> defs, const std::string& vehicleClass, const P3DModel& model)
{
    auto& log = Log::Get();
    hit_ = std::move(defs);
    vehicleClass_ = vehicleClass;
    hitLod_ = FindHitLod(model);
    hitPoints_.assign(hit_.size(), {});
    pickedHit_ = -1;
    if (vehicleClass_.empty()) return;
    log.Info("Hitpoints: CfgVehicles::{} - {} hitpoint class(es), Hit-points LOD {}", vehicleClass_, hit_.size(),
             hitLod_ >= 0 ? std::to_string(hitLod_) : "missing");
    if (hitLod_ < 0) return;
    const P3DLod& l = model.lods[hitLod_];
    for (size_t i = 0; i < hit_.size(); ++i)
    {
        const HitPointDef& d = hit_[i];
        const P3DSelection* sel = nullptr;
        for (const auto& s : l.selections)
            if (IEquals(s.name, d.selection)) sel = &s;
        std::string owner = d.turret.empty() ? d.cls : d.turret + "::" + d.cls;
        if (d.unresolved)
            log.Info("  {} ({}): defined only in game configs that are not on disk - name/radius unknown", owner, d.where);
        else if (d.selection.empty())
            log.Warn("  {} ({}): no name= - not linked to the Hit-points LOD", owner, d.where);
        else if (!sel && IEquals(d.selection, "NEtelo")) // BI placeholder ("not hull"): a hitpoint without points
            log.Info("  {} ({}): name=\"NEtelo\" - BI placeholder without points (damage only through other hitpoints)", owner, d.where);
        else if (!sel)
            log.Warn("  {} ({}): name=\"{}\" is not a selection of the Hit-points LOD", owner, d.where, d.selection);
        else
            for (size_t p = 0; p < sel->pointBytes.size(); ++p)
                if (sel->pointBytes[p]) hitPoints_[i].push_back(int(p));
        if (sel && d.radius < 0) log.Warn("  {} ({}): radius not set in the configs on disk", owner, d.where);
    }
    // Hit-points LOD selections without a HitPoints class: shown as plain named points
    for (const auto& s : l.selections)
    {
        bool used = false;
        for (const auto& d : hit_) used |= IEquals(d.selection, s.name);
        if (used || s.pointCount == 0) continue;
        HitPointDef d;
        d.selection = s.name;
        d.unlinked = true;
        hit_.push_back(d);
        hitPoints_.emplace_back();
        for (size_t p = 0; p < s.pointBytes.size(); ++p)
            if (s.pointBytes[p]) hitPoints_.back().push_back(int(p));
        log.Info("  Hit-points LOD selection \"{}\" ({} pt): no HitPoints class uses it", s.name, s.pointCount);
    }
}

void PointOverlay::EnsureNames(const P3DLod& lod)
{
    if (cacheLod_ == &lod && cachePoints_ == lod.points.size()) return;
    cacheLod_ = &lod;
    cachePoints_ = lod.points.size();
    pointSel_.assign(lod.points.size(), {});
    names_.assign(lod.points.size(), {});
    for (int s = 0; s < int(lod.selections.size()); ++s)
    {
        const P3DSelection& sel = lod.selections[s];
        if (sel.name.starts_with("proxy:")) continue;
        for (size_t p = 0; p < sel.pointBytes.size() && p < lod.points.size(); ++p)
            if (sel.pointBytes[p]) pointSel_[p].push_back(s);
    }
    for (size_t p = 0; p < pointSel_.size(); ++p)
    {
        const auto& v = pointSel_[p];
        for (size_t k = 0; k < v.size() && k < 3; ++k) names_[p] += (k ? ", " : "") + lod.selections[v[k]].name;
        if (v.size() > 3) names_[p] += std::format(" +{}", v.size() - 3);
    }
}

void PointOverlay::SetSettings(const PointOverlaySettings& s)
{
    s_ = s;
    strncpy_s(filterEdit_, s_.filter.c_str(), _TRUNCATE);
}

void PointOverlay::DrawControls(const P3DLod* lod)
{
    // the widgets edit a copy that goes back through SetSettings (the same way as any other UI)
    PointOverlaySettings e = s_;
    if (e.filter != filterEdit_) strncpy_s(filterEdit_, e.filter.c_str(), _TRUNCATE);
    ImGui::Checkbox("Point labels", &e.labels);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows the names of the points (named selections) in the view.");
    ImGui::SameLine();
    ImGui::Checkbox("on mesh LODs", &e.onMeshLods);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Off: labels only on LODs without faces (Memory, Land Contact, Hit-points ...)");
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputTextWithHint("Label filter", "name contains...", filterEdit_, sizeof(filterEdit_))) e.filter = filterEdit_;
    if (ImGui::IsItemHovered()) ui::SetTooltip("Shows only the labels whose name contains this text.");
    ImGui::Checkbox("Occlude by mesh", &e.occludeByMesh);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Points behind the geometry (depth buffer) are dimmed or hidden");
    if (e.occludeByMesh)
    {
        ImGui::SameLine();
        ImGui::Checkbox("hide", &e.occludedHide);
        if (ImGui::IsItemHovered()) ui::SetTooltip("Hides the points behind the geometry instead of dimming them.");
    }
    ImGui::Checkbox("Unnamed points", &e.unnamedDots);
    if (ImGui::IsItemHovered()) ui::SetTooltip("Also shows the points that are in no named selection, as dots.");
    ImGui::SameLine();
    ImGui::Checkbox("Hitpoints view", &e.hitpointsView);
    if (ImGui::IsItemHovered())
        ui::SetTooltip("Spheres of CfgVehicles >> HitPoints (incl. turrets): radius around every point of name= in the Hit-points LOD");
    if (!(e == s_)) SetSettings(e);
    if (s_.hitpointsView)
    {
        if (vehicleClass_.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "No CfgVehicles class with this model in config.cpp");
        else if (hitLod_ < 0) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Model has no Hit-points LOD");
        else ImGui::TextDisabled("CfgVehicles::%s, %zu hitpoints", vehicleClass_.c_str(), hit_.size());
    }
    if (s_.labels && (lod || s_.hitpointsView)) ImGui::TextDisabled("labels %d shown, %d hidden (overlap: zoom in)", labelsShown_, labelsHidden_);
    ImGui::TextDisabled("Click a point in the scene: name and coordinates");
}

void PointOverlay::Draw(ImDrawList* dl, const View& v, const P3DModel& model, int lodIndex, const std::vector<glm::mat4>& pointMats)
{
    drawn_.clear();
    labelsShown_ = labelsHidden_ = 0;
    if (lodIndex < 0 || lodIndex >= int(model.lods.size())) return;
    const P3DLod& lod = model.lods[lodIndex];
    const bool posed = pointMats.size() == lod.points.size();
    auto pos = [&](int p) {
        glm::vec3 x = lod.points[p].pos;
        return posed ? glm::vec3(pointMats[p] * glm::vec4(x, 1.0f)) : x;
    };
    float lastZ = 0.0f; // NDC depth of the last projected point
    auto project = [&](const glm::vec3& p, ImVec2& s, float& depth) {
        glm::vec4 c = v.viewProj * glm::vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        s = ImVec2(v.x + (c.x / c.w * 0.5f + 0.5f) * v.w, v.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * v.h);
        depth = c.w;
        lastZ = c.z / c.w;
        return s.x >= v.x && s.y >= v.y && s.x < v.x + v.w && s.y < v.y + v.h;
    };
    // behind the mesh: everything in a 3x3 pixel block of the depth buffer is nearer (small bias)
    const bool occl = s_.occludeByMesh && v.depthAt;
    auto occluded = [&](ImVec2 s) {
        if (!occl) return false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (v.depthAt(s.x + dx, s.y + dy) >= lastZ - 2e-5f) return false;
        return true;
    };
    auto dim = [](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(70) << 24); };

    struct Label { ImVec2 at; std::string text; ImU32 col; float priority; float dotR; };
    std::vector<Label> labels;
    const std::string filter = ToLower(s_.filter);
    const float dot = 2.5f * v.dpi;

    // ---- named points of the current LOD
    const bool faceless = lod.faces.empty() || LodResolutionName(lod.resolution) == "Hit-points";
    if (s_.labels && (faceless || s_.onMeshLods))
    {
        EnsureNames(lod);
        for (int p = 0; p < int(lod.points.size()); ++p)
        {
            bool named = !pointSel_[p].empty();
            if (!named && !s_.unnamedDots) continue;
            bool match = true;
            if (!filter.empty())
            {
                match = false;
                for (int s : pointSel_[p]) match |= ToLower(lod.selections[s].name).find(filter) != std::string::npos;
                if (!match) continue;
            }
            ImVec2 s;
            float depth;
            if (!project(pos(p), s, depth)) continue;
            bool sel = p == picked_ && pickedLod_ == lodIndex;
            bool hidden = !sel && occluded(s);
            if (hidden && s_.occludedHide) continue;
            ImU32 col = sel ? IM_COL32(255, 80, 255, 255) : named ? IM_COL32(255, 230, 90, 255) : IM_COL32(170, 170, 170, 200);
            if (hidden) col = dim(col);
            dl->AddCircleFilled(s, sel ? dot * 1.8f : dot, col);
            drawn_.push_back({ p, -1, s, 0 });
            if (named) labels.push_back({ s, names_[p], col, sel ? -1e9f : depth + (hidden ? 1e6f : 0.0f), dot });
        }
    }

    // ---- hitpoint spheres (from the Hit-points LOD, drawn over any LOD)
    if (s_.hitpointsView && hitLod_ >= 0 && hitLod_ < int(model.lods.size()))
    {
        const P3DLod& hl = model.lods[hitLod_];
        const bool hitPosed = hitLod_ == lodIndex && posed;
        for (size_t i = 0; i < hit_.size() && i < hitPoints_.size(); ++i)
        {
            const HitPointDef& d = hit_[i];
            if (!filter.empty() && ToLower(d.cls).find(filter) == std::string::npos && ToLower(d.selection).find(filter) == std::string::npos) continue;
            const float r = d.radius > 0 ? d.radius : 0.0f;
            bool sel = int(i) == pickedHit_;
            ImU32 line = sel ? IM_COL32(255, 80, 255, 255) : d.unlinked ? IM_COL32(170, 170, 170, 220) : HueColor(i, 230), fill = sel ? IM_COL32(255, 80, 255, 50) : HueColor(i, 28);
            ImVec2 best;
            float bestY = 1e30f, bestDepth = 0;
            int n = 0;
            for (int p : hitPoints_[i])
            {
                if (p >= int(hl.points.size())) continue;
                glm::vec3 w = hitPosed ? pos(p) : hl.points[p].pos;
                ImVec2 s, e;
                float depth, d2;
                if (!project(w, s, depth)) continue;
                const bool hidden = !sel && occluded(s);
                if (hidden && s_.occludedHide) continue;
                float rs = 0;
                if (r > 0)
                {
                    e = s;
                    project(w + v.cameraRight * r, e, d2); // the edge may be off screen
                    rs = std::hypot(e.x - s.x, e.y - s.y);
                }
                if (rs > 0)
                {
                    dl->AddCircleFilled(s, rs, hidden ? (fill & 0x00FFFFFF) | (ImU32(10) << 24) : fill, 32);
                    dl->AddCircle(s, rs, hidden ? dim(line) : line, 32, sel ? 2.0f : 1.0f);
                }
                dl->AddCircleFilled(s, dot, hidden ? dim(line) : line);
                drawn_.push_back({ p, int(i), s, rs });
                if (s.y - rs < bestY) { bestY = s.y - rs; best = ImVec2(s.x, s.y - rs); bestDepth = depth; }
                ++n;
            }
            if (n)
            {
                std::string text = d.unlinked ? std::format("{} ({} pt)\nno HitPoints class uses it", d.selection, n) : d.cls + (d.radius > 0 ? std::format("  r={:g}", d.radius) : "  r=?") +
                                   (d.armor >= 0 ? std::format("  armor {:g}", d.armor) : "") +
                                   std::format("\n{} ({} pt){}", d.selection, n, d.turret.empty() ? "" : "  [" + d.turret + "]");
                labels.push_back({ best, text, line, sel ? -1e9f : bestDepth - 1000.0f, 0 }); // hitpoint names before point names
            }
        }
    }

    // ---- labels: nearest first, four candidate places each, skipped when every place overlaps a placed label
    std::stable_sort(labels.begin(), labels.end(), [](const Label& a, const Label& b) { return a.priority < b.priority; });
    std::vector<ImVec4> placed;
    const float pad = 3.0f * v.dpi;
    for (const Label& l : labels)
    {
        ImVec2 ts = ImGui::CalcTextSize(l.text.c_str());
        ImVec2 sz(ts.x + pad * 2, ts.y + pad);
        const float g = l.dotR + 4.0f * v.dpi;
        ImVec2 cand[4] = { { l.at.x + g, l.at.y - sz.y * 0.5f }, { l.at.x - g - sz.x, l.at.y - sz.y * 0.5f },
                           { l.at.x - sz.x * 0.5f, l.at.y - g - sz.y }, { l.at.x - sz.x * 0.5f, l.at.y + g } };
        bool done = false;
        for (const ImVec2& c : cand)
        {
            ImVec4 r(c.x, c.y, c.x + sz.x, c.y + sz.y);
            if (r.x < v.x || r.y < v.y || r.z > v.x + v.w || r.w > v.y + v.h) continue;
            bool overlap = false;
            for (const ImVec4& q : placed)
                if (r.x < q.z && r.z > q.x && r.y < q.w && r.w > q.y) { overlap = true; break; }
            if (overlap) continue;
            placed.push_back(r);
            dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32(15, 15, 18, 190), 3.0f);
            dl->AddText(ImVec2(r.x + pad, r.y + pad * 0.5f), l.col, l.text.c_str());
            done = true;
            break;
        }
        (done ? labelsShown_ : labelsHidden_)++;
    }

    // ---- picked point info (top-left of the scene)
    if (picked_ >= 0 || pickedHit_ >= 0)
    {
        std::string info = Info(model, lodIndex);
        ImVec2 at(v.x + 10 * v.dpi, v.y + 10 * v.dpi), ts = ImGui::CalcTextSize(info.c_str());
        dl->AddRectFilled(ImVec2(at.x - 6, at.y - 4), ImVec2(at.x + ts.x + 6, at.y + ts.y + 4), IM_COL32(15, 15, 18, 220), 4.0f);
        dl->AddText(at, IM_COL32(255, 160, 255, 255), info.c_str());
    }
}

std::string PointOverlay::Info(const P3DModel& model, int lodIndex) const
{
    std::string s;
    int lodOfPoint = pickedHit_ >= 0 ? hitLod_ : pickedLod_;
    if (lodOfPoint < 0 || lodOfPoint >= int(model.lods.size()) || picked_ < 0) return s;
    const P3DLod& l = model.lods[lodOfPoint];
    if (picked_ >= int(l.points.size())) return s;
    const glm::vec3 p = l.points[picked_].pos;
    s = std::format("Point #{}  LOD {} ({})\nX {:.4f}  Y {:.4f}  Z {:.4f}  (P3D model space)", picked_, lodOfPoint,
                    LodResolutionName(l.resolution), p.x, p.y, p.z);
    std::string names;
    for (const auto& sel : l.selections)
        if (!sel.name.starts_with("proxy:") && picked_ < int(sel.pointBytes.size()) && sel.pointBytes[picked_])
            names += (names.empty() ? "" : ", ") + sel.name;
    s += "\nselections: " + (names.empty() ? std::string("(none)") : names);
    if (pickedHit_ >= 0 && pickedHit_ < int(hit_.size()))
    {
        const HitPointDef& d = hit_[pickedHit_];
        s += std::format("\nHitPoints::{}{}  radius {}  armor {}\n{}", d.cls, d.turret.empty() ? "" : "  (turret " + d.turret + ")",
                         d.radius > 0 ? std::format("{:g}", d.radius) : "?", d.armor >= 0 ? std::format("{:g}", d.armor) : "?", d.where);
    }
    (void)lodIndex;
    return s;
}

bool PointOverlay::Click(float x, float y, const P3DModel& model, int lodIndex)
{
    // nearest dot within 10 px; otherwise the smallest hit sphere containing the cursor
    int best = -1;
    float bestD = 10.0f * 10.0f;
    for (int i = 0; i < int(drawn_.size()); ++i)
    {
        float dx = drawn_[i].s.x - x, dy = drawn_[i].s.y - y, d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0)
    {
        float bestR = 1e30f;
        for (int i = 0; i < int(drawn_.size()); ++i)
        {
            const Drawn& d = drawn_[i];
            if (d.hit >= 0 && d.r > 0 && std::hypot(d.s.x - x, d.s.y - y) <= d.r && d.r < bestR) { bestR = d.r; best = i; }
        }
    }
    if (best < 0)
    {
        bool had = picked_ >= 0 || pickedHit_ >= 0;
        picked_ = pickedHit_ = -1;
        return had;
    }
    const Drawn& d = drawn_[best];
    picked_ = d.point;
    pickedHit_ = d.hit;
    pickedLod_ = d.hit >= 0 ? hitLod_ : lodIndex;
    const P3DLod& l = model.lods[pickedLod_];
    pickedPos_ = picked_ < int(l.points.size()) ? l.points[picked_].pos : glm::vec3(0);
    std::string info = Info(model, lodIndex);
    for (char& c : info)
        if (c == '\n') c = ';';
    Log::Get().Info("Picked: {}", info);
    return true;
}

bool PointOverlay::PointScreenPos(const std::string& name, float& x, float& y) const
{
    const std::string q = ToLower(name);
    const Drawn* sub = nullptr;
    for (const Drawn& d : drawn_)
    {
        std::string n;
        if (d.hit >= 0 && d.hit < int(hit_.size())) n = ToLower(hit_[d.hit].cls + " " + hit_[d.hit].selection);
        else if (d.point >= 0 && d.point < int(names_.size())) n = ToLower(names_[d.point]);
        if (n == q) { x = d.s.x; y = d.s.y; return true; }
        if (!sub && !q.empty() && n.find(q) != std::string::npos) sub = &d;
    }
    if (!sub) return false;
    x = sub->s.x;
    y = sub->s.y;
    return true;
}
