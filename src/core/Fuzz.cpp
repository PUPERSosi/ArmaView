#include "Fuzz.h"
#include "ArmaConfig.h"
#include "Log.h"
#include "RvMaterial.h"
#include "P3D.h"
#include "RTM.h"


#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <random>

#include "CoreGuard.h"

namespace fs = std::filesystem;

namespace {

fs::path TempFile(const char* ext)
{
    static const fs::path dir = [] {
        std::error_code ec;
        return fs::temp_directory_path(ec) / std::format("armaview_fuzz_{}", std::random_device{}());
    }();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (std::string("input") + ext);
}

bool WriteAll(const fs::path& p, const uint8_t* data, size_t size)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (size) f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    return bool(f);
}

} // namespace

const char* FuzzKindName(FuzzKind k)
{
    static const char* const kNames[] = { "p3d", "rtm", "paa", "tga", "config text", "config rap", "rvmat" };
    return int(k) >= 0 && k < FuzzKind::Count ? kNames[int(k)] : "?";
}

FuzzKind FuzzKindOf(const fs::path& file)
{
    std::wstring e = file.extension().native();
    for (auto& c : e) c = wchar_t(towlower(c));
    if (e == L".p3d") return FuzzKind::P3D;
    if (e == L".rtm") return FuzzKind::RTM;
    if (e == L".paa" || e == L".pac") return FuzzKind::PAA;
    if (e == L".tga") return FuzzKind::TGA;
    if (e == L".rvmat") return FuzzKind::Rvmat;
    if (e == L".bin") return FuzzKind::ConfigRap;
    return FuzzKind::ConfigText;
}

void FuzzOne(FuzzKind kind, const uint8_t* data, size_t size)
{
    std::vector<LogEntry> sink; // the parsers' messages: not into the log
    Log::CaptureScope scope(sink);
    try
    {
        switch (kind)
        {
        case FuzzKind::PAA:
        case FuzzKind::TGA:
        {
            const std::vector<uint8_t> v(data, data + size);
            Image img;
            std::string err;
            if (kind == FuzzKind::PAA) DecodePaa(v, img, err);
            else DecodeTga(v, img, err);
            break;
        }
        case FuzzKind::P3D:
        {
            const fs::path p = TempFile(".p3d");
            if (!WriteAll(p, data, size)) break;
            P3DModel m;
            LoadP3D(p, m);
            break;
        }
        case FuzzKind::RTM:
        {
            const fs::path p = TempFile(".rtm");
            if (!WriteAll(p, data, size)) break;
            RtmAnimation a;
            std::string err;
            std::vector<std::string> warn;
            LoadRtm(p, a, err, warn);
            break;
        }
        case FuzzKind::ConfigText:
        case FuzzKind::ConfigRap:
        {
            const fs::path p = TempFile(kind == FuzzKind::ConfigRap ? ".bin" : ".cpp");
            if (!WriteAll(p, data, size)) break;
            ArmaConfig cfg;
            cfg.Load(p, p.parent_path(), false);
            break;
        }
        case FuzzKind::Rvmat:
        {
            const fs::path p = TempFile(".rvmat");
            if (!WriteAll(p, data, size)) break;
            RvMaterial m;
            LoadRvMaterial(p, p.parent_path(), m);
            break;
        }
        default: break;
        }
    }
    catch (const std::exception& e)
    {
        // a parser that throws is a bug too (not a memory error): reported, then passed on (libFuzzer stops, FuzzRun counts)
        std::fprintf(stderr, "FuzzOne(%s): exception: %s\n", FuzzKindName(kind), e.what());
        throw;
    }
}

std::string FuzzRun(const std::vector<fs::path>& seeds, int iterations, uint32_t randomSeed, int* exceptionsOut,
                    const std::function<void()>& progress)
{
    std::mt19937 rng(randomSeed);
    const auto t0 = std::chrono::steady_clock::now();
    int runs = 0, exceptions = 0, files = 0;
    std::string firstException;
    for (const auto& s : seeds)
    {
        std::ifstream in(s, std::ios::binary);
        std::vector<uint8_t> orig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (orig.empty()) continue;
        ++files;
        const FuzzKind kind = FuzzKindOf(s);
        for (int i = 0; i < iterations; ++i)
        {
            std::vector<uint8_t> d = orig;
            // 1-8 mutations: bit flips, random bytes, 32-bit "interesting" values (sizes / counts), truncation, a duplicated run
            const int n = 1 + int(rng() % 8);
            for (int k = 0; k < n && !d.empty(); ++k)
            {
                const size_t at = rng() % d.size();
                switch (rng() % 6)
                {
                case 0: d[at] ^= uint8_t(1u << (rng() % 8)); break;
                case 1: d[at] = uint8_t(rng()); break;
                case 2:
                {
                    static const uint32_t kInteresting[] = { 0, 1, 0x7F, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x10000, 4096 };
                    const uint32_t v = kInteresting[rng() % std::size(kInteresting)];
                    if (at + 4 <= d.size()) std::memcpy(&d[at], &v, 4);
                    break;
                }
                case 3: d.resize(at); break;
                case 4:
                {
                    const size_t len = std::min<size_t>(d.size() - at, 1 + rng() % 64);
                    d.insert(d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(at + len));
                    break;
                }
                default: d.erase(d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(std::min(d.size(), at + 1 + rng() % 32))); break;
                }
            }
            try { FuzzOne(kind, d.data(), d.size()); }
            catch (const std::exception& e)
            {
                if (!exceptions++) firstException = std::format("{} ({}): {}", s.filename().string(), FuzzKindName(kind), e.what());
            }
            ++runs;
            if (progress) progress();
        }
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (exceptionsOut) *exceptionsOut = exceptions;
    return std::format("{} mutants of {} seed file(s) in {:.1f} s, {} escaped exception(s){}", runs, files, sec, exceptions,
                       firstException.empty() ? "" : " - first: " + firstException);
}
