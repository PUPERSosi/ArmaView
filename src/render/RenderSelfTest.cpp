#include "RenderSelfTest.h"
#include "Material.h"

#include <chrono>
#include <format>
#include <thread>

std::string RunTextureStreamTest(ID3D11Device* device, int* failures)
{
    std::string out;
    int failed = 0;
    auto check = [&](bool ok, const std::string& what) {
        out += std::format("\n    {} {}", ok ? "ok  " : "FAIL", what);
        failed += ok ? 0 : 1;
    };
    if (!device)
    {
        check(false, "texture streaming: no D3D device");
        if (failures) *failures += failed;
        return out;
    }
    // procedural textures decode without files: unique names per round
    auto names = [](int round, int part, int n) {
        std::vector<std::string> v;
        for (int i = 0; i < n; ++i)
            v.push_back(std::format("#(argb,8,8,3)color({:.3f},{:.3f},{:.3f},1)", (round % 97) / 97.0, part * 0.5 + i / 200.0, (i * 7 % 101) / 101.0));
        return v;
    };
    TextureCache tc;
    tc.Init(device, {});
    int stuck = 0, rounds = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < 12; ++round, ++rounds)
    {
        // a first batch, all decoded and the workers gone (but not uploaded: more than one frame's 16), a second batch then
        tc.PrefetchAsync(names(round, 0, 40));
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 3 == 0 ? 150 : round % 3 == 1 ? 5 : 0));
        tc.PollStreamed(16);
        tc.PrefetchAsync(names(round, 1, 40));
        tc.PollStreamed(16);
        tc.PrefetchAsync(names(round, 2, 3)); // and once more mid-upload
        const auto start = std::chrono::steady_clock::now();
        while (tc.Streaming() && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        {
            tc.PollStreamed(16);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (tc.Streaming()) { ++stuck; break; }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(stuck == 0 && tc.Count() == size_t(rounds) * 83u,
          std::format("texture streaming: {} rounds of PrefetchAsync while uploads wait, {} texture(s) arrived, streaming {} ({:.0f} ms)", rounds, tc.Count(),
                      stuck ? "STUCK" : "ended", ms));
    if (failures) *failures += failed;
    return out;
}
