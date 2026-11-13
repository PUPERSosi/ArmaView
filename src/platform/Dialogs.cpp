#include "Dialogs.h"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

std::filesystem::path OpenFileDialog(void* owner, const wchar_t* filter, const std::filesystem::path& initialDir)
{
    wchar_t file[MAX_PATH] = L"";
    const std::wstring dir = initialDir.native();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return file;
}

std::filesystem::path SaveFileDialog(void* owner, const wchar_t* filter, const wchar_t* defaultExt, const std::filesystem::path& initial)
{
    wchar_t file[MAX_PATH] = L"";
    wcsncpy_s(file, initial.filename().c_str(), _TRUNCATE);
    const std::wstring dir = initial.parent_path().native();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return file;
}

std::filesystem::path PickFolderDialog(void* owner, const wchar_t* title)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    dlg->SetTitle(title);
    if (FAILED(dlg->Show(static_cast<HWND>(owner)))) return {};
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
    {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}
