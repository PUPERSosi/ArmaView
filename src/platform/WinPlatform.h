#pragma once
// Installs the Win32 implementations of the core's platform hooks (ANSI code page, SUBST drives). Call once at startup,
// before any file is read.
void InstallWinPlatform();
