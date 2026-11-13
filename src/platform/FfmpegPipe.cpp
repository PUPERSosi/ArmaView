#include "FfmpegPipe.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <format>
#include <fstream>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

bool FfmpegPipe::Start(const std::wstring& exe, const std::wstring& args, const fs::path& logFile, std::string& error)
{
    Abort();
    log_ = logFile;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) { error = "CreatePipe failed"; return false; }
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0); // only ffmpeg's end is inherited
    HANDLE logH = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = readEnd;
    si.hStdOutput = logH != INVALID_HANDLE_VALUE ? logH : nullptr;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = std::format(L"\"{}\" {}", exe, args);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(readEnd);
    if (logH != INVALID_HANDLE_VALUE) CloseHandle(logH);
    if (!ok)
    {
        CloseHandle(writeEnd);
        error = std::format("cannot start ffmpeg ({}) - set its path in the Record window", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    input_ = writeEnd;
    return true;
}

bool FfmpegPipe::Write(const void* data, size_t bytes)
{
    if (!input_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (bytes)
    {
        DWORD n = 0;
        if (!WriteFile(input_, p, DWORD(std::min<size_t>(bytes, 1 << 20)), &n, nullptr) || n == 0) return false;
        p += n;
        bytes -= n;
    }
    return true;
}

bool FfmpegPipe::Finish(int& exitCode, std::string& logTail)
{
    exitCode = -1;
    if (input_) { CloseHandle(input_); input_ = nullptr; }
    if (process_)
    {
        WaitForSingleObject(process_, 120000);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        exitCode = int(code);
        CloseHandle(process_);
        process_ = nullptr;
    }
    std::ifstream in(log_, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    logTail = all.size() > 600 ? all.substr(all.size() - 600) : all;
    return exitCode == 0;
}

void FfmpegPipe::Abort()
{
    if (input_) { CloseHandle(input_); input_ = nullptr; }
    if (process_)
    {
        if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
    }
}

std::wstring FfmpegArgs(int w, int h, int fps, const fs::path& out)
{
    const std::wstring in = std::format(L"-hide_banner -y -f rawvideo -pix_fmt rgba -s {}x{} -r {} -i -", w, h, fps);
    std::wstring ext = out.extension().native();
    for (auto& c : ext) c = wchar_t(towlower(c));
    if (ext == L".gif")
        return in + L" -vf \"split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4\" -loop 0 \"" + out.native() + L"\"";
    return in + L" -c:v libx264 -pix_fmt yuv420p -crf 18 -preset medium -movflags +faststart \"" + out.native() + L"\"";
}

bool WritePngRgba(const fs::path& file, int w, int h, const std::vector<uint8_t>& rgba, std::string& error)
{
    std::vector<BYTE> bgra(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        bgra[i] = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i];
        bgra[i + 3] = 255;
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(w), UINT(h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(UINT(h), UINT(w) * 4, UINT(bgra.size()), bgra.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) error = std::format("PNG write failed (0x{:08x})", unsigned(hr));
    return SUCCEEDED(hr);
}
