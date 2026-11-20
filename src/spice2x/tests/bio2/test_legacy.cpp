#include "bio2/legacy.h"
#include "bio2/io2.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static void contract() {
    using bio2::Board;
    Board board = Board::IO2;
    check(bio2::parse_board("", board) && board == Board::BI2X, "default board compatibility");
    check(bio2::parse_board("bi2a", board) && board == Board::BI2A, "select BI2A");
    check(!bio2::parse_board("unknown", board) && board == Board::BI2A,
        "invalid selection does not select different hardware");
    check(!bio2::light_available(Board::BI2A, 18) && bio2::light_available(Board::BI2A, 92),
        "legacy output capabilities");
    check(bio2::light_available(Board::BI2X, 83) && !bio2::light_available(Board::BI2X, 84),
        "BI2X output channel compatibility");
    check(!bio2::button_available(Board::IO2, 21) && bio2::analog_available(Board::IO2, 6),
        "legacy input capabilities");

    // Wire bit positions from the IO2 specification, including reversed system order.
    constexpr unsigned int bits[] = {4, 5, 30, 0, 1, 2, 3,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
    for (size_t channel = 0; channel < 21; channel++) {
        bio2::Io2Packet packet {};
        std::fill(packet.begin() + 8, packet.begin() + 12, 0xff);
        packet[8 + bits[channel] / 8] ^= 1u << (bits[channel] % 8);
        const auto decoded = bio2::decode_io2(packet);
        for (size_t key = 0; key < decoded.buttons.size(); key++) {
            check(decoded.buttons[key] == (key == channel), "IO2 input wire bit order");
        }
    }
    bio2::LegacyInput input;
    std::vector<uint8_t> raw(46);
    raw[1] = 14;
    raw[9] = 0xf0;
    raw[16] = 0xaa;
    raw[17] = 0xff;
    raw[18] = 0x80;
    raw[44] = 0x80;
    raw[0] = 0xf0;
    check(bio2::decode_bi2a(raw, input), "BI2A status length");
    check(input.buttons[0] && input.buttons[1] && input.buttons[2]
        && input.buttons[3] && input.buttons[6] && input.buttons[7] && input.buttons[20]
        && !input.buttons[8], "BI2A system, panel and key byte offsets");
    check(input.analogs[0] == 170 / 255.f && input.analogs[1] == 1.f
        && input.analogs[2] == 1.f, "BI2A absolute turntables and slider");
    raw.pop_back();
    check(!bio2::decode_bi2a(raw, input), "reject truncated BI2A status");

    for (size_t channel = 0; channel < 27; channel++) {
        bio2::LegacyOutput output;
        output.lamps[channel] = true;
        const auto bi2a = bio2::encode_bi2a(output);
        const auto io2 = bio2::encode_io2(output);
        size_t offset;
        if (channel < 14) {
            offset = 7 + channel;
            check(io2[11 + channel / 8] == (1u << (channel % 8)), "IO2 key lamp order");
        } else if (channel < 18) {
            offset = 3 + channel - 14;
            check(io2[8] == (1u << (channel - 14)), "IO2 panel lamp order");
        } else if (channel < 26) {
            offset = 32 + channel - 18 + (channel >= 22);
            check(io2[14] == (1u << (channel - 18)), "IO2 spotlight order");
        } else {
            offset = 36;
            check(io2[15] == 1, "IO2 neon output");
        }
        check(bi2a[offset] == 0x80, "BI2A lamp bit and byte order");
    }
    for (const auto kind : {Board::BI2A, Board::IO2}) {
        check(bio2::start("BIO2_TEST_NO_DEVICE", "", true, kind), "asynchronous startup");
        bio2::stop();
        check(!bio2::status().started && !bio2::read_button(0),
            "stop before device initialization completes");
    }
}

int main(int argc, char **argv) {
    contract();
    if (argc != 3) {
        return 0;
    }
    bio2::Board board;
    check(bio2::parse_board(argv[1], board), "valid test board");
    check(bio2::start(argv[2], "", true, board), "start legacy backend");
    check(wait_for([] { return bio2::status().online; }, 5000), "legacy board online");
    check(bio2::read_button(0) && bio2::read_button(1) && bio2::read_button(2)
        && bio2::read_button(3) && bio2::read_button(7) && bio2::read_button(20)
        && !bio2::read_button(8), "physical legacy input snapshot");
    check(!bio2::read_button(21, true), "missing headphone input cannot invert to held");
    check(bio2::pressed_buttons() == (0x0fu | (1u << 7) | (1u << 20)),
        "binding snapshot identifies the pressed physical channels");
    check(std::abs(bio2::read_analog(0) - 170 / 255.f) < .001f
        && bio2::read_analog(1) == 1.f && bio2::read_analog(2) == 1.f
        && std::abs(bio2::read_analog(6) - 3 / 15.f) < .001f,
        "native turntables and five effect sliders");
    bio2::write_ticker("IIDX TEST");
    for (size_t channel : {0, 14, 84, 92}) {
        bio2::write_light(channel, 1.f);
    }
    bio2::test_light(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bio2::test_light(-1);
    check(bio2::read_light(1) == 0.f, "legacy light test restores game state");
    check(wait_for([] { return !bio2::status().online; }, 5000), "legacy timeout detected");
    check(!bio2::read_button(7) && !bio2::read_button(7, true)
        && bio2::read_analog(0) == 0.f, "legacy disconnect releases inputs");
    check(bio2::pressed_buttons() == 0, "binding snapshot is empty while disconnected");
    check(wait_for([] { return bio2::status().online; }, 5000), "legacy automatic reconnect");
    check(wait_for([] { return bio2::read_button(7); }, 500), "legacy inputs resume");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bio2::stop();
    check(!bio2::status().started, "legacy clean shutdown");
}
