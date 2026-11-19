#pragma once
// Checks of the render library that need a D3D11 device (the viewer's coretest runs them with its own device).
#include <d3d11.h>

#include <string>

// Texture streaming under stress: many PrefetchAsync calls while finished decodes still wait for their upload (the
// workers have already left); every texture must arrive and Streaming() must end. failures: incremented per failed check.
std::string RunTextureStreamTest(ID3D11Device* device, int* failures = nullptr);
