#include "reader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "util/logging.h"
#include "misc/eamuse.h"
#include "util/utils.h"
#include "structuredmessage.h"

static std::vector<std::thread *> READER_THREADS;
static std::atomic<bool> READER_THREAD_RUNNING {false};

Reader::Reader(const std::string &port) : port(port) {

    // open port using an NT path to support COM ports past 9
    std::filesystem::path serial_path = fmt::format("\\\\.\\{}", this->port);
    this->serial_handle = CreateFileW(
            serial_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

    // check if valid
    this->valid = this->serial_handle != INVALID_HANDLE_VALUE;
    if (!this->valid) {
        auto last_error = get_last_error_string();

        log_warning("reader", "{}: failed to open serial connection for reader: {}",
                this->port,
                last_error);
    }
}

Reader::~Reader() {
    if (this->serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(this->serial_handle);
        log_info("reader", "closed reader on {}", this->port);
    }
}

bool Reader::is_valid() {
    return this->valid;
}

bool Reader::initialize(bool discover_chain) {
    if (!this->set_comm_state(CBR_57600) || !this->wait_for_handshake()) {
        return false;
    }

    log_info("reader", "{}: card reader connected", this->port);
    const auto assigned = this->msg_write_read(
        StructuredMessage(0, 0, READER_CMD_SET_ID, this->gen_msg_id(), {0}));
    if (assigned.empty()) {
        log_warning("reader", "{}: no reply to ACIO address assignment", this->port);
        return false;
    }

    const auto reply = assigned.back().get_data();
    const uint8_t node_count = reply.size() == 6 ? reply[5] : 0;
    this->units[0].address = 1;
    if (!this->initialize_unit(this->units[0], false)) {
        return false;
    }
    this->unit_count = 1;

    if (discover_chain) {
        log_info("reader", "{}: ACIO reports {} node(s)", this->port, node_count);
        if (node_count >= 2) {
            this->units[1].address = 2;
            if (this->initialize_unit(this->units[1], true)) {
                this->unit_count = 2;
                log_info("reader", "{}: ACIO node 1 is P1, node 2 is P2", this->port);
            }
        }
    }

    return true;
}

bool Reader::initialize_unit(Unit &unit, bool require_card_model) {
    auto replies = this->msg_write_cmd_read(unit, READER_CMD_VERSION);
    if (replies.empty()) {
        log_warning("reader", "{} node {}: no reply to ACIO version request",
            this->port, unit.address);
        return false;
    }

    // ACIO version fields follow the five-byte packet header.
    const auto version = replies.back().get_data();
    if (version.size() < 17) {
        log_warning("reader", "{} node {}: short ACIO version reply ({})",
            this->port, unit.address, version.size());
        return false;
    }
    const std::string model(version.begin() + 13, version.begin() + 17);
    log_info("reader", "{} node {}: card reader model: {}", this->port, unit.address, model);
    if (require_card_model && model != "ICCA" && model != "ICCB" && model != "ICCC") {
        log_warning("reader", "{} node {}: no supported P2 reader", this->port, unit.address);
        return false;
    }

    if (this->msg_write_cmd_read(unit, READER_CMD_INIT2).empty()) {
        log_warning("reader", "{} node {}: no reply to ACIO startup", this->port, unit.address);
        return false;
    }

    unit.reinitialized = 1;
    if (this->msg_write_cmd_read(unit, READER_CMD_REINITIALIZE, {0}).empty()) {
        log_warning("reader", "{} node {}: no reply to card reader initialization",
            this->port, unit.address);
        return false;
    }
    log_info("reader", "{} node {}: card reader init done", this->port, unit.address);
    return this->init_crypt(unit);
}

bool Reader::init_crypt(Unit &unit) {
    std::vector<uint8_t> game_key(4);
    for (auto &byte : game_key) {
        byte = static_cast<uint8_t>(rand());
    }
    auto replies = this->msg_write_cmd_read(unit, READER_CMD_KEY_EXCHANGE, game_key);
    if (replies.empty()) {
        log_warning("reader", "{} node {}: no reply to encrypted polling setup",
            this->port, unit.address);
        return false;
    }
    const auto reply = replies.back().get_data();
    if (reply.size() != 9) {
        log_warning("reader", "{} node {}: invalid encrypted polling setup reply",
            this->port, unit.address);
        return false;
    }
    const auto key_from_bytes = [](const uint8_t *bytes) {
        return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
            (uint32_t{bytes[2]} << 8) | bytes[3];
    };
    unit.crypt.set_keys(key_from_bytes(reply.data() + 5), key_from_bytes(game_key.data()));
    log_info("reader", "{} node {}: reader crypt init done", this->port, unit.address);
    return true;
}

bool Reader::read_status(Unit &unit, int player) {
    auto replies = this->msg_write_cmd_read(unit, READER_CMD_GET_STATUS_ENC, {0x10});
    if (replies.empty()) {
        return false;
    }
    auto status = replies.back().get_data();
    if (status.size() != 23) {
        return true;
    }

    unit.crypt.crypt(status.data() + 5, 18);
    const uint16_t crc = status[21] << 8 | status[22];
    if (crc != unit.crypt.crc(status.data() + 5, 16)) {
        return false;
    }

    unit.keypad_started = status[16];
    unit.keypad_state = status[19] << 8 | status[20];
    if (unit.keypad_started > 0) {
        eamuse_set_keypad_overrides_reader(player, unit.keypad_state);
    }

    const auto now = std::chrono::steady_clock::now();
    if (status[5] == 2 && now >= unit.next_insert) {
        std::copy_n(status.data() + 7, unit.card_uid.size(), unit.card_uid.begin());
        log_info("reader", "{} node {}: reader input for P{}", this->port, unit.address, player + 1);
        eamuse_card_insert(player, unit.card_uid.data());
        unit.next_insert = now + std::chrono::milliseconds(2500);
    }
    return true;
}

bool Reader::poll(int first_player) {
    std::array<bool, 2> pending {};
    for (size_t i = 0; i < this->unit_count; ++i) {
        pending[i] = !this->msg_write_cmd_read(this->units[i], READER_CMD_RFID_READ_UID,
            {0x00, 0x03, 0xff, 0xff}).empty();
    }
    Sleep(200);
    bool ok = true;
    for (size_t i = 0; i < this->unit_count; ++i) {
        const auto player = first_player < 0 ? GetKeyState(VK_NUMLOCK) & 1
            : first_player + static_cast<int>(i);
        if (!pending[i] || !this->read_status(this->units[i], player)) {
            log_warning("reader", "{} node {}: card polling failed", this->port,
                this->units[i].address);
            ok = false;
        }
    }
    return ok;
}

bool Reader::set_comm_state(DWORD BaudRate) {

    // settings
    DCB serial_params{};
    serial_params.DCBlength = sizeof(serial_params);
    if (!GetCommState(this->serial_handle, &serial_params)) {
        log_warning("reader", "{}: unable to get COM port state: 0x{:x}", this->port, GetLastError());
        return false;
    }

    serial_params.BaudRate = BaudRate;
    serial_params.ByteSize = 8;
    serial_params.StopBits = ONESTOPBIT;
    serial_params.Parity = NOPARITY;
    // The ACIO reader cable only carries TX, RX and ground. Don't inherit flow
    // control from the Windows COM port settings.
    serial_params.fBinary = TRUE;
    serial_params.fParity = FALSE;
    serial_params.fOutxCtsFlow = FALSE;
    serial_params.fOutxDsrFlow = FALSE;
    serial_params.fDsrSensitivity = FALSE;
    serial_params.fOutX = FALSE;
    serial_params.fInX = FALSE;
    if (!SetCommState(this->serial_handle, &serial_params)) {
        log_warning("reader", "{}: unable to set COM port state: 0x{:x}", this->port, GetLastError());
        return false;
    }

    // timeouts
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 30;
    timeouts.ReadTotalTimeoutConstant = 30;
    timeouts.ReadTotalTimeoutMultiplier = 5;
    timeouts.WriteTotalTimeoutConstant = 30;
    timeouts.WriteTotalTimeoutMultiplier = 5;
    if (!SetCommTimeouts(this->serial_handle, &timeouts)) {
        log_warning("reader", "{}: unable to set COM port timeouts: 0x{:x}", this->port, GetLastError());
        return false;
    }

    return true;
}

bool Reader::wait_for_handshake() {
    // baud rates
    DWORD baud_rates[] = { CBR_57600, CBR_38400, CBR_19200, CBR_9600 };

    // variables
    DWORD bytes_written = 0;
    DWORD bytes_read = 0;
    uint8_t read_buffer[565];

    // generate handshake buffer
    uint8_t handshake_buffer[565];
    memset(handshake_buffer, 0, 525);
    memset(handshake_buffer + 525, 0xAA, 40);

    // try all the baud rates
    for (size_t i = 0; i < 4; i++) {

        if (!this->set_comm_state(baud_rates[i])) {
            continue;
        }

        // handshake loop
        for (size_t n = 0; n < 10; n++) {

            // write handshake
            if (!WriteFile(
                        this->serial_handle,
                        handshake_buffer,
                        sizeof(handshake_buffer),
                        &bytes_written,
                        nullptr))
            {
                break;
            }

            // read handshake
            bytes_read = 0;
            if (!ReadFile(
                        this->serial_handle,
                        read_buffer,
                        sizeof(read_buffer),
                        &bytes_read,
                        nullptr))
            {
                break;
            }

            // check handshake
            if (bytes_read > 0 && read_buffer[bytes_read - 1] == 0xAA) {
                return true;
            }

            // sleep
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        log_warning("reader", "{}: no handshake received for {} baud", this->port, baud_rates[i]);
    }

    // no handshake on all baud rates
    log_warning("reader", "{}: no handshake received for any attempted baud rate", this->port);
    return false;
}

bool Reader::msg_write(Message msg) {

    // get message data
    std::vector<uint8_t> msg_encoded = msg.get_data_encoded();
    uint8_t chk_sum = msg.chk_sum();

    // create write buffer
    uint8_t write_buffer[512];
    DWORD write_buffer_len = 0;

    // fill write buffer
    write_buffer[write_buffer_len++] = 0xAA;
    for (const uint8_t c : msg_encoded) {
        write_buffer[write_buffer_len++] = c;
    }

    // write checksum
    if (chk_sum == 0xAA || chk_sum == 0xFF) {
        write_buffer[write_buffer_len++] = 0xFF;
        write_buffer[write_buffer_len++] = ~chk_sum;
    } else {
        write_buffer[write_buffer_len++] = chk_sum;
    }

    // write buffer
    DWORD bytes_written = 0;
    return WriteFile(this->serial_handle,
            write_buffer,
            write_buffer_len,
            &bytes_written,
            nullptr) != 0;
}

std::vector<Message> Reader::msg_write_read(Message msg) {
    if (!this->msg_write(msg)) {
        return {};
    }
    return this->msg_read();
}

std::vector<Message> Reader::msg_write_cmd_read(
        const Unit &unit, uint8_t cmd, const std::vector<uint8_t> &data) {
    const auto packet_id = this->gen_msg_id();
    auto replies = this->msg_write_read(
        StructuredMessage(unit.address, unit.reinitialized, cmd, packet_id, data));
    // Commands are serialized on one port. Ignore echoes and stale replies from other nodes.
    std::erase_if(replies, [&](const Message &reply) {
        const auto &bytes = reply.get_data();
        return bytes[0] != (unit.address | 0x80) || bytes[2] != cmd || bytes[3] != packet_id;
    });
    return replies;
}

std::vector<Message> Reader::msg_read() {

    // create buffer
    std::vector<Message> msgs;
    uint8_t read_buffer[4096];
    DWORD read_buffer_len = 0;

    // read to buffer
    if (ReadFile(
                this->serial_handle,
                read_buffer,
                sizeof(read_buffer),
                &read_buffer_len,
                nullptr) && read_buffer_len > 0)
    {
        std::vector<uint8_t> msg_data;
        size_t msg_remaining = 0;
        bool escape = false;
        for (size_t i = 0; i < read_buffer_len; i++) {
            uint8_t b = read_buffer[i];
            if (msg_remaining > 0) {

                // add msg data length
                if (msg_data.size() < 6 && msg_remaining == 1)
                    msg_remaining += msg_data[4];

                if (escape) { // escaped byte
                    b = ~b;
                    msg_data.push_back(b);
                    msg_remaining--;
                    escape = false;
                } else if (b == 0xAA) { // message start
                    msg_remaining = 6;
                    msg_data.clear();
                } else if (b == 0xFF) { // escape
                    escape = true;
                } else { // normal data
                    msg_data.push_back(b);
                    msg_remaining--;
                }
            } else if (b == 0xAA) { // message start
                msg_remaining = 6;
                msg_data.clear();
            }

            if (msg_remaining == 0 && msg_data.size() >= 6) { // message done
                std::vector<uint8_t> msg_ext_data;
                for (size_t n = 0; n < msg_data[4] && n < msg_data.size() - 6; n++) {
                    msg_ext_data.push_back(msg_data[5 + n]);
                }
                StructuredMessage msg(
                        msg_data[0],
                        msg_data[1],
                        msg_data[2],
                        msg_data[3],
                        msg_ext_data
                );
                if (msg.chk_sum() == msg_data[msg_data.size() - 1]) {
                    msgs.push_back(msg);
                }
                msg_data.clear();
            }
        }
    }

    // return message buffer
    return msgs;
}

void start_reader_thread(const std::string &port, int id, bool discover_chain) {
    READER_THREAD_RUNNING = true;
    READER_THREADS.push_back(new std::thread([port, id, discover_chain]() {
        log_info("reader", "{}: starting reader thread", port);

        while (READER_THREAD_RUNNING) {
            Reader reader(port);
            if (reader.is_valid() && reader.initialize(discover_chain)) {
                while (READER_THREAD_RUNNING && reader.poll(id)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            } else {
                log_warning("reader", "{}: unable to initialize reader", port);
            }
            if (READER_THREAD_RUNNING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
            }
        }
    }));

    // wait for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void stop_reader_thread() {

    // stop threads
    if (READER_THREAD_RUNNING) {
        READER_THREAD_RUNNING = false;
    }

    // kill threads
    while (!READER_THREADS.empty()) {
        delete READER_THREADS.back();
        READER_THREADS.pop_back();
    }
}
