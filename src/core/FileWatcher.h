#pragma once
// Hot reload: polls the files the viewer loaded (P3D, model.cfg + includes, config.cpp + includes, pivots, RTM, RVMAT,
// textures). A change is reported once the file has been stable for `debounce` seconds and can be opened without another
// process holding it for writing (Blender / Object Builder do not write instantly). A failed reload is retried.
// Files are looked at through an IFileSystem (the platform one tells "still being written").
#include "FileSystem.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

class FileWatcher
{
public:
    explicit FileWatcher(const IFileSystem& fs) : fs_(fs) {}

    enum class Kind { Model, ModelCfg, ConfigCpp, Pivots, Rtm, Rvmat, Texture, Attachment };

    struct Event
    {
        std::filesystem::path path;
        Kind kind;
        int attempt = 0; // 0 = first try
    };

    double debounce = 0.3;      // seconds the file must stay unchanged
    double retryDelay = 0.5;    // seconds between retries of a failed reload
    int maxRetries = 20;
    bool enabled = true;

    // Replaces the watch list; files already watched keep their stamps (no false "changed" on rebuild of the list).
    void Set(const std::vector<std::pair<std::filesystem::path, Kind>>& files)
    {
        std::vector<Entry> next;
        for (const auto& [p, k] : files)
        {
            if (p.empty()) continue;
            bool dup = false;
            for (const auto& e : next) dup |= e.path == p;
            if (dup) continue;
            Entry e;
            e.path = p;
            e.kind = k;
            bool found = false;
            for (const auto& old : entries_)
                if (old.path == p) { e = old; e.kind = k; found = true; break; }
            if (!found) e.stamp = fs_.Stamp(e.path);
            next.push_back(e);
        }
        entries_ = std::move(next);
    }

    size_t Count() const { return entries_.size(); }

    // The file was just (re)loaded explicitly: take its current stamp and drop any pending change.
    void Restamp(const std::filesystem::path& p)
    {
        std::error_code ec;
        for (auto& e : entries_)
            if (std::filesystem::equivalent(e.path, p, ec))
            {
                e.stamp = fs_.Stamp(e.path);
                e.changedAt = -1.0;
                e.attempt = 0;
            }
    }

    // Call every frame with a monotonic clock in seconds.
    std::vector<Event> Poll(double now)
    {
        std::vector<Event> out;
        if (!enabled) return out;
        if (now - lastScan_ >= 0.25)
        {
            lastScan_ = now;
            for (auto& e : entries_)
            {
                const FileStamp now2 = fs_.Stamp(e.path);
                if (now2 != e.stamp)
                {
                    e.stamp = now2;
                    e.changedAt = now; // (re)start debounce
                    e.attempt = 0;
                }
            }
        }
        for (auto& e : entries_)
        {
            if (e.changedAt < 0.0 || now - e.changedAt < (e.attempt ? retryDelay : debounce)) continue;
            if (!e.stamp.exists) continue;                        // deleted / mid-rename: wait for it to come back
            if (fs_.IsWriteLocked(e.path)) { e.changedAt = now - (e.attempt ? retryDelay : debounce) + 0.1; continue; }
            out.push_back({ e.path, e.kind, e.attempt });
            e.changedAt = -1.0;
        }
        return out;
    }

    // The reload of `ev` failed (file half-written, parse error): try again later. Returns false when giving up.
    bool Retry(const Event& ev, double now)
    {
        for (auto& e : entries_)
            if (e.path == ev.path)
            {
                if (ev.attempt + 1 > maxRetries) return false;
                e.attempt = ev.attempt + 1;
                e.changedAt = now;
                return true;
            }
        return false;
    }

private:
    struct Entry
    {
        std::filesystem::path path;
        Kind kind = Kind::Model;
        FileStamp stamp;
        double changedAt = -1.0;
        int attempt = 0;
    };

    const IFileSystem& fs_;
    std::vector<Entry> entries_;
    double lastScan_ = -1.0;
};
