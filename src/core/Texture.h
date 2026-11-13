#pragma once
// Textures without a GPU: decoders to RGBA8 / BC images with mips, PAA channel swizzle, texture roles.
//
// PAA layout (checked on 400 files from P:): uint16 type (0xFF01 DXT1, 0xFF05 DXT5, 0x4444, 0x1555, 0x8888, 0x8080),
// "GGAT"+name+uint32 len+data tags, uint16 palette count (+3 bytes each), then mips: uint16 w, uint16 h, uint24 size, data;
// w = h = 0 ends. DXT mips with (w & 0x8000) are LZO1X compressed; ARGB mips smaller than w*h*bpp are BI-LZSS compressed.
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct Image
{
    enum class Format { RGBA8, BC1, BC2, BC3 };
    Format format = Format::RGBA8;
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> mips; // mip 0 first
    std::string description;                // e.g. "DXT5 2048x2048, 12 mips"
    // PAA "SWIZ" tag (TexConvert channel swizzle), per stored channel A,R,G,B: 0..3 = source A,R,G,B,
    // 4..7 = 1 - source, 8 = one. Identity: {0,1,2,3}. _nohq: {5,4,2,3} = X stored in alpha as 1-X.
    uint8_t swizzle[4] = { 0, 1, 2, 3 };
    bool Swizzled() const { return swizzle[0] != 0 || swizzle[1] != 1 || swizzle[2] != 2 || swizzle[3] != 3; }
};
// Linear map from the sampled (stored) rgba back to the original rgba: orig = m * stored + offset.
struct Unswizzle
{
    float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } }; // [orig][stored], rgba order
    float offset[4] = { 0, 0, 0, 0 };
};
Unswizzle UnswizzleOf(const uint8_t swizzle[4]);

// Decoders (no logging; false + error on failure).
bool DecodePaa(const std::vector<uint8_t>& file, Image& out, std::string& error);
bool DecodeTga(const std::vector<uint8_t>& file, Image& out, std::string& error);
// "#(argb,8,8,3)color(r,g,b,a,TAG)" -> 8x8 RGBA. Other generators: false.
bool MakeProceduralTexture(std::string_view spec, Image& out, std::string& error);

// Texture slot roles of the viewer's super-shader approximation.
enum class TexRole { Diffuse, Normal, Specular, AmbientShadow, Macro, Detail, Count };
const char* TexRoleName(TexRole r);
// Role from a texture name suffix (_co/_ca, _nohq, _smdi, _as, _mc, _dt/_cdt) or procedural tag; Count if unknown.
TexRole RoleFromTextureName(std::string_view texture);

// Box-filtered mips down to 1x1 (RGBA8 images; BC images keep their stored mips).
void GenerateMips(Image& img);
// Whole file into memory; false if it cannot be opened / read.
bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out);

// LZO1X decompression (minilzo format), bounds-checked; consumed = input bytes used.
bool Lzo1xDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t& consumed);
