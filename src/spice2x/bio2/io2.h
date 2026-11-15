#pragma once

#include <cstddef>
#include <cstdint>

namespace bio2 {
    // CyUSB3 SINGLE_TRANSFER wire ABI, identical on 32-bit and 64-bit Windows.
    #pragma pack(push, 1)
    struct Io2Transfer {
        uint8_t request_type;
        uint8_t request;
        uint16_t value;
        uint16_t index;
        uint16_t length;
        uint32_t timeout;
        uint8_t reserved;
        uint8_t endpoint;
        uint32_t nt_status;
        uint32_t usb_status;
        uint32_t iso_offset;
        uint32_t iso_length;
        uint32_t offset;
        uint32_t size;
    };
    #pragma pack(pop)
    static_assert(sizeof(Io2Transfer) == 38);
    static_assert(offsetof(Io2Transfer, endpoint) == 13);
    static_assert(offsetof(Io2Transfer, offset) == 30);
}
