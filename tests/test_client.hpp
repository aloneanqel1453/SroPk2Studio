#pragma once
// The game client folder the console tests read (Data.pk2, Media.pk2, Particles.pk2, ...): the
// SRO_CLIENT_DIR environment variable, returned with a trailing backslash. Every test that needs it
// stops with a clear message when it is not set.
//   set SRO_CLIENT_DIR=D:\Games\Silkroad
#include <cstdio>
#include <cstdlib>
#include <string>

inline std::wstring test_client_dir() {
    wchar_t* v = nullptr;
    std::size_t n = 0;
    if (_wdupenv_s(&v, &n, L"SRO_CLIENT_DIR") != 0 || !v) return {};
    std::wstring dir(v);
    std::free(v);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir;
}

// test_client_dir(), or exits with a usage note when SRO_CLIENT_DIR is not set.
inline std::wstring require_client_dir() {
    std::wstring dir = test_client_dir();
    if (dir.empty()) {
        std::fputs("Set SRO_CLIENT_DIR to the game client folder (the one with Data.pk2 / Media.pk2), "
                   "e.g.  set SRO_CLIENT_DIR=D:\\Games\\Silkroad\n", stderr);
        std::exit(2);
    }
    return dir;
}
