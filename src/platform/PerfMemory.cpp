// Process RAM and adapter VRAM for the perf reports (Win32 / DXGI: not in the core).
#include "PerfMemory.h"

#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <format>

std::string MemoryReport(IDXGIAdapter* adapter)
{
    std::string s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        s = std::format("RAM working set {:.0f} MB, private {:.0f} MB", pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0);
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
    if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a3))))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi)))
            s += std::format(", VRAM {:.0f} MB", vi.CurrentUsage / 1048576.0);
    }
    return s;
}
