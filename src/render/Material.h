#pragma once
// Materials on the GPU: the D3D11 texture cache (decoders: Texture.h, RVMAT: RvMaterial.h - both in the core) and WIC
// images (png / jpg / bmp).
#include "RvMaterial.h"
#include "Texture.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

bool DecodeWic(const std::filesystem::path& path, Image& out, std::string& error); // png/jpg/bmp

struct TextureEntry
{
    std::string key;                 // texture string as referenced (lower case)
    std::filesystem::path resolved;  // file actually loaded (empty for procedural / missing)
    enum class Status { Ok, Procedural, Missing, Error, Loading } status = Status::Missing;
    std::string info;                // format/size or the error
    ComPtr<ID3D11ShaderResourceView> srv; // placeholder for Missing/Error
    uint32_t width = 0, height = 0;
    uint64_t fileSize = 0;  // of `resolved` when loaded (DropChanged)
    int64_t fileTime = 0;
    uint8_t swizzle[4] = { 0, 1, 2, 3 }; // PAA SWIZ tag (see Image)
};

// Loads each referenced texture once. Arma paths resolve against the project root; a .tga/.png reference
// prefers the .paa next to it (as Arma does). Missing/broken textures get a magenta checker and one log line.
class TextureCache
{
public:
    void Init(ID3D11Device* device, std::filesystem::path projectRoot);
    void SetProjectRoot(std::filesystem::path root) { root_ = std::move(root); }
    const TextureEntry& Get(const std::string& texture);
    // Decodes the textures not cached yet on worker threads (all cores), then uploads them; Get is then a lookup.
    void Prefetch(const std::vector<std::string>& textures);
    // Streaming: the textures not cached yet are decoded on worker threads; until uploaded, Get returns a "loading"
    // placeholder (grey, never blocks). PollStreamed uploads a few finished ones per frame; true: some arrived.
    void PrefetchAsync(const std::vector<std::string>& textures);
    bool PollStreamed(int maxUploads = 16);
    bool Streaming() const { return streamTotal_ > streamDone_; }
    int StreamDone() const { return streamDone_; }
    int StreamTotal() const { return streamTotal_; }
    // Keeps entries whose file is unchanged (model reload keeps textures); returns how many were dropped.
    int DropChanged();
    // 1x1 defaults for empty slots
    ID3D11ShaderResourceView* Default(TexRole role);
    ID3D11ShaderResourceView* Untextured() { return untextured_.Get(); } // grey for faces without any texture
    void Clear() { entries_.clear(); }
    // Drops every entry loaded from `file` (hot reload). Returns how many.
    int Invalidate(const std::filesystem::path& file);
    bool AnyError(const std::filesystem::path& file) const;
    std::vector<std::filesystem::path> ResolvedFiles() const;
    size_t Count() const { return entries_.size(); }

private:
    struct Decoded { TextureEntry entry; Image image; std::string error; bool ok = false; };
    Decoded Decode(const std::string& texture) const; // no D3D, no logging (thread safe)
    const TextureEntry& Finish(Decoded&& d);           // upload + log + insert (UI thread)
    ComPtr<ID3D11ShaderResourceView> Upload(const Image& img, std::string& error);
    ComPtr<ID3D11ShaderResourceView> Solid(uint32_t rgba);

    ID3D11Device* device_ = nullptr;
    std::filesystem::path root_;
    std::map<std::string, TextureEntry> entries_;
    ComPtr<ID3D11ShaderResourceView> defaults_[int(TexRole::Count)];
    ComPtr<ID3D11ShaderResourceView> placeholder_, untextured_;
    // streaming: names decoding, finished decodes waiting for the upload, the workers
    struct Stream
    {
        std::mutex m;
        std::vector<std::string> todo;
        std::vector<Decoded> done;
        bool stop = false;
    };
    std::shared_ptr<Stream> stream_ = std::make_shared<Stream>();
    std::vector<std::jthread> workers_;
    std::set<std::string> pending_;  // keys being decoded
    TextureEntry loading_;           // what Get returns for them
    int streamTotal_ = 0, streamDone_ = 0;
public:
    ~TextureCache();
};
