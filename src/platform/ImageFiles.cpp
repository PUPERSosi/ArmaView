#include "ImageFiles.h"
#include "RunProgram.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <ocidl.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

bool WriteImageFile(const fs::path& file, const RgbaImage& img, std::string& error, float jpgQuality)
{
    if (img.Empty()) { error = "empty picture"; return false; }
    std::wstring ext = file.extension().native();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    const bool jpg = ext == L".jpg" || ext == L".jpeg";
    std::vector<BYTE> bgra(img.px.size());
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
    {
        bgra[i] = img.px[i + 2];
        bgra[i + 1] = img.px[i + 1];
        bgra[i + 2] = img.px[i];
        bgra[i + 3] = jpg ? 255 : img.px[i + 3];
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && jpg && props)
    {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = std::clamp(jpgQuality, 0.0f, 1.0f);
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(img.w), UINT(img.h));
    WICPixelFormatGUID fmt = jpg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    std::vector<BYTE> bgr;
    if (jpg)
    {
        bgr.resize(size_t(img.w) * img.h * 3);
        for (size_t p = 0; p < size_t(img.w) * img.h; ++p)
            for (int k = 0; k < 3; ++k) bgr[p * 3 + k] = bgra[p * 4 + k];
    }
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && jpg && fmt != GUID_WICPixelFormat24bppBGR) hr = E_FAIL; // the encoder wants another layout
    if (SUCCEEDED(hr))
        hr = jpg ? frame->WritePixels(UINT(img.h), UINT(img.w) * 3, UINT(bgr.size()), bgr.data())
                 : frame->WritePixels(UINT(img.h), UINT(img.w) * 4, UINT(bgra.size()), bgra.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) error = std::format("{} write failed (0x{:08x})", jpg ? "JPG" : "PNG", unsigned(hr));
    return SUCCEEDED(hr);
}

bool ConvertToPaa(const fs::path& tool, const fs::path& png, const fs::path& paa, std::string& error)
{
    std::error_code ec;
    if (tool.empty() || !fs::exists(tool, ec)) { error = "ImageToPAA not set (Arma 3 Tools\\ImageToPAA\\ImageToPAA.exe)"; return false; }
    fs::remove(paa, ec);
    std::string out;
    if (!RunProgram(tool.native(), L"\"" + png.native() + L"\" \"" + paa.native() + L"\"", true, out)) { error = "ImageToPAA did not start"; return false; }
    if (!fs::exists(paa, ec) || fs::file_size(paa, ec) == 0)
    {
        error = "ImageToPAA wrote no PAA: " + out.substr(0, 300);
        return false;
    }
    return true;
}

fs::path FindImageToPaa()
{
    std::error_code ec;
    std::vector<fs::path> roots;
    for (const wchar_t* base : { L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam" }) roots.emplace_back(base);
    for (wchar_t d = L'C'; d <= L'Z'; ++d)
    {
        roots.emplace_back(std::wstring(1, d) + L":\\Steam");
        roots.emplace_back(std::wstring(1, d) + L":\\SteamLibrary");
    }
    for (const auto& r : roots)
    {
        const fs::path p = r / L"steamapps\\common\\Arma 3 Tools\\ImageToPAA\\ImageToPAA.exe";
        if (fs::exists(p, ec)) return p;
    }
    return {};
}
