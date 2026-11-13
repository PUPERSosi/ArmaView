#pragma once
// Application log: one global access point (Log::Get) behind an interface - messages go to the registered ILogSinks
// (UI: the Log window and ArmaView.log, CLI: the console, tests: MemoryLogSink). The log is not passed through
// constructors. Substitution:
//   - ScopedLogSink: a sink for its own lifetime (receives every thread's entries);
//   - Log::CaptureScope / LogCapture: this thread's entries go into a vector instead of the sinks and are not counted
//     (Mod check reports, fuzzing, tests asserting on the messages). Workers a captured thread starts capture their
//     own entries and hand them back (see LoadP3D: replayed in order on the calling thread).
// The string helpers (ANSI/UTF-8/path, case-insensitive names) are in Text.h, included here for every user.
#include "Text.h"

#include <format>
#include <mutex>
#include <string>
#include <algorithm>
#include <vector>

enum class LogLevel { Debug, Info, Warn, Error };

struct LogEntry
{
    LogLevel level;
    std::string text;
};

// Where log entries go. Write is called under the log's lock, from any thread (background loaders log too).
class ILogSink
{
public:
    virtual ~ILogSink() = default;
    virtual void Write(LogLevel level, const std::string& text) = 0;
};

class Log
{
public:
    // TODO(migration step 12): the one process log; injected into the modules instead once they are objects.
    static Log& Get()
    {
        static Log instance;
        return instance;
    }

    // Sinks are not owned; a sink removes itself before it is destroyed.
    void AddSink(ILogSink* sink) { std::lock_guard lock(mutex_); sinks_.push_back(sink); }
    void RemoveSink(ILogSink* sink) { std::lock_guard lock(mutex_); std::erase(sinks_, sink); }

    // While a CaptureScope lives on a thread, that thread's entries go into its vector instead of the log (Mod check: the
    // parsers' messages become the report's problems instead of flooding the log).
    struct CaptureScope
    {
        explicit CaptureScope(std::vector<LogEntry>& to) : prev_(capture_) { capture_ = &to; }
        ~CaptureScope() { capture_ = prev_; }
        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;
    private:
        std::vector<LogEntry>* prev_;
    };

    void Add(LogLevel level, std::string text)
    {
        if (capture_) { capture_->push_back({ level, std::move(text) }); return; }
        std::lock_guard lock(mutex_);
        if (level == LogLevel::Warn) ++warnings_;
        if (level == LogLevel::Error) ++errors_;
        for (ILogSink* s : sinks_) s->Write(level, text);
    }

    template <class... A> void Debug(std::format_string<A...> f, A&&... a) { Add(LogLevel::Debug, std::format(f, std::forward<A>(a)...)); }
    template <class... A> void Info (std::format_string<A...> f, A&&... a) { Add(LogLevel::Info,  std::format(f, std::forward<A>(a)...)); }
    template <class... A> void Warn (std::format_string<A...> f, A&&... a) { Add(LogLevel::Warn,  std::format(f, std::forward<A>(a)...)); }
    template <class... A> void Error(std::format_string<A...> f, A&&... a) { Add(LogLevel::Error, std::format(f, std::forward<A>(a)...)); }

    int Warnings() const { return warnings_; }
    int Errors() const { return errors_; }
    void ResetCounts() { std::lock_guard lock(mutex_); warnings_ = errors_ = 0; }

private:
    std::vector<ILogSink*> sinks_;
    static inline thread_local std::vector<LogEntry>* capture_ = nullptr;
    std::mutex mutex_;
    int warnings_ = 0, errors_ = 0;
};

// Registers a sink for its own lifetime (declare it after the sink: it is removed before the sink is destroyed).
class ScopedLogSink
{
public:
    ScopedLogSink() = default;
    explicit ScopedLogSink(ILogSink* sink) : sink_(sink) { if (sink_) Log::Get().AddSink(sink_); }
    ~ScopedLogSink() { if (sink_) Log::Get().RemoveSink(sink_); }
    ScopedLogSink(const ScopedLogSink&) = delete;
    ScopedLogSink& operator=(const ScopedLogSink&) = delete;
    ScopedLogSink& operator=(ScopedLogSink&& o) noexcept
    {
        if (this != &o) { if (sink_) Log::Get().RemoveSink(sink_); sink_ = o.sink_; o.sink_ = nullptr; }
        return *this;
    }

private:
    ILogSink* sink_ = nullptr;
};

// Keeps every entry in memory (tests: what was logged, from any thread).
class MemoryLogSink : public ILogSink
{
public:
    void Write(LogLevel level, const std::string& text) override
    {
        std::lock_guard lock(mutex_);
        entries_.push_back({ level, text });
    }
    std::vector<LogEntry> Entries() const
    {
        std::lock_guard lock(mutex_);
        return entries_;
    }

private:
    std::vector<LogEntry> entries_;
    mutable std::mutex mutex_;
};

// This thread's entries while it lives: kept here, not written to the sinks, not counted (tests assert on them).
struct LogCapture
{
    std::vector<LogEntry> entries;
    int Count(LogLevel level) const { return int(std::count_if(entries.begin(), entries.end(), [&](const LogEntry& e) { return e.level == level; })); }
    bool Has(LogLevel level, std::string_view part) const
    {
        return std::any_of(entries.begin(), entries.end(), [&](const LogEntry& e) { return e.level == level && e.text.find(part) != std::string::npos; });
    }

private:
    Log::CaptureScope scope_{ entries };
};
