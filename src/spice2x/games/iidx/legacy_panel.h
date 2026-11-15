#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace games::iidx::legacy_panel {
    inline constexpr int WIDTH = 1280;
    inline constexpr int HEIGHT = 720;
    inline constexpr size_t CONTROL_COUNT = 31;
    inline constexpr std::array<const char *, 13> LABELS {
        "7", "8", "9", "4", "5", "6", "1", "2", "3", "0", "00", ".", "Insert Card",
    };
    // EAM_IO scan codes; separate bitfields are maintained for each player.
    inline constexpr std::array<unsigned int, 13> SCAN_CODES {3, 7, 11, 2, 6, 10,
        1, 5, 9, 0, 4, 8, 13};
    inline constexpr std::array<const char *, 5> SLIDER_NAMES {
        "VEFX", "Low-EQ", "Hi-EQ", "Filter", "Volume",
    };

    struct Rect {
        int left, top, right, bottom;
        bool contains(int x, int y) const {
            return left <= x && x < right && top <= y && y < bottom;
        }
    };

    inline Rect bounds(size_t control) {
        if (control >= 26) {
            const int x = 450 + static_cast<int>(control - 26) * 80;
            return {x, 260, x + 60, 620};
        }
        const int player = static_cast<int>(control / 13);
        const int key = static_cast<int>(control % 13);
        const int x = player == 0 ? 55 : 905;
        if (key == 12) {
            return {x, 615, x + 320, 685};
        }
        const int left = x + (key % 3) * 110;
        const int top = 245 + (key / 3) * 90;
        return {left, top, left + 100, top + 80};
    }

    class State {
    public:
        std::array<uint16_t, 2> keys {};
        std::array<int, 5> sliders {-1, -1, -1, -1, -1};

        void touch(uint32_t id, int x, int y, bool down, bool up) {
            if (up) {
                contacts.erase(id);
            } else {
                if (down) {
                    contacts.erase(id);
                    for (size_t i = 0; i < CONTROL_COUNT; i++) {
                        if (bounds(i).contains(x, y)) {
                            contacts.emplace(id, Contact {i, x, y});
                            break;
                        }
                    }
                }
                const auto found = contacts.find(id);
                if (found != contacts.end()) {
                    auto &point = found->second;
                    point.x = x;
                    point.y = y;
                    if (point.control >= 26) {
                        const auto rect = bounds(point.control);
                        sliders[point.control - 26] = std::clamp(
                            (rect.bottom - 1 - y) * 15 / (rect.bottom - rect.top - 1), 0, 15);
                    }
                }
            }
            keys.fill(0);
            for (const auto &[contact_id, point] : contacts) {
                if (point.control < 26 && bounds(point.control).contains(point.x, point.y)) {
                    keys[point.control / 13] |= 1u << SCAN_CODES[point.control % 13];
                }
            }
        }

        void release() {
            contacts.clear();
            keys.fill(0);
        }
    private:
        struct Contact { size_t control; int x; int y; };
        std::map<uint32_t, Contact> contacts;
    };
}
