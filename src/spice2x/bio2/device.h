#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bio2 {
    inline constexpr const char *DEVICE_ID = "BIO2:BI2X:TDJ";
    inline constexpr size_t BUTTON_COUNT = 23;
    inline constexpr size_t BI2X_LIGHT_COUNT = 84;
    inline constexpr size_t LIGHT_COUNT = 93;
    inline constexpr size_t TAPE_COUNT = 17;

    enum class Board { BI2X, BI2A, IO2 };
    bool parse_board(std::string_view name, Board &board);
    const char *board_name(Board board);
    bool button_available(Board board, size_t channel);
    bool analog_available(Board board, size_t channel);
    bool light_available(Board board, size_t channel);

    struct Status {
        bool started = false;
        bool online = false;
        std::string message;
    };

    const std::vector<std::string> &button_names();
    const std::vector<std::string> &light_names();
    const std::vector<std::string> &analog_names();
    const std::vector<std::string> &tape_names();
    bool is_device(std::string_view id);

    // The caller supplies runtime modules; they are never bundled with spice.
    bool start(const std::string &port, const std::string &module_dir, bool skip_reset = false,
        Board board = Board::BI2X);
    void stop();
    Status status();
    bool read_button(size_t channel, bool invert = false);
    float read_analog(size_t channel);
    void write_light(size_t channel, float value);
    float read_light(size_t channel);
    void write_ticker(const char *text, size_t size = 9);

    // A temporary output test preserves the game's latest output underneath it.
    void test_light(int channel);
    void set_tape_route(size_t source, size_t target);
    size_t tape_destination(size_t source);
    bool configure_tape_routes(std::string_view routes);
    void write_tape(size_t source, const uint8_t *rgb, size_t led_count);
}
