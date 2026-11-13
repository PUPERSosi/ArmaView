// ArmaView entry point: Win32 window + message loop. Usage: ArmaView.exe [file.p3d|file.rtm ...] [--options]
// Single instance: a second "ArmaView.exe <path>" hands its arguments to the running window through the named pipe
// \\.\pipe\ArmaView.Open and exits (skipped for --screenshot and --new-instance).
#include "WinPlatform.h"
#include "App.h"
#include "Input.h"
#include "Commands.h"
#include "Log.h"
#include "Version.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <objbase.h>
#include <dbghelp.h>
#include <shellapi.h>

#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ArmaView.Open";
constexpr UINT WM_APP_OPEN = WM_APP + 1;

App* g_app = nullptr;

// Hang watchdog (automation runs): if no frame completes for 10 s, log the UI thread's call stack (dbghelp + PDB).
std::atomic<uint64_t> g_frames{ 0 };
void Watchdog(HANDLE uiThread)
{
    uint64_t last = 0;
    int still = 0;
    for (;;)
    {
        Sleep(1000);
        uint64_t f = g_frames.load();
        still = f == last ? still + 1 : 0;
        last = f;
        if (still != 10) continue;
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);
        SuspendThread(uiThread);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(uiThread, &ctx);
        STACKFRAME64 sf{};
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode = AddrModeFlat;
        std::string out = "Watchdog: no frame for 10 s - UI thread stack:";
        for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, uiThread, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i)
        {
            char buf[sizeof(SYMBOL_INFO) + 512] = {};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            std::string name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
            IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
            DWORD d2 = 0;
            std::string where = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &d2, &line) ? std::format(" {}:{}", line.FileName, line.LineNumber) : "";
            out += std::format("\n  {}{}", name, where);
        }
        ResumeThread(uiThread);
        Log::Get().Error("{}", out);
    }
}
// ---- crash reports: %LOCALAPPDATA%\ArmaView\crashes\<time>.dmp (minidump for the debugger), .txt (exception, stack
// with the PDB's file:line, version) and .log (a copy of the log up to the crash)
std::wstring g_logFile;
bool g_automation = false;

void WriteCrashReport(EXCEPTION_POINTERS* ep, const char* what)
{
    static std::atomic<bool> once{ false };
    if (once.exchange(true)) return;
    wchar_t dirBuf[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\ArmaView\\crashes", dirBuf, MAX_PATH);
    const std::filesystem::path dir = dirBuf;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring stem = std::format(L"crash_{:04}{:02}{:02}_{:02}{:02}{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE proc = GetCurrentProcess();
    // minidump
    HANDLE f = CreateFileW((dir / (stem + L".dmp")).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
        MiniDumpWriteDump(proc, GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }
    // text: what, where, stack
    std::string out = std::format("ArmaView {} (commit {}) crashed: {}\n", AV_VERSION_STRING, AV_GIT_COMMIT, what);
    CONTEXT ctx{};
    if (ep && ep->ExceptionRecord)
    {
        out += std::format("exception 0x{:08X} at 0x{:016X}\n", ep->ExceptionRecord->ExceptionCode, uint64_t(ep->ExceptionRecord->ExceptionAddress));
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            out += std::format("access violation: {} 0x{:016X}\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from", ep->ExceptionRecord->ExceptionInformation[1]);
        ctx = *ep->ContextRecord;
    }
    else
        RtlCaptureContext(&ctx);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    out += "stack:\n";
    for (int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i)
    {
        char buf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
        DWORD d2 = 0;
        std::string where = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &d2, &line) ? std::format(" {}:{}", line.FileName, line.LineNumber) : "";
        out += std::format("  {}{}\n", name, where);
    }
    std::ofstream((dir / (stem + L".txt"))) << out;
    if (!g_logFile.empty()) CopyFileW(g_logFile.c_str(), (dir / (stem + L".log")).c_str(), FALSE);
    if (!g_automation)
    {
        const std::wstring msg = std::format(L"ArmaView crashed.\n\nA report (dump, stack, log) was written to\n{}\\{}.*", dir.native(), stem);
        MessageBoxW(nullptr, msg.c_str(), L"ArmaView", MB_OK | MB_ICONERROR);
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    WriteCrashReport(ep, "unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER; // ends the process
}

void InstallCrashHandlers()
{
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] { WriteCrashReport(nullptr, "std::terminate (uncaught C++ exception)"); std::_Exit(3); });
    std::signal(SIGABRT, [](int) { WriteCrashReport(nullptr, "abort()"); std::_Exit(3); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { WriteCrashReport(nullptr, "invalid CRT parameter"); std::_Exit(3); });
    _set_purecall_handler([] { WriteCrashReport(nullptr, "pure virtual call"); std::_Exit(3); });
}

std::mutex g_pipeMutex;
std::vector<std::vector<std::wstring>> g_pipeQueue; // argument lists received from other instances

// Opens files and applies options; used for the own command line and for forwarded ones.
void HandleArgs(App& app, const std::vector<std::wstring>& args)
{
    Automation a;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring& arg = args[i];
        const CliOption* opt = nullptr;
        for (const auto& o : CliOptions())
            if (arg == o.name) opt = &o;
        if (!opt)
        {
            if (arg.starts_with(L"--")) Log::Get().Warn("Unknown option {} (F1: list of options)", WideToUtf8(arg));
            else app.OpenFile(arg);
            continue;
        }
        std::wstring value;
        const bool optional = opt->arg && opt->arg[0] == '[';
        if (opt->arg && i + 1 < args.size() && !(optional && args[i + 1].starts_with(L"--"))) value = args[++i];
        opt->apply(a, value);
    }
    app.ApplyAutomation(a);
}

// Client side: send our arguments (paths made absolute) to a running instance. True = handed over.
bool ForwardToRunningInstance(const std::vector<std::wstring>& args)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2 && pipe == INVALID_HANDLE_VALUE; ++attempt)
    {
        pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() != ERROR_PIPE_BUSY) return false; // no running instance
            WaitNamedPipeW(kPipeName, 2000);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    std::wstring msg;
    for (const auto& a : args)
    {
        // Only existing files become absolute paths (the other instance has another working directory);
        // option values such as "--focus-bone leftarm" or "--time 0.25" are passed as they are.
        std::wstring v = a;
        std::error_code ec;
        if (!a.starts_with(L"--") && std::filesystem::exists(a, ec))
        {
            auto abs = std::filesystem::absolute(a, ec);
            if (!ec) v = abs.native();
        }
        msg += v + L'\n';
    }
    AllowSetForegroundWindow(ASFW_ANY); // let the running window come to the front
    DWORD written = 0;
    BOOL ok = WriteFile(pipe, msg.data(), DWORD(msg.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(pipe);
    return ok;
}

// Server side: accepts one client at a time, queues its argument list and wakes the UI thread.
void PipeServer(HWND hwnd)
{
    for (;;)
    {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, 0, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
        {
            std::wstring data;
            wchar_t buf[4096];
            DWORD read = 0;
            while (ReadFile(pipe, buf, sizeof(buf), &read, nullptr) && read) data.append(buf, read / sizeof(wchar_t));
            std::vector<std::wstring> args;
            size_t a = 0;
            for (size_t b; (b = data.find(L'\n', a)) != std::wstring::npos; a = b + 1)
                if (b > a) args.push_back(data.substr(a, b - a));
            if (!args.empty())
            {
                {
                    std::lock_guard lock(g_pipeMutex);
                    g_pipeQueue.push_back(std::move(args));
                }
                PostMessageW(hwnd, WM_APP_OPEN, 0, 0);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

bool IsUserInput(UINT msg)
{
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_MOUSELEAVE ||
           msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_SETCURSOR;
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ui-script raw input (Win32 key messages as the active layout would deliver them) is marked with bit 27 of lParam
    const bool scripted = (lParam & LPARAM(kScriptedKeyBit)) != 0 && msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
    if (scripted) lParam &= ~LPARAM(kScriptedKeyBit);
    if (g_app && g_app->ScriptActive() && IsUserInput(msg) && !scripted) return DefWindowProcW(hwnd, msg, wParam, lParam); // --ui-script owns the input
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && !(HIWORD(lParam) & KF_EXTENDED))
        if (const unsigned vk = PhysicalVk(LOBYTE(HIWORD(lParam)))) wParam = WPARAM(vk);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (g_app && wParam != SIZE_MINIMIZED) g_app->OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && g_app; ++i)
        {
            wchar_t path[MAX_PATH];
            if (DragQueryFileW(drop, i, path, MAX_PATH)) g_app->OpenFile(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_APP_OPEN:
    {
        std::vector<std::vector<std::wstring>> queue;
        {
            std::lock_guard lock(g_pipeMutex);
            queue.swap(g_pipeQueue);
        }
        if (g_app)
            for (const auto& args : queue)
            {
                Log::Get().Info("Opened from another ArmaView.exe launch ({} argument(s))", args.size());
                HandleArgs(*g_app, args);
            }
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; // no Alt menu beep
        break;
    case WM_CLOSE:
        if (g_app && !g_app->AllowClose()) return 0; // unsaved edits: the app asks first
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd)
{
    InstallWinPlatform(); // the core reads P3D / config strings in the system ANSI code page through this
    InstallCrashHandlers();
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_logFile = (std::filesystem::path(exe).parent_path() / L"ArmaView.log").native(); // copied into a crash report
    }
    std::vector<std::wstring> args;
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
            LocalFree(argv);
        }
    }
    bool separate = false;
    for (const auto& a : args) separate |= a == L"--screenshot" || a == L"--new-instance" || a == L"--ui-script" || a == L"--selftest";
    if (!separate && ForwardToRunningInstance(args)) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // folder picker, WIC
    ImGui_ImplWin32_EnableDpiAwareness();
    float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // res/armaview.ico (src/ArmaView.rc)
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ArmaViewWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ArmaView", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              int(1600 * dpi), int(900 * dpi), nullptr, nullptr, instance, nullptr);
    DragAcceptFiles(hwnd, TRUE);

    App app;
    if (!app.Init(hwnd, dpi))
    {
        DestroyWindow(hwnd);
        return 1;
    }
    g_app = &app;
    g_logFile = app.LogFile().native(); // the log App writes (settings folder, or next to the exe when portable / testing)
    app.busyTick = [] { ++g_frames; }; // long commands on the UI thread (fuzz): not a hang
    bool automation = false;
    for (const auto& a : args) automation |= a == L"--ui-script" || a == L"--screenshot" || a == L"--selftest";
    g_automation = automation;
    // automation runs do not take the focus from whatever the user is doing
    ShowWindow(hwnd, automation ? SW_SHOWNOACTIVATE : (showCmd == SW_HIDE ? SW_SHOWDEFAULT : showCmd));
    UpdateWindow(hwnd);

    if (!separate) std::thread(PipeServer, hwnd).detach(); // automation instances do not take over the pipe
    for (const auto& a : args)
        if (a == L"--ui-script" || a == L"--selftest")
        {
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            std::thread(Watchdog, self).detach();
            break;
        }
    HandleArgs(app, args);

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(hwnd))
        {
            if (automation) ShowWindow(hwnd, SW_SHOWNOACTIVATE); // a script needs the full client area for its screenshots
            else
            {
                Sleep(16);
                continue;
            }
        }
        app.Frame();
        ++g_frames;
    }

    g_app = nullptr;
    app.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    CoUninitialize();
    // a selftest with errors in the log fails (CI)
    for (const auto& a : args)
        if (a == L"--selftest" && Log::Get().Errors() > 0) return 1;
    return 0; // the pipe thread is detached and ends with the process
}
