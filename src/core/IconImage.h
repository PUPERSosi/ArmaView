#pragma once
// Pictures for the Icons mode on the CPU (RGBA8, straight alpha): the item's opaque bounds, fitting it into a picture
// with a free border, exposure / saturation / contrast, a white silhouette, an outline, a background for JPG.
#include <cstdint>
#include <vector>

struct RgbaImage
{
    int w = 0, h = 0;
    std::vector<uint8_t> px; // w * h * 4
    bool Empty() const { return w <= 0 || h <= 0 || px.size() < size_t(w) * size_t(h) * 4; }
};

struct ImageBounds
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // opaque pixels: [x0, x1) x [y0, y1); empty when x1 <= x0
    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};
ImageBounds AlphaBounds(const RgbaImage& img, uint8_t threshold = 8);

// Box filter by an integer factor (alpha-weighted colours)
RgbaImage Downsample(const RgbaImage& img, int factor);
// The item (its opaque bounds) scaled to fit into w x h inside the border (fractions of w / h), keeping its proportions;
// centred, or standing on the bottom border (bottom = true). Bilinear, alpha-weighted. Transparent around it.
RgbaImage FitToFrame(const RgbaImage& img, int w, int h, float marginL, float marginR, float marginT, float marginB, bool bottom);
// exposure in EV (0: none), saturation and contrast as factors (1: none); on the colour, the alpha stays
void AdjustColor(RgbaImage& img, float exposureEv, float saturation, float contrast);
// every pixel the given colour, the alpha kept (a silhouette)
void Silhouette(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b);
// a border of `radius` pixels around the opaque shape, behind it
void Outline(RgbaImage& img, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
// composited over a colour, fully opaque (for JPG)
void FlattenOn(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b);

// what the measurements of BI's pictures use: fill and free border of the opaque pixels (fractions)
struct ImageStats
{
    bool alpha = false;      // some pixel not opaque
    float fillW = 0, fillH = 0;
    float marginL = 0, marginR = 0, marginT = 0, marginB = 0;
    bool whiteSilhouette = false; // the opaque pixels near white and one colour
};
ImageStats MeasureImage(const RgbaImage& img);
