#pragma once
// Windows file dialogs (owner: the main window).
#include <filesystem>
#include <string>

// "Name (*.a;*.b)\0*.a;*.b\0..." filter; empty path: cancelled
std::filesystem::path OpenFileDialog(void* owner, const wchar_t* filter, const std::filesystem::path& initialDir);
std::filesystem::path SaveFileDialog(void* owner, const wchar_t* filter, const wchar_t* defaultExt, const std::filesystem::path& initial);
std::filesystem::path PickFolderDialog(void* owner, const wchar_t* title); // empty: cancelled
