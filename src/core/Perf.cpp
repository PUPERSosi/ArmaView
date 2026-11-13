#include "Perf.h"

#include <algorithm>
#include <format>
#include <vector>

#include "CoreGuard.h"

Perf& Perf::Get()
{
    static Perf p;
    return p;
}

void Perf::Add(const char* section, double ms) { sums_[section] += ms; }

std::string Perf::FrameReport() const
{
    if (!frames_) return "no frames";
    std::vector<std::pair<std::string, double>> v(sums_.begin(), sums_.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string s;
    for (const auto& [k, t] : v) s += std::format("{}{} {:.3f}", s.empty() ? "" : ", ", k, t / frames_);
    return s + std::format(" ms/frame over {} frames", frames_);
}

void Perf::Note(const std::string& name, double ms) { notes_[name] += ms; }

std::string Perf::NotesReport() const
{
    std::string s;
    for (const auto& [k, t] : notes_) s += std::format("{}{} {:.0f} ms", s.empty() ? "" : ", ", k, t);
    return s;
}

PerfNoteScope::~PerfNoteScope() { Perf::Get().Note(name, std::chrono::duration<double, std::milli>(Perf::Clock::now() - t0).count()); }
