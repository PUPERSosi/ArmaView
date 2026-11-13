#pragma once
// Command palette (Ctrl+Shift+P): every registered command, filtered by the typed words in both languages; Enter runs.
#include "Commands.h"

#include <functional>
#include <vector>

class CommandPalette
{
public:
    CommandPalette(const std::vector<Hotkey>& commands, std::function<bool()> hasModel)
        : hotkeys_(commands), hasModel_(std::move(hasModel)) {}
    void Draw(float dpi);
    const char* Filter() const { return filter_; }

    bool open = false;
    bool focus = false; // give the filter the keyboard on the next frame

private:
    const std::vector<Hotkey>& hotkeys_;
    std::function<bool()> hasModel_; // commands that need a model are greyed out without one
    char filter_[128] = {};
    int sel_ = 0;
};
