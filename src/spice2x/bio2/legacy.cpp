#include "legacy.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include "external/bi2x/bi2x_port.h"
}

namespace bio2 {
    bool decode_bi2a(const std::vector<uint8_t> &data, LegacyInput &input) {
        if (data.size() != 46) {
            return false;
        }
        input = {};
        input.buttons[0] = data[1] & 8;
        input.buttons[1] = data[1] & 4;
        input.buttons[2] = data[1] & 2;
        for (size_t i = 0; i < 4; i++) {
            input.buttons[3 + i] = data[9] & (0x80 >> i);
        }
        for (size_t i = 0; i < 14; i++) {
            input.buttons[7 + i] = data[18 + i * 2] & 0x80;
        }
        input.analogs[0] = data[16] / 255.f;
        input.analogs[1] = data[17] / 255.f;
        constexpr size_t sliders[] = {0, 2, 4, 6, 7};
        for (size_t i = 0; i < 5; i++) {
            input.analogs[2 + i] = (data[sliders[i]] >> 4) / 15.f;
        }
        return true;
    }

    std::array<uint8_t, 48> encode_bi2a(const LegacyOutput &output) {
        std::array<uint8_t, 48> result {};
        for (size_t i = 0; i < 14; i++) {
            result[7 + i] = output.lamps[i] ? 0x80 : 0;
        }
        for (size_t i = 0; i < 4; i++) {
            result[3 + i] = output.lamps[14 + i] ? 0x80 : 0;
        }
        for (size_t i = 0; i < 8; i++) {
            result[0x20 + i + (i >= 4)] = output.lamps[18 + i] ? 0x80 : 0;
        }
        result[0x24] = output.lamps[26] ? 0x80 : 0;
        std::copy(output.ticker.begin(), output.ticker.end(), result.begin() + 0x17);
        return result;
    }

    LegacyInput decode_io2(const Io2Packet &packet) {
        LegacyInput input;
        uint32_t pad = 0;
        for (size_t i = 0; i < 4; i++) {
            pad |= static_cast<uint32_t>(packet[8 + i] ^ 0xff) << (8 * i);
        }
        input.buttons[0] = pad & (1u << 4);
        input.buttons[1] = pad & (1u << 5);
        input.buttons[2] = pad & (1u << 30);
        for (size_t i = 0; i < 4; i++) {
            input.buttons[3 + i] = pad & (1u << i);
        }
        for (size_t i = 0; i < 14; i++) {
            input.buttons[7 + i] = pad & (1u << (16 + i));
        }
        input.analogs[0] = packet[14] / 255.f;
        input.analogs[1] = packet[13] / 255.f;
        for (size_t i = 0; i < 5; i++) {
            input.analogs[2 + i] = ((packet[15 + i / 2] >> (4 * (i % 2))) & 15) / 15.f;
        }
        return input;
    }

    Io2Packet encode_io2(const LegacyOutput &output) {
        Io2Packet result {};
        for (size_t i = 0; i < 14; i++) {
            result[11 + i / 8] |= static_cast<uint8_t>(output.lamps[i]) << (i % 8);
        }
        for (size_t i = 0; i < 4; i++) {
            result[8] |= static_cast<uint8_t>(output.lamps[14 + i]) << i;
        }
        for (size_t i = 0; i < 8; i++) {
            result[14] |= static_cast<uint8_t>(output.lamps[18 + i]) << i;
        }
        result[15] = output.lamps[26];
        std::copy(output.ticker.begin(), output.ticker.end(), result.begin() + 16);
        return result;
    }

    LegacyBoard::~LegacyBoard() {
        close();
    }

    void LegacyBoard::close() {
        bi2x_serial_close(serial);
        serial = nullptr;
        usb.close();
        node = 0;
        ready = false;
    }

    const char *LegacyBoard::error() const {
        return kind == Board::IO2
            ? "IO2 unavailable. Check the USB path, CyUSB3 driver and loaded IIDX FX2 firmware."
            : "BI2A unavailable. Check the serial port and installed IIDX BI2A firmware.";
    }

    bool LegacyBoard::exchange(uint8_t address, uint16_t command,
            const uint8_t *data, size_t size, std::vector<uint8_t> &reply) {
        if (size > 255) {
            return false;
        }
        const auto seq = ++sequence;
        std::array<uint8_t, 523> frame {};
        size_t written = 0;
        frame[written++] = 0xaa;
        uint8_t sum = 0;
        auto append = [&](uint8_t value) {
            if (value == 0xaa || value == 0xff) {
                frame[written++] = 0xff;
                frame[written++] = value ^ 0xff;
            } else {
                frame[written++] = value;
            }
        };
        auto byte = [&](uint8_t value) {
            sum += value;
            append(value);
        };
        byte(address);
        byte(command >> 8);
        byte(command & 0xff);
        byte(seq);
        byte(static_cast<uint8_t>(size));
        for (size_t i = 0; i < size; i++) {
            byte(data[i]);
        }
        append(sum);
        if (bi2x_serial_write(serial, frame.data(), written) != 0) {
            return false;
        }
        // Bounded streaming parser: ignore noise, stale replies and broken checksums.
        const auto deadline = bi2x_time_ms() + 200;
        std::array<uint8_t, 261> packet {};
        size_t received = 0;
        bool started = false;
        bool escaped = false;
        while (bi2x_time_ms() < deadline) {
            uint8_t value = 0;
            const int count = bi2x_serial_read(serial, &value, 1, 2);
            if (count < 0) {
                return false;
            }
            if (count == 0) {
                continue;
            }
            if (value == 0xaa && !escaped) {
                started = true;
                received = 0;
                continue;
            }
            if (!started) {
                continue;
            }
            if (escaped) {
                value ^= 0xff;
                escaped = false;
            } else if (value == 0xff) {
                escaped = true;
                continue;
            }
            packet[received++] = value;
            if (received < 5 || received != size_t(6 + packet[4])) {
                continue;
            }
            uint8_t checksum = 0;
            for (size_t i = 0; i + 1 < received; i++) {
                checksum += packet[i];
            }
            const bool valid = checksum == packet[received - 1] && packet[0] == (address | 0x80)
                && packet[1] == (command >> 8) && packet[2] == (command & 0xff)
                && packet[3] == seq;
            if (valid) {
                reply.assign(packet.begin() + 5, packet.begin() + received - 1);
                return true;
            }
            started = false;
            received = 0;
        }
        return false;
    }

    bool LegacyBoard::connect(const std::string &port, bool skip_reset,
            const std::atomic<bool> &running) {
        close();
        if (kind == Board::IO2) {
            ready = usb.open(port);
            return ready;
        }
        const auto path = port.empty() ? find_bi2a_port() : port;
        if (path.empty() || bi2x_serial_open(&serial, path.c_str()) != 0) {
            return false;
        }
        auto pause = [&](unsigned int milliseconds) {
            for (unsigned int i = 0; i < milliseconds && running; i += 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return running.load();
        };
        if (!skip_reset) {
            std::array<uint8_t, 525> reset {};
            if (bi2x_serial_write(serial, reset.data(), reset.size()) != 0
                    || bi2x_serial_set_break(serial, 1) != 0) {
                return false;
            }
            const bool active = pause(1450);
            bi2x_serial_set_break(serial, 0);
            if (!active || !pause(1200)) {
                return false;
            }
        }
        bi2x_serial_purge(serial);
        const auto deadline = bi2x_time_ms() + 500;
        bool synced = false;
        while (running && bi2x_time_ms() < deadline) {
            const uint8_t sync = 0xaa;
            uint8_t value = 0;
            if (bi2x_serial_write(serial, &sync, 1) != 0) {
                return false;
            }
            if (bi2x_serial_read(serial, &value, 1, 10) == 1 && value == sync) {
                synced = true;
                break;
            }
        }
        if (!synced) {
            return false;
        }
        bi2x_serial_purge(serial);
        const uint8_t zero = 0;
        if (!exchange(0, 1, &zero, 1, reply) || reply.size() != 1
                || reply[0] == 0 || reply[0] > 16) {
            return false;
        }
        const auto nodes = reply[0];
        for (uint8_t address = 1; address <= nodes && running; address++) {
            if (!exchange(address, 2, nullptr, 0, reply) || reply.size() < 12) {
                return false;
            }
            if (std::memcmp(reply.data() + 8, "BI2A", 4) == 0) {
                node = address;
            }
            if (!exchange(address, 3, nullptr, 0, reply) || reply.size() != 1
                    || reply[0] != 0) {
                return false;
            }
        }
        // Initialize the sub-I/O relay; without 0x2d only the fourteen keys work.
        const uint8_t init = 0x2d;
        ready = running && node != 0 && exchange(node, 0x100, &init, 1, reply)
            && reply.size() == 1 && reply[0] == 0;
        return ready;
    }

    bool LegacyBoard::poll(const LegacyOutput &output, LegacyInput &input) {
        if (!ready) {
            return false;
        }
        if (kind == Board::IO2) {
            auto packet = encode_io2(output);
            if (!usb.transfer(0x01, packet) || !usb.transfer(0x81, packet)) {
                return false;
            }
            input = decode_io2(packet);
            return true;
        }
        const auto data = encode_bi2a(output);
        return exchange(node, 0x152, data.data(), data.size(), reply)
            && decode_bi2a(reply, input);
    }
}
