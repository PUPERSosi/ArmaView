#pragma once
// Writing the Icons mode's pictures (Win32 / WIC): PNG with its alpha, JPG for editor previews, PAA through
// ImageToPAA of the Arma 3 Tools.
#include "IconImage.h"

#include <filesystem>
#include <string>

// by the extension: .png (alpha kept) or .jpg (quality 0..1, flattened by the caller)
bool WriteImageFile(const std::filesystem::path& file, const RgbaImage& img, std::string& error, float jpgQuality = 0.92f);
// ImageToPAA.exe <png> <paa>; false + error: the tool failed or wrote nothing
bool ConvertToPaa(const std::filesystem::path& imageToPaa, const std::filesystem::path& png, const std::filesystem::path& paa, std::string& error);
// ImageToPAA.exe in the usual Arma 3 Tools folders (Steam libraries of this machine); empty if not found
std::filesystem::path FindImageToPaa();
