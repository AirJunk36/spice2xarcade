#include "bio2/device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

static void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

template<class F> static bool wait_for(F predicate, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int main(int argc, char **argv) {
    check(bio2::button_names().size() == bio2::BUTTON_COUNT, "input catalogue");
    check(bio2::light_names().size() == bio2::LIGHT_COUNT, "output catalogue");
    check(bio2::is_device(bio2::DEVICE_ID) && !bio2::is_device("keyboard"), "device identity");
    auto board = bio2::Board::SDVX_BI2A;
    check(bio2::parse_sdvx_board("bi2x", board) && board == bio2::Board::BI2X,
        "SDVX selects the native Lightning BI2X transport");
    constexpr std::string_view sdvx_keys[] {"BT-A", "BT-B", "BT-C", "BT-D", "FX-L", "FX-R"};
    for (size_t key = 0; key < std::size(sdvx_keys); ++key) {
        check(bio2::sdvx_iidx_channel_name(sdvx_keys[key]) == bio2::button_names(board)[key + 7]
            && bio2::sdvx_iidx_channel_name(sdvx_keys[key]) == bio2::light_names(board)[key],
            "SDVX keys and lamps use the same Lightning P1 layout");
    }
    check(bio2::sdvx_iidx_channel_name("Start") == "P1 Start"
        && bio2::sdvx_iidx_channel_name("VOL-L") == bio2::analog_names(board)[0]
        && bio2::sdvx_iidx_channel_name("VOL-R") == bio2::analog_names(board)[1],
        "SDVX start and analog mapping");
    check(!bio2::read_button(0) && !bio2::read_button(9999), "offline inputs released");
    check(!bio2::read_button(0, true), "offline inverted inputs released");
    check(bio2::configure_tape_routes(""), "default tape routes");
    check(!bio2::configure_tape_routes("1,2"), "reject partial routes");
    check(!bio2::configure_tape_routes("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17"),
        "reject invalid destination");
    check(bio2::configure_tape_routes("1,0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"),
        "accept strip swap");
    check(bio2::tape_destination(0) == 1 && bio2::tape_destination(1) == 0, "strip swap");
    check(!bio2::configure_tape_routes("-1,0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"),
        "reject negative destination");
    check(bio2::tape_destination(0) == 1, "invalid routes leave previous routes intact");
    bio2::write_light(0, 2.f);
    check(bio2::read_light(0) == 1.f, "clamp brightness");
    bio2::write_light(0, std::numeric_limits<float>::quiet_NaN());
    check(bio2::read_light(0) == 1.f, "reject NaN brightness");
    bio2::stop();

    if (argc != 3) {
        return 0;
    }
    check(bio2::start(argv[1], argv[2], true, board), "start native driver with synthetic modules");
    check(wait_for([] { return bio2::status().online; }, 10000), "board online");
    check(wait_for([] { return bio2::read_button(7) && !bio2::read_button(8)
            && bio2::read_button(9) && bio2::read_button(15); }, 2000), "physical key bitmap");
    const auto &buttons = bio2::button_names(board);
    const auto mapped = [&](std::string_view name) {
        const auto channel = std::find(buttons.begin(), buttons.end(),
            bio2::sdvx_iidx_channel_name(name));
        return channel != buttons.end() && bio2::read_button(channel - buttons.begin());
    };
    check(mapped("BT-A") && !mapped("BT-B") && mapped("BT-C"),
        "SDVX layout reads the expected physical BI2X inputs");
    bio2::write_light(0, 1.f);
    bio2::write_light(14, 1.f);
    bio2::write_light(16, 1.f);
    bio2::write_light(18, 1.f);
    bio2::write_light(20, 1.f);
    bio2::test_light(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bio2::test_light(-1);
    check(bio2::read_light(1) == 0.f, "light test preserves original output");
    // Route full RGB data to another physical strip, including a different length.
    bio2::set_tape_route(0, 2);
    const uint8_t rgb[] = {255, 0, 0, 0, 255, 0};
    bio2::write_tape(0, rgb, 2);
    bio2::test_light(41);
    for (int i = 0; i < 20; i++) {
        bio2::write_tape(0, rgb, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bio2::test_light(-1);
    check(wait_for([] { return !bio2::status().online; }, 9000), "connection loss detected");
    check(!bio2::read_button(7), "disconnect releases held keys");
    check(wait_for([] { return bio2::status().online; }, 10000), "automatic reconnect");
    check(wait_for([] { return bio2::read_button(7); }, 2000), "inputs resume after reconnect");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    bio2::stop();
    check(!bio2::status().started && !bio2::read_button(7), "clean shutdown");
    return 0;
}
