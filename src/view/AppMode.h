#pragma once
// The viewer's modes (Model, Skeleton & Anim, ...): what the view does differs per mode (weapon scene, recorder).
// Only the enum and the mode names - no dependencies; the UI shell (mode tabs) and any other UI use it.

enum class AppMode { Model, Animation, Vehicle, Materials, Validation, Reload, Optics, Count };

inline const char* AppModeName(AppMode m)
{
    static const char* const names[] = { "Model", "Skeleton & Anim", "Vehicle & Crew", "Materials", "Validation", "Reload", "Optics" };
    return m < AppMode::Count ? names[int(m)] : "?";
}
