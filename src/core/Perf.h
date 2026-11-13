#pragma once
// Performance counters: named sections accumulated over a window (frame parts), one-shot timings (startup, loading)
// (memory: PerfMemory.h, platform). Cheap enough to stay on; read by --selftest and ARMAVIEW_PROFILE.
#include <chrono>
#include <map>
#include <string>

class Perf
{
public:
    static Perf& Get();
    using Clock = std::chrono::steady_clock;

    void Add(const char* section, double ms);          // per-frame part (averaged over EndFrame calls)
    void EndFrame() { ++frames_; }
    std::string FrameReport() const;                    // "section avg ms" sorted, since the last Reset
    void ResetFrames() { sums_.clear(); frames_ = 0; }

    void Note(const std::string& name, double ms);      // one-shot (startup, load parts)
    std::string NotesReport() const;
    void ClearNotes() { notes_.clear(); }


    struct Scope // times a block into a frame section
    {
        const char* name;
        Clock::time_point t0 = Clock::now();
        explicit Scope(const char* n) : name(n) {}
        ~Scope() { Get().Add(name, std::chrono::duration<double, std::milli>(Clock::now() - t0).count()); }
    };

private:
    std::map<std::string, double> sums_;
    int frames_ = 0;
    std::map<std::string, double> notes_;
};

// Accumulates into a one-shot note (e.g. all texture decodes of a load)
struct PerfNoteScope
{
    std::string name;
    Perf::Clock::time_point t0 = Perf::Clock::now();
    explicit PerfNoteScope(std::string n) : name(std::move(n)) {}
    ~PerfNoteScope();
};
