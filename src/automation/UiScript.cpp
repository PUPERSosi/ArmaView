#include "UiScript.h"
#include "Log.h"
#include "Commands.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

namespace {

ImGuiKey KeyFromName(std::string n)
{
    n = ToLower(n);
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return ImGuiKey(ImGuiKey_A + (n[0] - 'a'));
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return ImGuiKey(ImGuiKey_0 + (n[0] - '0'));
    if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1]))
    {
        int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return ImGuiKey(ImGuiKey_F1 + f - 1);
    }
    static const std::pair<const char*, ImGuiKey> named[] = {
        { "esc", ImGuiKey_Escape }, { "escape", ImGuiKey_Escape }, { "enter", ImGuiKey_Enter }, { "return", ImGuiKey_Enter },
        { "tab", ImGuiKey_Tab }, { "space", ImGuiKey_Space }, { "backspace", ImGuiKey_Backspace }, { "delete", ImGuiKey_Delete },
        { "del", ImGuiKey_Delete }, { "up", ImGuiKey_UpArrow }, { "down", ImGuiKey_DownArrow }, { "left", ImGuiKey_LeftArrow },
        { "right", ImGuiKey_RightArrow }, { "home", ImGuiKey_Home }, { "end", ImGuiKey_End }, { "pageup", ImGuiKey_PageUp },
        { "pagedown", ImGuiKey_PageDown }, { "comma", ImGuiKey_Comma }, { "period", ImGuiKey_Period },
    };
    for (const auto& [name, key] : named)
        if (n == name) return key;
    return ImGuiKey_None;
}

} // namespace

bool UiScript::Load(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
    {
        Log::Get().Error("ui-script: cannot open '{}'", PathToUtf8(file));
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    baseDir_ = file.parent_path();
    steps_.clear();
    next_ = 0;
    Parse(ss.str());
    active_ = !steps_.empty();
    Log::Get().Info("ui-script: '{}', {} step(s)", PathToUtf8(file), steps_.size());
    return active_;
}

bool UiScript::LoadText(const std::string& text, const fs::path& baseDir)
{
    baseDir_ = baseDir;
    steps_.clear();
    next_ = 0;
    Parse(text);
    active_ = !steps_.empty();
    Log::Get().Info("ui-script: built-in, {} step(s), output in '{}'", steps_.size(), PathToUtf8(baseDir_));
    return active_;
}

void UiScript::MouseButton(int button, bool down)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mx_, my_);
    io.AddMouseButtonEvent(button, down);
}

void UiScript::Parse(const std::string& text)
{
    auto& log = Log::Get();
    std::istringstream lines(text);
    std::string raw;
    int lineNo = 0;
    auto add = [&](std::function<void(const Host&)> fn) { steps_.push_back({ lineNo, std::move(fn) }); };
    auto noop = [](const Host&) {};
    while (std::getline(lines, raw))
    {
        ++lineNo;
        if (auto h = raw.find('#'); h != std::string::npos) raw.resize(h);
        std::istringstream in(raw);
        std::string cmd;
        if (!(in >> cmd)) continue;
        cmd = ToLower(cmd);
        std::string rest;
        std::getline(in >> std::ws, rest);
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
        std::istringstream args(rest);
        const int ln = lineNo;
        auto bad = [&]() { log.Error("ui-script line {}: cannot parse \"{}\"", ln, raw); };

        if (cmd == "wait")
        {
            int n = 0;
            if (!(args >> n) || n < 0) { bad(); continue; }
            for (int i = 0; i < n; ++i) add(noop);
        }
        else if (cmd == "move")
        {
            float x, y;
            if (!(args >> x >> y)) { bad(); continue; }
            add([this, x, y](const Host&) { mx_ = x; my_ = y; });
        }
        else if (cmd == "click")
        {
            std::string b = "left";
            args >> b;
            int button = ToLower(b) == "right" ? 1 : ToLower(b) == "middle" ? 2 : 0;
            add([this, button](const Host&) { MouseButton(button, true); });
            add([this, button](const Host&) { MouseButton(button, false); });
        }
        else if (cmd == "ctrlclick") // left click with Ctrl held (face pick)
        {
            add([this](const Host&) { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); MouseButton(0, true); });
            add([this](const Host&) { MouseButton(0, false); });
            add([](const Host&) { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); });
        }
        else if (cmd == "dblclick")
        {
            for (int k = 0; k < 2; ++k)
            {
                add([this](const Host&) { MouseButton(0, true); });
                add([this](const Host&) { MouseButton(0, false); });
            }
        }
        else if (cmd == "drag")
        {
            float x1, y1, x2, y2;
            if (!(args >> x1 >> y1 >> x2 >> y2)) { bad(); continue; }
            add([this, x1, y1](const Host&) { mx_ = x1; my_ = y1; });
            add([this](const Host&) { MouseButton(0, true); });
            const int steps = 12;
            for (int i = 1; i <= steps; ++i)
            {
                float t = float(i) / steps;
                add([this, x1, y1, x2, y2, t](const Host&) { mx_ = x1 + (x2 - x1) * t; my_ = y1 + (y2 - y1) * t; });
            }
            add([this](const Host&) { MouseButton(0, false); });
        }
        else if (cmd == "dragby") // drag from the current mouse position (e.g. after focus_node)
        {
            float dx, dy;
            if (!(args >> dx >> dy)) { bad(); continue; }
            auto start = std::make_shared<ImVec2>();
            add([this, start](const Host&) { *start = ImVec2(mx_, my_); MouseButton(0, true); });
            const int steps = 12;
            for (int i = 1; i <= steps; ++i)
            {
                float t = float(i) / steps;
                add([this, start, dx, dy, t](const Host&) { mx_ = start->x + dx * t; my_ = start->y + dy * t; });
            }
            add([this](const Host&) { MouseButton(0, false); });
        }
        else if (cmd == "type") // text into the focused input field
        {
            if (rest.empty()) { bad(); continue; }
            add([rest](const Host&) { ImGui::GetIO().AddInputCharactersUTF8(rest.c_str()); });
        }
        else if (cmd == "wheel")
        {
            float s = 0;
            if (!(args >> s)) { bad(); continue; }
            add([s](const Host&) { ImGui::GetIO().AddMouseWheelEvent(0.0f, s); });
        }
        else if (cmd == "hold" || cmd == "release") // key held across frames (control mode)
        {
            std::string spec;
            if (!(args >> spec)) { bad(); continue; }
            const bool down = cmd == "hold";
            const std::string k = ToLower(spec);
            ImGuiKey key = k == "shift" ? ImGuiMod_Shift : k == "ctrl" ? ImGuiMod_Ctrl : KeyFromName(k);
            if (key == ImGuiKey_None) { log.Error("ui-script line {}: unknown key \"{}\"", ln, spec); continue; }
            add([key, down](const Host&) { ImGui::GetIO().AddKeyEvent(key, down); });
        }
        else if (cmd == "layout" || cmd == "rawkey" || cmd == "rawtype" || cmd == "inputstate")
        {
            // raw Win32 keyboard messages (as the active keyboard layout delivers them), see App::ScriptRawInput
            const std::string arg = "raw:" + cmd + " " + rest;
            add([this, arg](const Host& h) { if (h.move) report_ += arg.substr(4) + ": " + h.move("x:" + arg, false) + "\n"; });
        }
        else if (cmd == "edit" || cmd == "moves_tests" || cmd == "moves_traps" || cmd == "control" || cmd == "layers" || cmd == "weapon" || cmd == "lang" ||
                 cmd == "tip" || cmd == "guide" || cmd == "modcheck" || cmd == "record" ||
                 cmd == "crashtest" || cmd == "unsaved" || cmd == "fuzz" || cmd == "coretest" || cmd == "bonecheck" || cmd == "blend" || cmd == "handanim" || cmd == "missing" || cmd == "statesgraph" || cmd == "idconflicts" ||
                 cmd == "mode" || cmd == "show" || cmd == "frame" || cmd == "focusbone" || cmd == "layouts" || cmd == "unit" || cmd == "gltf_sample" || cmd == "optstate" || cmd == "skintest")
        {
            // edit set <state> <entry> <value...> | edit link <from> <to> [connect] | edit new <name> [base] | edit setc <Cls/Sub> <entry> <value> | edit diff | edit save
            const std::string arg = cmd + ":" + rest;
            const bool loads = cmd == "unit" || (cmd == "gltf_sample" && rest.find("check") == std::string::npos); // may open the class's model: the next step waits for it
            add([this, arg, loads](const Host& h) {
                if (h.move) report_ += arg + ": " + h.move("x:" + arg, false) + "\n";
                if (loads) waitLoad_ = true;
            });
        }
        else if (cmd == "key")
        {
            std::string spec;
            if (!(args >> spec)) { bad(); continue; }
            std::vector<ImGuiKey> mods;
            std::string keyName = ToLower(spec);
            for (size_t plus; (plus = keyName.find('+')) != std::string::npos && plus + 1 < keyName.size(); keyName = keyName.substr(plus + 1))
            {
                std::string m = keyName.substr(0, plus);
                if (m == "ctrl") mods.push_back(ImGuiMod_Ctrl);
                else if (m == "shift") mods.push_back(ImGuiMod_Shift);
                else if (m == "alt") mods.push_back(ImGuiMod_Alt);
            }
            ImGuiKey key = KeyFromName(keyName);
            if (key == ImGuiKey_None) { log.Error("ui-script line {}: unknown key \"{}\"", ln, spec); continue; }
            add([mods, key](const Host&) {
                ImGuiIO& io = ImGui::GetIO();
                for (ImGuiKey m : mods) io.AddKeyEvent(m, true);
                io.AddKeyEvent(key, true);
            });
            add([mods, key](const Host&) {
                ImGuiIO& io = ImGui::GetIO();
                io.AddKeyEvent(key, false);
                for (ImGuiKey m : mods) io.AddKeyEvent(m, false);
            });
        }
        else if (cmd == "screenshot")
        {
            if (rest.empty()) { bad(); continue; }
            fs::path p = fs::path(Utf8ToWide(rest));
            if (p.is_relative()) p = baseDir_ / p;
            add([p](const Host& h) { if (h.screenshot) h.screenshot(p); });
        }
        else if (cmd == "focus_node" || cmd == "focus_point" || cmd == "hover")
        {
            if (rest.empty()) { bad(); continue; }
            bool node = cmd == "focus_node";
            const bool control = cmd == "hover";
            add([this, rest, node, control, ln](const Host& h) {
                float x = 0, y = 0;
                bool ok = control ? (h.findControl && h.findControl(rest, x, y))
                                  : node ? (h.findNode && h.findNode(rest, x, y)) : (h.findPoint && h.findPoint(rest, x, y));
                if (ok) { mx_ = x; my_ = y; Log::Get().Info("ui-script line {}: {} \"{}\" at ({:.0f}, {:.0f})", ln, node ? "node" : "point", rest, x, y); }
                else Log::Get().Error("ui-script line {}: {} \"{}\" not found on screen", ln, node ? "graph node" : "point", rest);
            });
        }
        else if (cmd == "focus_window")
        {
            if (rest.empty()) { bad(); continue; }
            add([this, rest](const Host&) { focusWindow_ = rest; });
        }
        else if (cmd == "wait_ms")
        {
            double ms = 0;
            if (!(args >> ms) || ms < 0) { bad(); continue; }
            add([this, ms](const Host&) { waitUntil_ = ImGui::GetTime() + ms / 1000.0; });
        }
        else if (cmd == "playmove" || cmd == "switchmove")
        {
            if (rest.empty()) { bad(); continue; }
            const bool sw = cmd == "switchmove";
            add([this, rest, sw](const Host& h) { if (h.move) report_ += std::string(sw ? "switchMove " : "playMove ") + rest + ": " + h.move(rest, sw) + "\n"; });
        }
        else if (cmd == "retarget" || cmd == "retarget_export" || cmd == "retarget_export_overwrite" || cmd == "retarget_footik")
        {
            // retarget_export_overwrite <file>: an existing file moves to <file>.bak (.bak2...), then the export
            const std::string arg = (cmd == "retarget" ? "rt:" : cmd == "retarget_footik" ? "rtfoot:" : cmd == "retarget_export" ? "rtexport:" : "rtexport!:") + rest;
            add([this, arg](const Host& h) { if (h.move) report_ += arg + ": " + h.move(arg, false) + "\n"; });
        }
        else if (cmd == "rvmat_survey") // scan, wait, table into the report
        {
            add([this](const Host& h) { if (h.move) { h.move("survey:start", false); waitReady_ = true; } });
            add([this](const Host& h) { if (h.move) report_ += "RVMAT survey:" + h.move("survey:report", false) + "\n"; });
        }
        else if (cmd == "playgesture" || cmd == "maskheat")
        {
            if (rest.empty()) { bad(); continue; }
            const std::string arg = (cmd == "playgesture" ? "gesture:" : "heat:") + rest;
            add([this, arg](const Host& h) { if (h.move) report_ += arg + ": " + h.move(arg, false) + "\n"; });
        }
        else if (cmd == "moves_check")
        {
            add([this](const Host& h) { if (h.movesCheck) { h.movesCheck(true); waitReady_ = true; } });
            add([this, rest](const Host& h) { if (h.movesCheck) report_ += "CfgMoves " + rest + ": " + h.movesCheck(false) + "\n"; });
        }
        else if (cmd == "open" || cmd == "open_async" || cmd == "state" || cmd == "validation" || cmd == "report" || cmd == "perf_end")
        {
            if (rest.empty()) { bad(); continue; }
            fs::path p = fs::path(Utf8ToWide(rest));
            if (p.is_relative() && cmd == "report") p = baseDir_ / p;
            const std::string what = cmd;
            add([this, what, rest, p](const Host& h) {
                if (what == "open" && h.open) { h.open(p); waitLoad_ = true; }
                else if (what == "open_async" && h.open) h.open(p); // the next steps run while it loads
                else if (what == "state" && h.state) h.state(rest);
                else if (what == "validation" && h.validation) report_ += h.validation(rest) + "\n";
                else if (what == "perf_end")
                {
                    perf_ = false;
                    double sum = 0, mx = 0;
                    for (double v : perfMs_) { sum += v; mx = std::max(mx, v); }
                    std::vector<double> s = perfMs_;
                    std::sort(s.begin(), s.end());
                    double p95 = s.empty() ? 0 : s[std::min(s.size() - 1, size_t(double(s.size()) * 0.95))];
                    report_ += std::format("perf {}: {} frames, CPU frame avg {:.2f} ms, p95 {:.2f} ms, max {:.2f} ms\n", rest, perfMs_.size(),
                                           perfMs_.empty() ? 0.0 : sum / double(perfMs_.size()), p95, mx);
                    if (h.perfFrames) report_ += "  parts: " + h.perfFrames() + "\n";
                }
                else if (what == "report")
                {
                    std::error_code ec;
                    fs::create_directories(p.parent_path(), ec);
                    std::ofstream f(p, std::ios::binary | std::ios::trunc);
                    f << report_;
                    Log::Get().Info("ui-script: report written to '{}'", PathToUtf8(p));
                }
            });
        }
        else if (cmd == "play" || cmd == "pause")
        {
            bool on = cmd == "play";
            add([on](const Host& h) { if (h.play) h.play(on); });
        }
        else if (cmd == "perf_begin")
            add([this](const Host& h) { perf_ = true; perfMs_.clear(); if (h.perfReset) h.perfReset(); });
        else if (cmd == "perf_notes")
        {
            std::string label = rest;
            add([this, label](const Host& h) { if (h.perfNotes) report_ += "load " + label + ": " + h.perfNotes() + "\n"; });
        }
        else if (cmd == "wait_config")
            add([this](const Host&) { waitReady_ = true; });
        else if (cmd == "exit")
            add([](const Host& h) { if (h.exit) h.exit(); });
        else
        {
            std::string known;
            for (const auto& c : UiScriptCommands()) known += c.name.substr(0, c.name.find(' ')) + " ";
            log.Error("ui-script line {}: unknown command \"{}\" (known: {})", ln, cmd, known);
        }
    }
}

void UiScript::BeforeNewFrame(const Host& host)
{
    if (!active_) return;
    // The platform backend queues the real cursor position every frame while the window is focused; drop it so
    // scripted wheel / button events are not processed at the real cursor position (trickled event queue).
    {
        auto& q = ImGui::GetCurrentContext()->InputEventsQueue;
        for (int i = q.Size - 1; i >= 0; --i)
            if (q[i].Type == ImGuiInputEventType_MousePos) q.erase(q.Data + i);
    }
    if (mx_ >= 0) ImGui::GetIO().AddMousePosEvent(mx_, my_);
    if (perf_ && host.frameMs) perfMs_.push_back(host.frameMs());
    if (waitUntil_ > 0.0)
    {
        if (ImGui::GetTime() < waitUntil_) return;
        waitUntil_ = 0.0;
    }
    if (waitLoad_)
    {
        if (host.loading && host.loading()) return;
        waitLoad_ = false;
    }
    if (waitReady_)
    {
        if (host.ready && !host.ready()) return;
        waitReady_ = false;
    }
    if (next_ < steps_.size())
    {
        const Step& s = steps_[next_++];
        s.run(host);
    }
    if (mx_ >= 0) ImGui::GetIO().AddMousePosEvent(mx_, my_); // overrides whatever the backend queued
    if (next_ >= steps_.size())
    {
        active_ = false;
        Log::Get().Info("ui-script: finished");
    }
}

void UiScript::AfterNewFrame()
{
    if (focusWindow_.empty()) return;
    // panels are "Title###Title_<mode>": match the visible title among the windows of the last frame
    std::string target = focusWindow_;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    {
        // the visible title (translated) or the id ("Moves" in "Движения###Moves_1")
        std::string name = w->Name, id;
        if (auto k = name.find("###"); k != std::string::npos)
        {
            id = name.substr(k + 3);
            name.resize(k);
            if (auto u = id.rfind('_'); u != std::string::npos && u + 1 < id.size() && std::isdigit(uint8_t(id[u + 1]))) id.resize(u);
        }
        if (w->WasActive && (name == focusWindow_ || id == focusWindow_)) { target = w->Name; break; }
    }
    ImGui::SetWindowFocus(target.c_str());
    focusWindow_.clear();
}
