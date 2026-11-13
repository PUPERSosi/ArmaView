#pragma once
// Included last by every .cpp of ArmaViewCore: the core must not see ImGui, Win32 or D3D (also not through a header
// it includes). The core target does not link them either; this catches windows.h / d3d11.h from the SDK.
#if defined(IMGUI_VERSION)
#error ArmaViewCore must not include ImGui
#endif
#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINDEF_)
#error ArmaViewCore must not include windows.h
#endif
#if defined(__d3d11_h__) || defined(__dxgi_h__)
#error ArmaViewCore must not include D3D / DXGI
#endif
