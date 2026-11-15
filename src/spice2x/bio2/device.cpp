#include "device.h"
#include "legacy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <mutex>
#include <thread>

extern "C" {
#include "external/bi2x/bi2x_tdj.h"
#include "external/bi2x/bi2x_log.h"
}

#if defined(_WIN32) && !defined(BIO2_STANDALONE_TEST)
#include "util/logging.h"
#endif

namespace bio2 {
    bool parse_board(std::string_view name, Board &board) {
        if (name.empty() || name == "bi2x") {
            board = Board::BI2X;
        } else if (name == "bi2a") {
            board = Board::BI2A;
        } else if (name == "io2") {
            board = Board::IO2;
        } else {
            return false;
        }
        return true;
    }

    const char *board_name(Board board) {
        switch (board) {
            case Board::BI2A: return "BI2A";
            case Board::IO2: return "IO2 / EZUSB FX2";
            default: return "BI2X / Lightning Model";
        }
    }

    bool button_available(Board board, size_t channel) {
        return channel < (board == Board::BI2X ? BUTTON_COUNT : 21);
    }

    bool analog_available(Board board, size_t channel) {
        return channel < (board == Board::BI2X ? 2u : 7u);
    }

    bool light_available(Board board, size_t channel) {
        return board == Board::BI2X ? channel < BI2X_LIGHT_COUNT
            : channel < 18 || (BI2X_LIGHT_COUNT <= channel && channel < LIGHT_COUNT);
    }

    const std::vector<std::string> &analog_names() {
        static const std::vector<std::string> names {
            "Turntable P1", "Turntable P2", "VEFX", "Low-EQ", "Hi-EQ", "Filter", "Play Volume",
        };
        return names;
    }

    const std::vector<std::string> &button_names() {
        static const std::vector<std::string> names {
            "Test", "Service", "Coin Mech", "P1 Start", "P2 Start", "VEFX", "EFFECT",
            "P1 1", "P1 2", "P1 3", "P1 4", "P1 5", "P1 6", "P1 7",
            "P2 1", "P2 2", "P2 3", "P2 4", "P2 5", "P2 6", "P2 7",
            "P1 Headphone", "P2 Headphone",
        };
        return names;
    }

    const std::vector<std::string> &tape_names() {
        static const std::vector<std::string> names {
            "Stage Left", "Stage Right", "Cabinet Left", "Cabinet Right",
            "Control Panel Under", "Ceiling Left", "Title Left", "Title Right",
            "Ceiling Right", "Touch Panel Left", "Touch Panel Right",
            "Side Panel Left Inner", "Side Panel Left Outer", "Side Panel Left",
            "Side Panel Right Outer", "Side Panel Right Inner", "Side Panel Right",
        };
        return names;
    }

    const std::vector<std::string> &light_names() {
        static const auto names = [] {
            std::vector<std::string> result {
                "P1 1", "P1 2", "P1 3", "P1 4", "P1 5", "P1 6", "P1 7",
                "P2 1", "P2 2", "P2 3", "P2 4", "P2 5", "P2 6", "P2 7",
                "P1 Start", "P2 Start", "VEFX", "Effect",
            };
            for (const auto *name : {"Woofer", "IC Card Reader P1", "IC Card Reader P2",
                    "Turntable P1", "Turntable P2"}) {
                for (const auto *color : {" R", " G", " B"}) {
                    result.emplace_back(std::string(name) + color);
                }
            }
            for (const auto &name : tape_names()) {
                for (const auto *color : {" Avg R", " Avg G", " Avg B"}) {
                    result.emplace_back(name + color);
                }
            }
            for (size_t i = 1; i <= 8; i++) {
                result.emplace_back("Spot Light " + std::to_string(i));
            }
            result.emplace_back("Neon Lamp");
            return result;
        }();
        return names;
    }

    bool is_device(std::string_view id) {
        return id == DEVICE_ID;
    }

    namespace {
        uint8_t brightness(float value) {
            return static_cast<uint8_t>(std::lround(value * 255.f));
        }

        class Backend {
        public:
            std::mutex lifecycle;
            std::mutex mutex;
            std::atomic<bool> running {false};
            std::thread worker;
            bi2x_tdj *device = nullptr;
            std::unique_ptr<LegacyBoard> legacy;
            Board board = Board::BI2X;
            std::string port;
            bool skip_reset = false;
            Status state;
            std::array<bool, BUTTON_COUNT> buttons {};
            std::array<float, 7> analogs {};
            std::array<float, LIGHT_COUNT> lights {};
            std::array<bool, LIGHT_COUNT> dirty {};
            std::array<std::vector<uint8_t>, TAPE_COUNT> tapes;
            std::array<bool, TAPE_COUNT> tape_dirty {};
            std::array<size_t, TAPE_COUNT> tape_routes {};
            int test = -1;
            LegacyOutput legacy_output;

            Backend() {
                for (size_t i = 0; i < TAPE_COUNT; i++) {
                    tape_routes[i] = i;
                    tapes[i].resize(bi2x_tdj_tape_led_count(i) * 3);
                }
            }

            ~Backend() {
                shutdown();
            }

            void shutdown() {
                running = false;
                if (worker.joinable()) {
                    worker.join();
                }
                if (legacy != nullptr) {
                    LegacyInput unused;
                    // Best-effort blackout; this exchange has a bounded timeout.
                    legacy->poll(LegacyOutput {}, unused);
                    legacy.reset();
                }
                if (device != nullptr && bi2x_tdj_is_online(device)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        // Leave the cabinet lamps off when disconnecting. Runtime module
                        // state is volatile; no persistent firmware is modified here.
                        lights.fill(0.f);
                        test = -1;
                        for (size_t channel = 0; channel < 33; channel++) {
                            apply_light(channel);
                        }
                        for (size_t section = 0; section < TAPE_COUNT; section++) {
                            std::fill(tapes[section].begin(), tapes[section].end(), 0);
                            bi2x_tdj_set_tape_led_data(device, section, tapes[section].data());
                        }
                        bi2x_tdj_update(device);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                bi2x_tdj_close(device);
                device = nullptr;
                std::lock_guard<std::mutex> lock(mutex);
                state = {};
                buttons.fill(false);
                analogs.fill(0.f);
                lights.fill(0.f);
                dirty.fill(false);
                tape_dirty.fill(false);
                for (auto &tape : tapes) {
                    std::fill(tape.begin(), tape.end(), 0);
                }
                test = -1;
                legacy_output = {};
            }

            void apply_light(size_t channel) {
                const auto value = test == static_cast<int>(channel) ? 1.f : lights[channel];
                if (channel < 14) {
                    bi2x_tdj_set_player_button_lamp(device, channel / 7, channel % 7, value > 0.f);
                } else if (channel < 16) {
                    bi2x_tdj_set_start_lamp(device, channel - 14, value > 0.f);
                } else if (channel == 16) {
                    bi2x_tdj_set_vefx_button_lamp(device, value > 0.f);
                } else if (channel == 17) {
                    bi2x_tdj_set_effect_button_lamp(device, value > 0.f);
                } else if (channel < 33) {
                    const size_t group = (channel - 18) / 3;
                    uint32_t color = 0;
                    for (size_t i = 0; i < 3; i++) {
                        const size_t index = 18 + group * 3 + i;
                        color = (color << 8) | brightness(
                            test == static_cast<int>(index) ? 1.f : lights[index]);
                    }
                    if (group == 0) {
                        bi2x_tdj_set_woofer_led(device, color);
                    } else if (group < 3) {
                        bi2x_tdj_set_iccr_led(device, group - 1, color);
                    } else {
                        bi2x_tdj_set_turntable_led(device, group - 3, color);
                    }
                } else if (channel < BI2X_LIGHT_COUNT) {
                    const size_t section = (channel - 33) / 3;
                    const size_t color = (channel - 33) % 3;
                    for (size_t led = 0; led < tapes[section].size() / 3; led++) {
                        tapes[section][led * 3 + color] = brightness(lights[channel]);
                    }
                    tape_dirty[section] = true;
                }
            }

            void poll_legacy() {
                bool connected = false;
                while (running) {
                    const bool previous = connected;
                    if (!connected) {
                        connected = legacy->connect(port, skip_reset, running);
                    }
                    // Stopping is not a transport failure. Keep the initialized
                    // connection open so shutdown can send the final blackout.
                    if (!running) {
                        break;
                    }
                    LegacyOutput output;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        output = legacy_output;
                        for (size_t i = 0; i < output.lamps.size(); i++) {
                            const auto channel = i < 18 ? i : BI2X_LIGHT_COUNT + i - 18;
                            output.lamps[i] = test == static_cast<int>(channel)
                                || lights[channel] > 0.f;
                        }
                    }
                    LegacyInput input;
                    connected = connected && legacy->poll(output, input);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.online = connected;
                        buttons = connected ? input.buttons : decltype(buttons) {};
                        analogs = connected ? input.analogs : decltype(analogs) {};
                        state.message = connected ? "" : legacy->error();
                    }
#if defined(_WIN32) && !defined(BIO2_STANDALONE_TEST)
                    if (connected != previous) {
                        if (connected) {
                            log_info("bio2", "{} connected", board_name(board));
                        } else {
                            log_warning("bio2", "{} disconnected; retrying", board_name(board));
                        }
                    }
#else
                    (void) previous;
#endif
                    if (!connected) {
                        legacy->close();
                        for (size_t i = 0; i < 100 && running; i++) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }

            void poll() {
                while (running) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.online = bi2x_tdj_is_online(device) != 0;
                        bi2x_tdj_input input {};
                        if (state.online) {
                            bi2x_tdj_get_input(device, &input);
                        }
                        buttons[0] = input.test;
                        buttons[1] = input.service;
                        buttons[2] = input.coin;
                        buttons[3] = input.start[0];
                        buttons[4] = input.start[1];
                        buttons[5] = input.vefx;
                        buttons[6] = input.effect;
                        for (size_t player = 0; player < 2; player++) {
                            for (size_t key = 0; key < 7; key++) {
                                buttons[7 + player * 7 + key] = (input.keys[player] & (1 << key)) != 0;
                            }
                            analogs[player] = input.turntable[player] / 255.f;
                            buttons[21 + player] = (input.raw[2] & (1 << (4 + player))) != 0;
                        }
                        for (size_t channel = 0; channel < BI2X_LIGHT_COUNT; channel++) {
                            if (dirty[channel]) {
                                apply_light(channel);
                                dirty[channel] = false;
                            }
                        }
                        for (size_t section = 0; section < TAPE_COUNT; section++) {
                            if (tape_dirty[section]) {
                                const uint8_t *output = tapes[section].data();
                                std::array<uint8_t, 68 * 3> overlay;
                                if (test >= 33 && (test - 33) / 3 == static_cast<int>(section)) {
                                    std::copy(tapes[section].begin(), tapes[section].end(), overlay.begin());
                                    for (size_t led = 0; led < tapes[section].size() / 3; led++) {
                                        overlay[led * 3 + (test - 33) % 3] = 255;
                                    }
                                    output = overlay.data();
                                }
                                bi2x_tdj_set_tape_led_data(device, section, output);
                                tape_dirty[section] = false;
                            }
                        }
                    }
                    bi2x_tdj_update(device);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        };

        Backend &backend() {
            static Backend value;
            return value;
        }

        void log_message(int level, const char *message) {
            if (level >= BI2X_LOG_WARNING) {
                auto &b = backend();
                std::lock_guard<std::mutex> lock(b.mutex);
                b.state.message = message;
            }
#if defined(_WIN32) && !defined(BIO2_STANDALONE_TEST)
            if (level >= BI2X_LOG_WARNING) {
                log_warning("bio2", "{}", message);
            } else {
                log_info("bio2", "{}", message);
            }
#endif
        }
    }

    bool start(const std::string &port, const std::string &module_dir, bool skip_reset,
            Board board) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.lifecycle);
        b.shutdown();
        {
            std::lock_guard<std::mutex> state_lock(b.mutex);
            b.board = board;
        }
        if (board != Board::BI2X) {
            b.port = port;
            b.skip_reset = skip_reset;
            b.legacy = std::make_unique<LegacyBoard>(board);
            {
                std::lock_guard<std::mutex> state_lock(b.mutex);
                b.state.started = true;
            }
            b.running = true;
            b.worker = std::thread([&b] { b.poll_legacy(); });
            return true;
        }
        bi2x_log_set_sink(log_message);
        bi2x_log_set_level(BI2X_LOG_INFO);
        std::string directory = module_dir;
        if (!directory.empty() && directory.back() != '/' && directory.back() != '\\') {
            directory += '/';
        }
        const char *dirs[] = {directory.c_str()};
        bi2x_tdj_config config {};
        config.port = port.c_str();
        config.module_dirs = directory.empty() ? nullptr : dirs;
        config.module_dir_count = directory.empty() ? 0 : 1;
        config.skip_reset = skip_reset;
        config.poll_interval_ms = 1;
        if (bi2x_tdj_open(&config, &b.device) < 0) {
            std::lock_guard<std::mutex> state_lock(b.mutex);
            if (b.state.message.empty()) {
                b.state.message = "Could not load BI2X runtime modules from the selected directory.";
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> state_lock(b.mutex);
            b.state.started = true;
            b.dirty.fill(true);
        }
        b.running = true;
        b.worker = std::thread([&b] { b.poll(); });
        return true;
    }

    void stop() {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.lifecycle);
        b.shutdown();
    }

    Status status() {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        return b.state;
    }

    bool read_button(size_t channel, bool invert) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        return button_available(b.board, channel) && b.state.online
            && (b.buttons[channel] != invert);
    }

    float read_analog(size_t channel) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        return analog_available(b.board, channel) && b.state.online ? b.analogs[channel] : 0.f;
    }

    void write_light(size_t channel, float value) {
        if (channel >= LIGHT_COUNT || !std::isfinite(value)) {
            return;
        }
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        value = std::clamp(value, 0.f, 1.f);
        if (b.lights[channel] != value) {
            b.lights[channel] = value;
            b.dirty[channel] = true;
        }
    }

    float read_light(size_t channel) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        return channel < LIGHT_COUNT ? b.lights[channel] : 0.f;
    }

    void write_ticker(const char *text, size_t size) {
        if (text == nullptr) {
            return;
        }
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        b.legacy_output.ticker.fill(' ');
        std::copy_n(text, std::min(size, b.legacy_output.ticker.size()),
            b.legacy_output.ticker.begin());
    }

    void test_light(int channel) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        channel = channel >= 0 && channel < static_cast<int>(LIGHT_COUNT) ? channel : -1;
        if (b.test == channel) {
            return;
        }
        if (b.test >= 0) {
            if (b.test < 33) {
                b.dirty[b.test] = true;
            } else if (b.test < static_cast<int>(BI2X_LIGHT_COUNT)) {
                b.tape_dirty[(b.test - 33) / 3] = true;
            }
        }
        b.test = channel >= 0 && channel < static_cast<int>(LIGHT_COUNT) ? channel : -1;
        if (b.test >= 0) {
            if (b.test < 33) {
                b.dirty[b.test] = true;
            } else if (b.test < static_cast<int>(BI2X_LIGHT_COUNT)) {
                b.tape_dirty[(b.test - 33) / 3] = true;
            }
        }
    }

    void set_tape_route(size_t source, size_t target) {
        if (source >= TAPE_COUNT || target >= TAPE_COUNT) {
            return;
        }
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        if (b.tape_routes[source] == target) {
            return;
        }
        const auto old_target = b.tape_routes[source];
        std::fill(b.tapes[old_target].begin(), b.tapes[old_target].end(), 0);
        b.tape_dirty[old_target] = true;
        b.tape_routes[source] = target;
    }

    size_t tape_destination(size_t source) {
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        return source < TAPE_COUNT ? b.tape_routes[source] : TAPE_COUNT;
    }

    bool configure_tape_routes(std::string_view routes) {
        std::array<size_t, TAPE_COUNT> parsed {};
        for (size_t i = 0; i < TAPE_COUNT; i++) {
            parsed[i] = i;
        }
        if (!routes.empty()) {
            for (size_t i = 0; i < TAPE_COUNT; i++) {
                const auto end = routes.find(',');
                const auto token = routes.substr(0, end);
                unsigned int value = 0;
                const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
                if (result.ec != std::errc() || result.ptr != token.data() + token.size()
                    || value >= TAPE_COUNT || (i + 1 < TAPE_COUNT) != (end != routes.npos)) {
                    return false;
                }
                parsed[i] = value;
                if (end != routes.npos) {
                    routes.remove_prefix(end + 1);
                }
            }
        }
        for (size_t i = 0; i < TAPE_COUNT; i++) {
            set_tape_route(i, parsed[i]);
        }
        return true;
    }

    void write_tape(size_t source, const uint8_t *rgb, size_t led_count) {
        if (source >= TAPE_COUNT || rgb == nullptr || led_count == 0 || led_count > 4096) {
            return;
        }
        auto &b = backend();
        std::lock_guard<std::mutex> lock(b.mutex);
        if (!b.state.started || b.board != Board::BI2X) {
            return;
        }
        const auto target = b.tape_routes[source];
        for (size_t channel = 33 + target * 3; channel < 36 + target * 3; channel++) {
            b.dirty[channel] = false;
        }
        auto &output = b.tapes[target];
        const auto count = output.size() / 3;
        for (size_t i = 0; i < count; i++) {
            const size_t input = i * led_count / count;
            std::copy_n(rgb + input * 3, 3, output.data() + i * 3);
        }
        b.tape_dirty[target] = true;
    }
}
