#pragma once
// The Log window: the entries of the LogWindowSink, filtered by level, auto-scroll, clear / copy.
// Panels show state and send commands; this one only needs the log entries (constructor dependency).
#include "LogSinks.h"

#include <string>

class LogPanel
{
public:
    explicit LogPanel(LogWindowSink& entries) : entries_(entries) {}
    void Draw(const std::string& windowName); // windowName: the translated title with its stable "###" id

    bool showLevel[4] = { false, true, true, true }; // Debug, Info, Warn, Error
    bool autoScroll = true;

private:
    LogWindowSink& entries_;
};
