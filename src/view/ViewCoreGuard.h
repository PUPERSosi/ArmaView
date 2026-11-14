#pragma once
// Included last by every .cpp of ArmaViewViewCore (view logic and interfaces another UI builds on): no ImGui, no Win32,
// no D3D - not even through a header. The target does not link them either (CMake check).
#if defined(IMGUI_VERSION)
#error ArmaViewViewCore must not include ImGui
#endif
#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINDEF_)
#error ArmaViewViewCore must not include windows.h
#endif
#if defined(__d3d11_h__) || defined(__dxgi_h__)
#error ArmaViewViewCore must not include D3D / DXGI
#endif
