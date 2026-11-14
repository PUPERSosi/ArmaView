#pragma once
// Checks of the view logic without any UI (armaview-cli selftest and the viewer's coretest run them after the core's):
// SceneInteraction on synthetic InputStates (Q turns left, W walks forward, a click picks, bone selection, drag orbits),
// PointOverlaySettings through the settings file. Files only in dir.
#include <filesystem>
#include <string>

std::string RunViewCoreTest(const std::filesystem::path& dir, int* failures = nullptr);
