#include "IconImage.h"

#include <algorithm>
#include <cmath>

#include "CoreGuard.h"

namespace
{
inline uint8_t U8(float v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); }
} // namespace

ImageBounds AlphaBounds(const RgbaImage& img, uint8_t threshold)
{
    ImageBounds b{ img.w, img.h, 0, 0 };
    if (img.Empty()) return {};
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.px[(size_t(y) * img.w + x) * 4 + 3] > threshold)
            {
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1);
                b.y1 = std::max(b.y1, y + 1);
            }
    if (b.x1 <= b.x0) return {};
    return b;
}

RgbaImage Downsample(const RgbaImage& img, int f)
{
    if (f <= 1 || img.Empty()) return img;
    RgbaImage o;
    o.w = img.w / f;
    o.h = img.h / f;
    o.px.assign(size_t(o.w) * o.h * 4, 0);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
        {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < f; ++sy)
                for (int sx = 0; sx < f; ++sx)
                {
                    const uint8_t* p = &img.px[(size_t(y * f + sy) * img.w + (x * f + sx)) * 4];
                    const float al = p[3] / 255.0f;
                    r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al;
                }
            uint8_t* q = &o.px[(size_t(y) * o.w + x) * 4];
            if (a > 0) { q[0] = U8(r / a); q[1] = U8(g / a); q[2] = U8(b / a); }
            q[3] = U8(a / float(f * f) * 255.0f);
        }
    return o;
}

RgbaImage FitToFrame(const RgbaImage& img, int w, int h, float mL, float mR, float mT, float mB, bool bottom)
{
    RgbaImage o;
    o.w = w;
    o.h = h;
    o.px.assign(size_t(w) * h * 4, 0);
    const ImageBounds bb = AlphaBounds(img);
    if (bb.Empty() || w <= 0 || h <= 0) return o;
    const float boxL = mL * w, boxR = w - mR * w, boxT = mT * h, boxB = h - mB * h;
    const float bw = float(bb.x1 - bb.x0), bh = float(bb.y1 - bb.y0);
    const float scale = std::min((boxR - boxL) / bw, (boxB - boxT) / bh);
    const float dw = bw * scale, dh = bh * scale;
    const float ox = boxL + ((boxR - boxL) - dw) * 0.5f;
    const float oy = bottom ? boxB - dh : boxT + ((boxB - boxT) - dh) * 0.5f;
    // each target pixel: the source area under it (box filter when shrinking, bilinear when enlarging)
    const float step = 1.0f / scale;
    const int taps = std::max(1, int(std::ceil(step)));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float sx0 = bb.x0 + (x - ox) * step, sy0 = bb.y0 + (y - oy) * step;
            float r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int ty = 0; ty < taps; ++ty)
                for (int tx = 0; tx < taps; ++tx)
                {
                    const float sx = sx0 + (tx + 0.5f) * step / taps - 0.5f, sy = sy0 + (ty + 0.5f) * step / taps - 0.5f;
                    n += 1;
                    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    const float fx = sx - ix, fy = sy - iy;
                    for (int k = 0; k < 4; ++k)
                    {
                        const int px = ix + (k & 1), py = iy + (k >> 1);
                        if (px < bb.x0 || py < bb.y0 || px >= bb.x1 || py >= bb.y1) continue;
                        const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                        const uint8_t* p = &img.px[(size_t(py) * img.w + px) * 4];
                        const float al = p[3] / 255.0f * wgt;
                        r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al;
                    }
                }
            uint8_t* q = &o.px[(size_t(y) * w + x) * 4];
            if (a > 0) { q[0] = U8(r / a); q[1] = U8(g / a); q[2] = U8(b / a); }
            q[3] = U8(a / n * 255.0f);
        }
    return o;
}

void AdjustColor(RgbaImage& img, float ev, float sat, float con)
{
    if (ev == 0 && sat == 1 && con == 1) return;
    const float gain = std::exp2(ev);
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
    {
        float c[3] = { img.px[i] / 255.0f, img.px[i + 1] / 255.0f, img.px[i + 2] / 255.0f };
        for (float& v : c) v *= gain;
        const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
        for (float& v : c)
        {
            v = l + (v - l) * sat;
            v = 0.5f + (v - 0.5f) * con;
        }
        for (int k = 0; k < 3; ++k) img.px[i + k] = U8(c[k] * 255.0f);
    }
}

void Silhouette(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
    {
        img.px[i] = r;
        img.px[i + 1] = g;
        img.px[i + 2] = b;
    }
}

void Outline(RgbaImage& img, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (radius <= 0 || img.Empty()) return;
    std::vector<uint8_t> grow(size_t(img.w) * img.h, 0);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
        {
            uint8_t m = 0;
            for (int dy = -radius; dy <= radius && m < 255; ++dy)
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    if (dx * dx + dy * dy > radius * radius) continue;
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= img.w || sy >= img.h) continue;
                    m = std::max(m, img.px[(size_t(sy) * img.w + sx) * 4 + 3]);
                }
            grow[size_t(y) * img.w + x] = uint8_t(m * a / 255);
        }
    for (size_t p = 0; p < grow.size(); ++p)
    {
        uint8_t* q = &img.px[p * 4];
        const float fa = q[3] / 255.0f, ba = grow[p] / 255.0f;
        const float oa = fa + ba * (1 - fa);
        if (oa <= 0) continue;
        const uint8_t bc[3] = { r, g, b };
        for (int k = 0; k < 3; ++k) q[k] = U8((q[k] * fa + bc[k] * ba * (1 - fa)) / oa);
        q[3] = U8(oa * 255.0f);
    }
}

void FlattenOn(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t bc[3] = { r, g, b };
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
    {
        const float a = img.px[i + 3] / 255.0f;
        for (int k = 0; k < 3; ++k) img.px[i + k] = U8(img.px[i + k] * a + bc[k] * (1 - a));
        img.px[i + 3] = 255;
    }
}

ImageStats MeasureImage(const RgbaImage& img)
{
    ImageStats s;
    if (img.Empty()) return s;
    for (size_t i = 3; i < img.px.size(); i += 4) s.alpha |= img.px[i] < 250;
    const ImageBounds b = AlphaBounds(img, 24);
    if (b.Empty()) return s;
    s.fillW = float(b.x1 - b.x0) / img.w;
    s.fillH = float(b.y1 - b.y0) / img.h;
    s.marginL = float(b.x0) / img.w;
    s.marginR = float(img.w - b.x1) / img.w;
    s.marginT = float(b.y0) / img.h;
    s.marginB = float(img.h - b.y1) / img.h;
    double sum = 0, sum2 = 0, sat = 0;
    size_t n = 0;
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
        if (img.px[i + 3] > 200)
        {
            const double l = img.px[i] * 0.30 + img.px[i + 1] * 0.59 + img.px[i + 2] * 0.11;
            sum += l; sum2 += l * l; ++n;
            sat += std::max({ img.px[i], img.px[i + 1], img.px[i + 2] }) - std::min({ img.px[i], img.px[i + 1], img.px[i + 2] });
        }
    if (n)
    {
        const double mean = sum / n, sd = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
        s.whiteSilhouette = mean > 200 && sd < 18 && sat / n < 20;
    }
    return s;
}
