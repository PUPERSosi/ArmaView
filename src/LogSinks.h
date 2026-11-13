#pragma once
// The viewer's log sinks: ArmaView.log next to the exe and the entries of the ImGui "Log" window.
#include "Log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// Every entry. Warnings and errors are flushed at once (a crash keeps them); debug / info lines - thousands per model
// load, one disk flush each cost more than the load - are flushed at most every 200 ms and by Flush (every frame).
class FileLogSink : public ILogSink
{
public:
    explicit FileLogSink(const std::filesystem::path& path) : file_(path, std::ios::out | std::ios::trunc) {}
    ~FileLogSink() { Flush(); }
    void Write(LogLevel level, const std::string& text) override
    {
        static constexpr const char* tags[] = { "[dbg] ", "[inf] ", "[WRN] ", "[ERR] " };
        std::lock_guard lock(mutex_);
        if (!file_.is_open()) return;
        file_ << tags[static_cast<int>(level)] << text << '\n';
        const auto now = std::chrono::steady_clock::now();
        if (level >= LogLevel::Warn || now - lastFlush_ > std::chrono::milliseconds(200))
        {
            file_.flush();
            lastFlush_ = now;
        }
    }
    void Flush()
    {
        std::lock_guard lock(mutex_);
        if (file_.is_open()) file_.flush();
        lastFlush_ = std::chrono::steady_clock::now();
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point lastFlush_ = std::chrono::steady_clock::now();
};

// Entries shown by the Log window. Constructed on the UI thread, which owns Entries(); other threads (background
// loads) queue into pending_ until the next Flush on the UI thread.
class LogWindowSink : public ILogSink
{
public:
    void Write(LogLevel level, const std::string& text) override
    {
        std::lock_guard lock(mutex_);
        if (std::this_thread::get_id() != uiThread_) pending_.push_back({ level, text });
        else entries_.push_back({ level, text });
    }
    void Flush() // UI thread, once per frame
    {
        std::lock_guard lock(mutex_);
        for (auto& e : pending_) entries_.push_back(std::move(e));
        pending_.clear();
    }
    void Clear()
    {
        std::lock_guard lock(mutex_);
        entries_.clear();
        pending_.clear();
    }
    const std::vector<LogEntry>& Entries() const { return entries_; }

private:
    std::vector<LogEntry> entries_, pending_;
    std::mutex mutex_;
    std::thread::id uiThread_ = std::this_thread::get_id();
};

