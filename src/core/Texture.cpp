// Textures without a GPU: PAA (DXT1/5, ARGB, LZO1X / BI-LZSS), TGA and procedural "#(argb,..)" images, mips, swizzle,
// texture roles. The D3D11 upload and cache are in Material.cpp (render).
#include "Texture.h"
#include "ArmaConfig.h" // EvalNumber
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

#include "CoreGuard.h"

namespace fs = std::filesystem;

// ============================================================================================ LZO1X / BI-LZSS



// LZO1X decompression (format of minilzo's lzo1x_decompress), bounds-checked. Index based to avoid pointer UB.
bool Lzo1xDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t& consumed)
{
    size_t ip = 0, op = 0, t = 0, dist = 0;
#define NEED_IP(n) if (inLen - ip < size_t(n)) return false
#define NEED_OP(n) if (outLen - op < size_t(n)) return false
#define COPY_MATCH(len)                                  \
    {                                                    \
        if (dist == 0 || dist > op) return false;        \
        NEED_OP(len);                                    \
        for (size_t k = 0; k < (len); ++k, ++op) out[op] = out[op - dist]; \
    }

    NEED_IP(1);
    if (in[ip] > 17)
    {
        t = in[ip++] - 17;
        if (t < 4) goto match_next;
        NEED_OP(t);
        NEED_IP(t + 1);
        do out[op++] = in[ip++]; while (--t > 0);
        goto first_literal_run;
    }
    for (;;)
    {
        NEED_IP(3);
        t = in[ip++];
        if (t >= 16) goto match;
        if (t == 0)
        {
            while (in[ip] == 0) { t += 255; ++ip; NEED_IP(1); }
            t += 15 + in[ip++];
        }
        NEED_OP(t + 3);
        NEED_IP(t + 6);
        for (size_t k = 0; k < t + 3; ++k) out[op++] = in[ip++];
    first_literal_run:
        NEED_IP(1);
        t = in[ip++];
        if (t >= 16) goto match;
        NEED_IP(1);
        dist = 1 + 0x0800 + (t >> 2) + (size_t(in[ip++]) << 2);
        COPY_MATCH(3);
        goto match_done;

        for (;;)
        {
        match:
            if (t >= 64)
            {
                NEED_IP(1);
                dist = 1 + ((t >> 2) & 7) + (size_t(in[ip++]) << 3);
                t = (t >> 5) - 1;
                COPY_MATCH(t + 2);
                goto match_done;
            }
            else if (t >= 32)
            {
                t &= 31;
                if (t == 0)
                {
                    NEED_IP(1);
                    while (in[ip] == 0) { t += 255; ++ip; NEED_IP(1); }
                    t += 31 + in[ip++];
                }
                NEED_IP(2);
                dist = 1 + (in[ip] >> 2) + (size_t(in[ip + 1]) << 6);
                ip += 2;
            }
            else if (t >= 16)
            {
                size_t hi = (t & 8) << 11;
                t &= 7;
                if (t == 0)
                {
                    NEED_IP(1);
                    while (in[ip] == 0) { t += 255; ++ip; NEED_IP(1); }
                    t += 7 + in[ip++];
                }
                NEED_IP(2);
                dist = hi + (in[ip] >> 2) + (size_t(in[ip + 1]) << 6);
                ip += 2;
                if (dist == 0) goto eof_found;
                dist += 0x4000;
            }
            else
            {
                NEED_IP(1);
                dist = 1 + (t >> 2) + (size_t(in[ip++]) << 2);
                COPY_MATCH(2);
                goto match_done;
            }
            COPY_MATCH(t + 2);
        match_done:
            t = in[ip - 2] & 3;
            if (t == 0) break;
        match_next:
            NEED_OP(t);
            NEED_IP(t + 1);
            do out[op++] = in[ip++]; while (--t > 0);
            t = in[ip++];
        }
    }
eof_found:
    consumed = ip;
    return op == outLen;
#undef NEED_IP
#undef NEED_OP
#undef COPY_MATCH
}


namespace {


// BI LZSS: flag byte (bit 1 = literal), 2-byte back references (12-bit distance, 4-bit length + 3),
// references before the start produce spaces; followed by a 4-byte additive checksum.
bool LzssDecompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t expected, size_t& consumed, bool& checksumOk)
{
    size_t ip = 0;
    out.clear();
    out.reserve(expected);
    while (out.size() < expected)
    {
        if (ip >= inLen) return false;
        uint8_t flags = in[ip++];
        for (int bit = 0; bit < 8 && out.size() < expected; ++bit, flags >>= 1)
        {
            if (flags & 1)
            {
                if (ip >= inLen) return false;
                out.push_back(in[ip++]);
            }
            else
            {
                if (ip + 1 >= inLen) return false;
                int b1 = in[ip], b2 = in[ip + 1];
                ip += 2;
                ptrdiff_t rpos = b1 | ((b2 & 0xF0) << 4);
                int rlen = (b2 & 0x0F) + 3;
                ptrdiff_t pos = ptrdiff_t(out.size()) - rpos;
                for (int k = 0; k < rlen && out.size() < expected; ++k)
                {
                    ptrdiff_t q = pos + k;
                    out.push_back(q < 0 ? 0x20 : out[size_t(q)]);
                }
            }
        }
    }
    checksumOk = false;
    if (ip + 4 <= inLen)
    {
        uint32_t sum = 0, stored = 0;
        for (uint8_t b : out) sum += b;
        std::memcpy(&stored, in + ip, 4);
        checksumOk = sum == stored;
        ip += 4;
    }
    consumed = ip;
    return true;
}

} // namespace

void GenerateMips(Image& img)
{
    if (img.format != Image::Format::RGBA8 || img.mips.empty()) return;
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1)
    {
        const auto& src = img.mips.back();
        uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
                    uint32_t y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
                    uint32_t s = src[(size_t(y0) * w + x0) * 4 + c] + src[(size_t(y0) * w + x1) * 4 + c] +
                                 src[(size_t(y1) * w + x0) * 4 + c] + src[(size_t(y1) * w + x1) * 4 + c];
                    dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
                }
        img.mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

bool ReadFileBytes(const fs::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

// ============================================================================================ decoders

bool DecodePaa(const std::vector<uint8_t>& d, Image& out, std::string& error)
{
    out = {};
    size_t o = 0;
    auto need = [&](size_t n) { return o <= d.size() && d.size() - o >= n; }; // o can pass the end (a palette size): no underflow
    if (!need(2)) { error = "file too short"; return false; }
    uint16_t type = uint16_t(d[0] | (d[1] << 8));
    o = 2;
    const char* typeName = nullptr;
    int bpp = 0; // bytes per pixel for ARGB formats
    switch (type)
    {
    case 0xFF01: typeName = "DXT1"; out.format = Image::Format::BC1; break;
    case 0xFF02: typeName = "DXT2"; out.format = Image::Format::BC2; break; // premultiplied variants: same blocks
    case 0xFF03: typeName = "DXT3"; out.format = Image::Format::BC2; break;
    case 0xFF04: typeName = "DXT4"; out.format = Image::Format::BC3; break;
    case 0xFF05: typeName = "DXT5"; out.format = Image::Format::BC3; break;
    case 0x4444: typeName = "ARGB4444"; bpp = 2; break;
    case 0x1555: typeName = "ARGB1555"; bpp = 2; break;
    case 0x8888: typeName = "ARGB8888"; bpp = 4; break;
    case 0x8080: typeName = "AI88"; bpp = 2; break;
    default: error = std::format("unsupported PAA type 0x{:04X}", type); return false;
    }
    while (need(12) && std::memcmp(&d[o], "GGAT", 4) == 0)
    {
        uint32_t len;
        std::memcpy(&len, &d[o + 8], 4);
        const bool swiz = std::memcmp(&d[o + 4], "ZIWS", 4) == 0;
        o += 12;
        if (!need(len)) { error = "truncated TAGG"; return false; }
        if (swiz && len == 4) // TexConvert channel swizzle, stored channels A, R, G, B
            for (int c = 0; c < 4; ++c) out.swizzle[c] = d[o + c];
        o += len;
    }
    if (!need(2)) { error = "truncated palette"; return false; }
    uint16_t pal = uint16_t(d[o] | (d[o + 1] << 8));
    o += 2 + size_t(pal) * 3;
    const bool dxt = bpp == 0;
    int lzoMips = 0, lzssMips = 0;
    uint32_t expectW = 0, expectH = 0;

    for (int mip = 0;; ++mip)
    {
        if (!need(4)) break;
        uint16_t w = uint16_t(d[o] | (d[o + 1] << 8)), h = uint16_t(d[o + 2] | (d[o + 3] << 8));
        o += 4;
        if (w == 0 && h == 0) break;
        if (w == 1234 && h == 8765) // large-texture marker: real size follows (CWR PAADecoder.cpp:358)
        {
            if (!need(4)) { error = std::format("truncated mip {} header", mip); return false; }
            w = uint16_t(d[o] | (d[o + 1] << 8));
            h = uint16_t(d[o + 2] | (d[o + 3] << 8));
            o += 4;
        }
        if (!need(3)) { error = std::format("truncated mip {} header", mip); return false; }
        size_t size = d[o] | (d[o + 1] << 8) | (size_t(d[o + 2]) << 16);
        o += 3;
        if (!need(size)) { error = std::format("mip {} data truncated ({} bytes)", mip, size); return false; }
        bool lzo = dxt && (w & 0x8000);
        w &= 0x7FFF;
        if (mip == 0) { out.width = w; out.height = h; }
        else if (w != expectW || h != expectH) break; // non-standard chain: keep what we have
        expectW = std::max(1u, uint32_t(w) / 2);
        expectH = std::max(1u, uint32_t(h) / 2);

        size_t blocks = size_t(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4);
        size_t expected = dxt ? blocks * (out.format == Image::Format::BC1 ? 8 : 16) : size_t(w) * h * bpp; // BC2/BC3: 16 bytes
        std::vector<uint8_t> raw;
        if (lzo)
        {
            raw.resize(expected);
            size_t used = 0;
            if (!Lzo1xDecompress(&d[o], size, raw.data(), expected, used)) { error = std::format("LZO error in mip {} ({}x{})", mip, w, h); return false; }
            ++lzoMips;
        }
        else if (!dxt && size < expected)
        {
            size_t used = 0;
            bool csOk = false;
            if (!LzssDecompress(&d[o], size, raw, expected, used, csOk)) { error = std::format("LZSS error in mip {}", mip); return false; }
            if (!csOk) { error = std::format("LZSS checksum mismatch in mip {}", mip); return false; }
            if (raw.size() < expected) { error = std::format("LZSS mip {}: {} of {} bytes", mip, raw.size(), expected); return false; }
            ++lzssMips;
        }
        else
        {
            if (size != expected) { error = std::format("mip {} has {} bytes, expected {}", mip, size, expected); return false; }
            raw.assign(d.begin() + o, d.begin() + o + size);
        }
        o += size;

        if (!dxt) // convert to RGBA8
        {
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            for (size_t i = 0; i < size_t(w) * h; ++i)
            {
                uint8_t* p = &rgba[i * 4];
                if (type == 0x8888) { p[0] = raw[i * 4 + 2]; p[1] = raw[i * 4 + 1]; p[2] = raw[i * 4 + 0]; p[3] = raw[i * 4 + 3]; }
                else
                {
                    uint16_t v = uint16_t(raw[i * 2] | (raw[i * 2 + 1] << 8));
                    if (type == 0x4444)
                    {
                        p[0] = uint8_t(((v >> 8) & 15) * 17); p[1] = uint8_t(((v >> 4) & 15) * 17);
                        p[2] = uint8_t((v & 15) * 17); p[3] = uint8_t((v >> 12) * 17);
                    }
                    else if (type == 0x1555)
                    {
                        p[0] = uint8_t(((v >> 10) & 31) * 255 / 31); p[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                        p[2] = uint8_t((v & 31) * 255 / 31); p[3] = (v & 0x8000) ? 255 : 0;
                    }
                    else { p[0] = p[1] = p[2] = uint8_t(v & 0xFF); p[3] = uint8_t(v >> 8); } // AI88
                }
            }
            raw = std::move(rgba);
        }
        out.mips.push_back(std::move(raw));
    }
    if (out.mips.empty()) { error = "no mipmaps"; return false; }
    if (dxt && ((out.width % 4) || (out.height % 4))) { error = std::format("{} size {}x{} is not a multiple of 4", typeName, out.width, out.height); return false; }
    out.description = std::format("{} {}x{}, {} mips{}{}", typeName, out.width, out.height, out.mips.size(),
                                  lzoMips ? std::format(", {} LZO", lzoMips) : "", lzssMips ? std::format(", {} LZSS", lzssMips) : "");
    if (!dxt && out.mips.size() == 1) GenerateMips(out);
    return true;
}

bool DecodeTga(const std::vector<uint8_t>& d, Image& out, std::string& error)
{
    out = {};
    if (d.size() < 18) { error = "TGA too short"; return false; }
    uint8_t idLen = d[0], cmap = d[1], type = d[2], bpp = d[16], desc = d[17];
    uint32_t w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8);
    bool rle = type == 10 || type == 11;
    bool gray = type == 3 || type == 11;
    if (cmap || !(type == 2 || type == 3 || type == 10 || type == 11)) { error = std::format("TGA type {} not supported", type); return false; }
    int bytes = bpp / 8;
    if (!(bytes == 1 && gray) && bytes != 3 && bytes != 4) { error = std::format("TGA {} bpp not supported", bpp); return false; }
    size_t o = 18 + idLen;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    size_t px = 0, total = size_t(w) * h;
    auto put = [&](const uint8_t* s) {
        uint8_t* p = &rgba[px * 4];
        if (bytes == 1) { p[0] = p[1] = p[2] = s[0]; p[3] = 255; }
        else { p[0] = s[2]; p[1] = s[1]; p[2] = s[0]; p[3] = bytes == 4 ? s[3] : 255; }
        ++px;
    };
    while (px < total)
    {
        if (!rle)
        {
            if (o + bytes > d.size()) { error = "TGA truncated"; return false; }
            put(&d[o]);
            o += bytes;
            continue;
        }
        if (o >= d.size()) { error = "TGA truncated"; return false; }
        uint8_t c = d[o++];
        size_t n = (c & 0x7F) + 1;
        if (c & 0x80)
        {
            if (o + bytes > d.size()) { error = "TGA truncated"; return false; }
            for (size_t k = 0; k < n && px < total; ++k) put(&d[o]);
            o += bytes;
        }
        else
            for (size_t k = 0; k < n && px < total; ++k)
            {
                if (o + bytes > d.size()) { error = "TGA truncated"; return false; }
                put(&d[o]);
                o += bytes;
            }
    }
    if (!(desc & 0x20)) // bottom-left origin -> flip
        for (uint32_t y = 0; y < h / 2; ++y)
            std::swap_ranges(rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4, rgba.begin() + size_t(h - 1 - y) * w * 4);
    out.width = w;
    out.height = h;
    out.mips.push_back(std::move(rgba));
    out.description = std::format("TGA {}x{} {}bpp{}", w, h, bpp, rle ? " RLE" : "");
    GenerateMips(out);
    return true;
}

bool MakeProceduralTexture(std::string_view spec, Image& out, std::string& error)
{
    out = {};
    std::string s = ToLower(spec);
    size_t gen = s.find(')');
    if (s.size() < 3 || s[0] != '#' || gen == std::string::npos) { error = "malformed procedural texture"; return false; }
    std::string rest = s.substr(gen + 1);
    size_t open = rest.find('('), close = rest.rfind(')');
    std::string name = rest.substr(0, open);
    if (name != "color" || open == std::string::npos || close == std::string::npos)
    {
        error = std::format("procedural generator \"{}\" not supported", name);
        return false;
    }
    std::vector<float> v;
    std::string args = rest.substr(open + 1, close - open - 1);
    size_t a = 0;
    while (a <= args.size())
    {
        size_t b = args.find(',', a);
        if (b == std::string::npos) b = args.size();
        double num;
        if (EvalNumber(args.substr(a, b - a), num)) v.push_back(float(num));
        a = b + 1;
    }
    if (v.size() < 3) { error = "color() needs r,g,b[,a]"; return false; }
    if (v.size() < 4) v.push_back(1.0f);
    uint8_t c[4];
    for (int i = 0; i < 4; ++i) c[i] = uint8_t(std::clamp(v[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    out.width = out.height = 4;
    out.mips.push_back(std::vector<uint8_t>(64));
    for (int i = 0; i < 16; ++i) std::memcpy(&out.mips[0][i * 4], c, 4);
    out.description = std::format("color({:.3f},{:.3f},{:.3f},{:.3f})", v[0], v[1], v[2], v[3]);
    GenerateMips(out);
    return true;
}

// ============================================================================================ roles / rvmat

const char* TexRoleName(TexRole r)
{
    static const char* names[] = { "diffuse", "normal", "specular", "ambient shadow", "macro", "detail", "-" };
    return names[int(r)];
}

TexRole RoleFromTextureName(std::string_view texture)
{
    std::string s = ToLower(texture);
    std::string suffix;
    if (!s.empty() && s[0] == '#')
    {
        size_t close = s.rfind(')'), comma = s.rfind(',', close);
        if (close != std::string::npos && comma != std::string::npos) suffix = s.substr(comma + 1, close - comma - 1);
        suffix.erase(std::remove(suffix.begin(), suffix.end(), ' '), suffix.end());
    }
    else
    {
        std::string stem = PathToUtf8(ArmaStringPath(s).stem());
        size_t us = stem.rfind('_');
        if (us != std::string::npos) suffix = stem.substr(us + 1);
    }
    if (suffix == "co" || suffix == "ca" || suffix == "lco" || suffix == "lca") return TexRole::Diffuse;
    if (suffix == "nohq" || suffix == "no" || suffix == "novhq" || suffix == "nopx" || suffix == "non" || suffix == "nof") return TexRole::Normal;
    if (suffix == "smdi" || suffix == "sm") return TexRole::Specular;
    if (suffix == "as" || suffix == "ads") return TexRole::AmbientShadow;
    if (suffix == "mc" || suffix == "mca") return TexRole::Macro;
    if (suffix == "dt" || suffix == "cdt" || suffix == "mco") return TexRole::Detail;
    return TexRole::Count;
}

Unswizzle UnswizzleOf(const uint8_t swizzle[4])
{
    // stored channel c (A,R,G,B) holds code k: orig[k & 3] (inverted for 4..7). Invert per channel; channels the
    // swizzle does not carry keep the stored value. ARGB index -> rgba index: A 3, R 0, G 1, B 2.
    static const int rgba[4] = { 3, 0, 1, 2 };
    Unswizzle u;
    bool set[4] = {};
    for (int c = 0; c < 4; ++c)
    {
        const int k = swizzle[c];
        if (k >= 8) continue;
        const int orig = rgba[k & 3], stored = rgba[c];
        if (set[orig]) continue;
        set[orig] = true;
        for (int j = 0; j < 4; ++j) u.m[orig][j] = 0;
        u.m[orig][stored] = k >= 4 ? -1.0f : 1.0f;
        u.offset[orig] = k >= 4 ? 1.0f : 0.0f;
    }
    return u;
}
