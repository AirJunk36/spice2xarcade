#pragma once

#include "device.h"

#include <array>
#include <atomic>
#include <memory>

struct bi2x_serial;

namespace bio2 {
    struct LegacyInput {
        std::array<bool, BUTTON_COUNT> buttons {};
        std::array<float, 7> analogs {};
    };

    struct LegacyOutput {
        // Fourteen keys, four panel lamps, eight spotlights, neon.
        std::array<bool, 27> lamps {};
        std::array<uint8_t, 9> ticker {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        std::array<uint8_t, 31> sdvx_lamps {};
    };

    using Io2Packet = std::array<uint8_t, 64>;
    bool decode_bi2a(const std::vector<uint8_t> &data, LegacyInput &input);
    std::array<uint8_t, 48> encode_bi2a(const LegacyOutput &output);
    LegacyInput decode_io2(const Io2Packet &packet);
    Io2Packet encode_io2(const LegacyOutput &output);
    bool decode_sdvx(Board board, const std::vector<uint8_t> &data, LegacyInput &input);
    std::array<uint8_t, 40> encode_sdvx_bi2a(const LegacyOutput &output);
    std::array<uint8_t, 24> encode_sdvx_kfca(const LegacyOutput &output);

    // Windows uses CyUSB3 endpoint transfers. POSIX is a test transport only.
    class Io2Usb {
    public:
        ~Io2Usb();
        bool open(const std::string &path);
        void close();
        bool transfer(uint8_t endpoint, Io2Packet &packet);
    private:
        void *handle = nullptr;
        void *completion = nullptr;
    };

    std::string find_bi2a_port();

    class LegacyBoard {
    public:
        explicit LegacyBoard(Board kind) : kind(kind) { reply.reserve(255); }
        ~LegacyBoard();
        bool connect(const std::string &port, bool skip_reset, const std::atomic<bool> &running);
        bool poll(const LegacyOutput &output, LegacyInput &input);
        void close();
        const char *error() const;
    private:
        Board kind;
        bi2x_serial *serial = nullptr;
        Io2Usb usb;
        uint8_t sequence = 0;
        uint8_t node = 0;
        bool ready = false;
        std::vector<uint8_t> reply;
        bool exchange(uint8_t address, uint16_t command,
            const uint8_t *data, size_t size, std::vector<uint8_t> &reply);
    };
}
