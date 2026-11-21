#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <windows.h>

namespace games::iidx {
    inline constexpr char LEGACY_SUBSCREEN_WINDOW_CLASS[] = "SpiceIIDXLegacySubscreen";
    inline constexpr UINT LEGACY_SUBSCREEN_TOPMOST_INTERVAL_MS = 500;

#ifndef SPICE_XP
    bool run_builtin_legacy_subscreen(HWND main, const RECT &rect, const std::atomic_bool &stopping,
        std::array<std::atomic<uint16_t>, 2> &keypads, std::array<std::atomic<int>, 5> &sliders);
#endif
    bool legacy_subscreen_start(const std::string &monitor);
    std::array<char, 10> legacy_subscreen_ticker();
    void legacy_subscreen_stop();
    bool is_legacy_subscreen_window(HWND window);
    void keep_legacy_subscreen_topmost(HWND window, bool show = false);
    uint16_t legacy_subscreen_keypad(size_t player);
    int legacy_subscreen_slider(size_t slider);
}
