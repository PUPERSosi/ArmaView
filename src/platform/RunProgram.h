#pragma once
// Starting another program (Win32). wait: hidden, up to two minutes, stdout + stderr returned in output;
// otherwise opened normally (ShellExecute) and left running.
#include <string>

bool RunProgram(const std::wstring& exe, const std::wstring& args, bool wait, std::string& output);
