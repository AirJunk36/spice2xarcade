#include "legacy.h"
#include "io2.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>

namespace bio2 {
    static const GUID io2_guid = {0xae18aa60, 0x7f6a, 0x11d4,
        {0x97, 0xdd, 0x00, 0x01, 0x02, 0x29, 0xb9, 0x59}};
    static const GUID ports_guid = {0x4d36e978, 0xe325, 0x11ce,
        {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

    std::string find_bi2a_port() {
        const auto devices = SetupDiGetClassDevsA(&ports_guid, nullptr, nullptr, DIGCF_PRESENT);
        if (devices == INVALID_HANDLE_VALUE) {
            return {};
        }
        std::string result;
        for (DWORD i = 0; result.empty(); i++) {
            SP_DEVINFO_DATA info {};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInfo(devices, i, &info)) {
                break;
            }
            char id[512] {};
            if (!SetupDiGetDeviceInstanceIdA(devices, &info, id, sizeof(id), nullptr)) {
                continue;
            }
            CharUpperBuffA(id, static_cast<DWORD>(std::strlen(id)));
            if (!std::strstr(id, "VID_1CCF&PID_804C")
                    && !std::strstr(id, "VID_1CCF&PID_8040")) {
                continue;
            }
            const auto key = SetupDiOpenDevRegKey(devices, &info,
                DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
            if (key == INVALID_HANDLE_VALUE) {
                continue;
            }
            char port[256] {};
            DWORD length = sizeof(port);
            DWORD type = 0;
            if (RegQueryValueExA(key, "PortName", nullptr, &type,
                    reinterpret_cast<BYTE *>(port), &length) == ERROR_SUCCESS
                    && type == REG_SZ && length < sizeof(port)) {
                result = port;
            }
            RegCloseKey(key);
        }
        SetupDiDestroyDeviceInfoList(devices);
        return result;
    }

    static std::string find_io2() {
        const auto devices = SetupDiGetClassDevsA(&io2_guid, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) {
            return {};
        }
        std::string result;
        for (DWORD i = 0; result.empty(); i++) {
            SP_DEVICE_INTERFACE_DATA info {};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &io2_guid, i, &info)) {
                break;
            }
            DWORD length = 0;
            SetupDiGetDeviceInterfaceDetailA(devices, &info, nullptr, 0, &length, nullptr);
            if (length < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A)) {
                continue;
            }
            std::vector<uint8_t> storage(length);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(storage.data());
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailA(devices, &info, detail, length,
                    nullptr, nullptr)) {
                result = detail->DevicePath;
            }
        }
        SetupDiDestroyDeviceInfoList(devices);
        return result;
    }

    bool Io2Usb::open(const std::string &path) {
        close();
        const auto device_path = path.empty() ? find_io2() : path;
        if (device_path.empty()) {
            return false;
        }
        const auto fd = CreateFileA(device_path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (fd == INVALID_HANDLE_VALUE) {
            return false;
        }
        handle = fd;
        completion = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (completion == nullptr) {
            close();
            return false;
        }
        return true;
    }

    void Io2Usb::close() {
        if (handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
        if (completion != nullptr) {
            CloseHandle(completion);
            completion = nullptr;
        }
    }

    bool Io2Usb::transfer(uint8_t endpoint, Io2Packet &packet) {
        if (handle == nullptr) {
            return false;
        }
        struct {
            Io2Transfer transfer;
            Io2Packet data;
        } buffer {};
        static_assert(offsetof(decltype(buffer), data) == sizeof(Io2Transfer));
        buffer.transfer.timeout = 1; // Driver units are seconds.
        buffer.transfer.endpoint = endpoint;
        buffer.transfer.offset = sizeof(Io2Transfer);
        buffer.transfer.size = packet.size();
        buffer.data = packet;
        OVERLAPPED pending {};
        pending.hEvent = completion;
        ResetEvent(completion);
        DWORD transferred = 0;
        BOOL ok = DeviceIoControl(handle, CTL_CODE(FILE_DEVICE_UNKNOWN, 9,
            METHOD_BUFFERED, FILE_ANY_ACCESS), &buffer, sizeof(buffer), &buffer,
            sizeof(buffer), &transferred, &pending);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(pending.hEvent, 200) != WAIT_OBJECT_0) {
                // Cancel on the issuing thread (also supported on Windows XP).
                CancelIo(handle);
            }
            ok = GetOverlappedResult(handle, &pending, &transferred, TRUE);
        }
        if (!ok || transferred != sizeof(buffer)
                || buffer.transfer.nt_status != 0 || buffer.transfer.usb_status != 0
                || buffer.transfer.size != packet.size()) {
            return false;
        }
        packet = buffer.data;
        return true;
    }
}
#else
extern "C" {
#include "external/bi2x/bi2x_port.h"
}

namespace bio2 {
    std::string find_bi2a_port() {
        return {};
    }

    bool Io2Usb::open(const std::string &path) {
        close();
        bi2x_serial *serial = nullptr;
        if (path.empty() || bi2x_serial_open(&serial, path.c_str()) != 0) {
            return false;
        }
        handle = serial;
        return true;
    }

    void Io2Usb::close() {
        bi2x_serial_close(static_cast<bi2x_serial *>(handle));
        handle = nullptr;
    }

    bool Io2Usb::transfer(uint8_t endpoint, Io2Packet &packet) {
        auto *serial = static_cast<bi2x_serial *>(handle);
        if (serial == nullptr || bi2x_serial_write(serial, &endpoint, 1) != 0) {
            return false;
        }
        if (endpoint == 1) {
            return bi2x_serial_write(serial, packet.data(), packet.size()) == 0;
        }
        size_t received = 0;
        const auto deadline = bi2x_time_ms() + 200;
        while (received < packet.size() && bi2x_time_ms() < deadline) {
            const int count = bi2x_serial_read(serial, packet.data() + received,
                packet.size() - received, 2);
            if (count < 0) {
                return false;
            }
            received += count;
        }
        return received == packet.size();
    }
}
#endif

namespace bio2 {
    Io2Usb::~Io2Usb() {
        close();
    }
}
