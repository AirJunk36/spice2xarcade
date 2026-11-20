#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <windows.h>

namespace games::iidx {
    void legacy_subscreen_start(const std::string &monitor, const std::string &path);
    std::array<char, 10> legacy_subscreen_ticker();
    void legacy_subscreen_stop();
    bool is_legacy_subscreen_window(HWND window);
    uint16_t legacy_subscreen_keypad(size_t player);
    int legacy_subscreen_slider(size_t slider);
}
