#include "Material.h"
#include "Perf.h"
#include "Log.h"

#include <wincodec.h>

#include <algorithm>
#include <thread>
#include <atomic>
#include <functional>
#include <cmath>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;


bool DecodeWic(const fs::path& path, Image& out, std::string& error)
{
    out = {};
    // texture decoding runs on worker threads: COM once per thread (multithreaded apartment; S_FALSE / RPC_E_CHANGED_MODE
    // = already initialized there, fine)
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (SUCCEEDED(hr)) hr = conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
    if (FAILED(hr)) { error = std::format("WIC decode failed (hr=0x{:08X})", unsigned(hr)); return false; }
    out.width = w;
    out.height = h;
    out.mips.push_back(std::move(px));
    out.description = std::format("{} {}x{}", PathToUtf8(path.extension()), w, h);
    GenerateMips(out);
    return true;
}

// ============================================================================================ texture cache (D3D11)

void TextureCache::Init(ID3D11Device* device, fs::path projectRoot)
{
    device_ = device;
    root_ = std::move(projectRoot);
    entries_.clear();
    defaults_[int(TexRole::Diffuse)] = Solid(0xFFFFFFFF);
    defaults_[int(TexRole::Normal)] = Solid(0xFFFF8080);        // (0.5, 0.5, 1)
    defaults_[int(TexRole::Specular)] = Solid(0xFFFFFFFF);
    defaults_[int(TexRole::AmbientShadow)] = Solid(0xFFFFFFFF);
    defaults_[int(TexRole::Macro)] = Solid(0x00000000);
    defaults_[int(TexRole::Detail)] = Solid(0xFF808080);
    untextured_ = Solid(0xFFA0A0A0);
    Image chk;
    chk.width = chk.height = 8;
    chk.mips.push_back(std::vector<uint8_t>(256));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            uint8_t* p = &chk.mips[0][(y * 8 + x) * 4];
            bool on = ((x / 2) + (y / 2)) & 1;
            p[0] = on ? 255 : 30; p[1] = 0; p[2] = on ? 255 : 30; p[3] = 255;
        }
    GenerateMips(chk);
    std::string err;
    placeholder_ = Upload(chk, err);
}

ID3D11ShaderResourceView* TextureCache::Default(TexRole role) { return defaults_[int(role == TexRole::Count ? TexRole::Diffuse : role)].Get(); }

ComPtr<ID3D11ShaderResourceView> TextureCache::Solid(uint32_t rgba)
{
    Image img;
    img.width = img.height = 1;
    img.mips.push_back(std::vector<uint8_t>(4));
    std::memcpy(img.mips[0].data(), &rgba, 4);
    std::string err;
    return Upload(img, err);
}

ComPtr<ID3D11ShaderResourceView> TextureCache::Upload(const Image& img, std::string& error)
{
    DXGI_FORMAT fmt = img.format == Image::Format::BC1 ? DXGI_FORMAT_BC1_UNORM
                    : img.format == Image::Format::BC2 ? DXGI_FORMAT_BC2_UNORM
                    : img.format == Image::Format::BC3 ? DXGI_FORMAT_BC3_UNORM
                                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.width;
    td.Height = img.height;
    td.MipLevels = UINT(img.mips.size());
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(img.mips.size());
    uint32_t w = img.width, h = img.height;
    for (size_t i = 0; i < img.mips.size(); ++i)
    {
        UINT pitch = img.format == Image::Format::RGBA8 ? w * 4 : UINT(std::max(1u, (w + 3) / 4) * (img.format == Image::Format::BC1 ? 8 : 16));
        init[i] = { img.mips[i].data(), pitch, 0 };
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateTexture2D(&td, init.data(), &tex);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) error = std::format("CreateTexture2D failed (hr=0x{:08X})", unsigned(hr));
    return srv;
}

int TextureCache::Invalidate(const fs::path& file)
{
    int n = 0;
    std::error_code ec;
    for (auto it = entries_.begin(); it != entries_.end();)
        if (!it->second.resolved.empty() && fs::equivalent(it->second.resolved, file, ec)) { it = entries_.erase(it); ++n; }
        else ++it;
    return n;
}

bool TextureCache::AnyError(const fs::path& file) const
{
    std::error_code ec;
    for (const auto& [k, e] : entries_)
        if (!e.resolved.empty() && e.status == TextureEntry::Status::Error && fs::equivalent(e.resolved, file, ec)) return true;
    return false;
}

std::vector<fs::path> TextureCache::ResolvedFiles() const
{
    std::vector<fs::path> out;
    for (const auto& [k, e] : entries_)
        if (!e.resolved.empty()) out.push_back(e.resolved);
    return out;
}

TextureCache::Decoded TextureCache::Decode(const std::string& texture) const
{
    Decoded d;
    TextureEntry& e = d.entry;
    e.key = ToLower(texture);
    if (!e.key.empty() && e.key[0] == '#')
    {
        d.ok = MakeProceduralTexture(texture, d.image, d.error);
        e.status = d.ok ? TextureEntry::Status::Procedural : TextureEntry::Status::Error;
        return d;
    }
    fs::path p = ResolveArmaPath(texture, root_);
    std::string ext = ToLower(PathToUtf8(p.extension()));
    std::vector<fs::path> candidates;
    if (ext.empty()) candidates.push_back(fs::path(p) += ".paa");
    else if (ext != ".paa" && ext != ".pac") candidates.push_back(fs::path(p).replace_extension(".paa"));
    candidates.push_back(p);
    std::error_code ec;
    for (const auto& cnd : candidates)
        if (fs::exists(cnd, ec)) { e.resolved = cnd; break; }
    if (e.resolved.empty())
    {
        e.status = TextureEntry::Status::Missing;
        d.error = std::format("not found: {}", PathToUtf8(candidates.front()));
        return d;
    }
    e.fileSize = uint64_t(fs::file_size(e.resolved, ec));
    e.fileTime = int64_t(fs::last_write_time(e.resolved, ec).time_since_epoch().count());
    std::vector<uint8_t> bytes;
    std::string rext = ToLower(PathToUtf8(e.resolved.extension()));
    if (rext == ".paa" || rext == ".pac") d.ok = ReadFileBytes(e.resolved, bytes) && DecodePaa(bytes, d.image, d.error);
    else if (rext == ".tga") d.ok = ReadFileBytes(e.resolved, bytes) && DecodeTga(bytes, d.image, d.error);
    else d.ok = DecodeWic(e.resolved, d.image, d.error);
    e.status = d.ok ? TextureEntry::Status::Ok : TextureEntry::Status::Error;
    return d;
}

const TextureEntry& TextureCache::Finish(Decoded&& d)
{
    auto& log = Log::Get();
    TextureEntry& e = d.entry;
    if (e.status == TextureEntry::Status::Missing) log.Warn("Texture '{}' not found on {} (placeholder used)", e.key, PathToUtf8(root_));
    else if (!d.ok && e.status == TextureEntry::Status::Error)
    {
        if (e.resolved.empty()) log.Debug("Procedural texture '{}': {}", e.key, d.error);
        else log.Error("Texture '{}': {}", PathToUtf8(e.resolved), d.error.empty() ? "read error" : d.error);
    }
    bool ok = d.ok;
    if (ok)
    {
        e.srv = Upload(d.image, d.error);
        if (!e.srv) { ok = false; e.status = TextureEntry::Status::Error; log.Error("Texture '{}': {}", e.key, d.error); }
    }
    if (ok)
    {
        e.info = d.image.description;
        e.width = d.image.width;
        e.height = d.image.height;
        for (int c = 0; c < 4; ++c) e.swizzle[c] = d.image.swizzle[c];
        if (d.image.Swizzled()) e.info += std::format(", swizzle {}{}{}{}", d.image.swizzle[0], d.image.swizzle[1], d.image.swizzle[2], d.image.swizzle[3]);
    }
    else
    {
        e.info = d.error;
        e.srv = placeholder_;
    }
    std::string key = e.key;
    return entries_.insert_or_assign(key, std::move(e)).first->second;
}

const TextureEntry& TextureCache::Get(const std::string& texture)
{
    std::string key = ToLower(texture);
    if (auto it = entries_.find(key); it != entries_.end()) return it->second;
    if (pending_.count(key))
    {
        loading_.key = key;
        loading_.status = TextureEntry::Status::Loading;
        loading_.info = "loading...";
        loading_.srv = untextured_;
        return loading_;
    }
    PerfNoteScope perfScope("load: textures (read+decode+upload)");
    return Finish(Decode(texture));
}

void TextureCache::Prefetch(const std::vector<std::string>& textures)
{
    std::vector<std::string> todo;
    for (const auto& t : textures)
    {
        std::string k = ToLower(t);
        if (!k.empty() && !entries_.count(k) &&
            std::none_of(todo.begin(), todo.end(), [&](const std::string& x) { return ToLower(x) == k; }))
            todo.push_back(t);
    }
    if (todo.empty()) return;
    PerfNoteScope perfScope("load: textures (read+decode+upload)");
    std::vector<Decoded> done(todo.size());
    std::atomic<size_t> next{ 0 };
    const unsigned workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(todo.size())));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < todo.size();) done[i] = Decode(todo[i]);
        });
    for (auto& t : pool) t.join();
    for (auto& d : done) Finish(std::move(d));
}

int TextureCache::DropChanged()
{
    int n = 0;
    std::error_code ec;
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        const TextureEntry& e = it->second;
        bool drop = e.status == TextureEntry::Status::Missing || e.status == TextureEntry::Status::Error;
        if (!drop && !e.resolved.empty())
            drop = uint64_t(fs::file_size(e.resolved, ec)) != e.fileSize ||
                   int64_t(fs::last_write_time(e.resolved, ec).time_since_epoch().count()) != e.fileTime;
        if (drop) { it = entries_.erase(it); ++n; }
        else ++it;
    }
    return n;
}


TextureCache::~TextureCache()
{
    {
        std::lock_guard lock(stream_->m);
        stream_->stop = true;
        stream_->todo.clear();
    }
    workers_.clear(); // joins
}

void TextureCache::PrefetchAsync(const std::vector<std::string>& textures)
{
    std::vector<std::string> todo;
    for (const auto& t : textures)
    {
        const std::string k = ToLower(t);
        if (k.empty() || entries_.count(k) || pending_.count(k)) continue;
        pending_.insert(k);
        todo.push_back(t);
    }
    if (todo.empty()) return;
    if (!Streaming()) streamTotal_ = streamDone_ = 0;
    streamTotal_ += int(todo.size());
    {
        std::lock_guard lock(stream_->m);
        stream_->todo.insert(stream_->todo.end(), todo.begin(), todo.end());
    }
    // a worker per core while there is work; each takes the next name until the list is empty, then ends
    const unsigned want = std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    std::erase_if(workers_, [](std::jthread& t) { return !t.joinable(); });
    while (workers_.size() < want)
        workers_.emplace_back([this, s = stream_] {
            for (;;)
            {
                std::string name;
                {
                    std::lock_guard lock(s->m);
                    if (s->stop || s->todo.empty()) return;
                    name = std::move(s->todo.back());
                    s->todo.pop_back();
                }
                Decoded d = Decode(name); // no D3D, no logging
                std::lock_guard lock(s->m);
                if (!s->stop) s->done.push_back(std::move(d));
            }
        });
}

bool TextureCache::PollStreamed(int maxUploads)
{
    std::vector<Decoded> ready;
    {
        std::lock_guard lock(stream_->m);
        while (!stream_->done.empty() && int(ready.size()) < maxUploads)
        {
            ready.push_back(std::move(stream_->done.back()));
            stream_->done.pop_back();
        }
    }
    if (ready.empty()) return false;
    PerfNoteScope perfScope("load: textures (streamed upload)");
    for (auto& d : ready)
    {
        pending_.erase(d.entry.key);
        Finish(std::move(d));
        ++streamDone_;
    }
    if (!Streaming())
    {
        std::lock_guard lock(stream_->m);
        if (stream_->todo.empty()) std::erase_if(workers_, [](std::jthread&) { return true; }); // all done: join the idle workers
    }
    return true;
}
