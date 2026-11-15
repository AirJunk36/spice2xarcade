#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace games::iidx {
    bool legacy_subscreen_start(const std::string &monitor);
    void legacy_subscreen_stop();
    uint16_t legacy_subscreen_keypad(size_t player);
    int legacy_subscreen_slider(size_t slider);
}
