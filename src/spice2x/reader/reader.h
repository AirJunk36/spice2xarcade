#pragma once

#include <array>
#include <chrono>
#include <string>

#include <windows.h>

#include "crypt.h"
#include "message.h"

enum reader_cmd {
    READER_CMD_SET_ID         = 0x01,
    READER_CMD_VERSION        = 0x02,
    READER_CMD_INIT2          = 0x03,
    READER_CMD_REINITIALIZE   = 0x30,
    READER_CMD_READ_CARD_UID  = 0x31,
    READER_CMD_GET_STATUS     = 0x34,
    READER_CMD_SET_ACTION     = 0x35,
    READER_CMD_SLEEP_MODE     = 0x3A,
    READER_CMD_KEY_EXCHANGE   = 0x60,
    READER_CMD_RFID_READ_UID  = 0x61,
    READER_CMD_GET_STATUS_ENC = 0x64,
};

enum reader_action {
    READER_ACTION_BLOCK       = 0x00,
    READER_ACTION_ACCEPT_CARD = 0x11,
    READER_ACTION_EJECT_CARD  = 0x12,
};

class Reader {

public:
    struct Unit {
        uint8_t address = 0;
        uint8_t reinitialized = 0;
        uint8_t keypad_started = 0;
        uint16_t keypad_state = 0;
        std::array<uint8_t, 8> card_uid {};
        Crypt crypt;
        std::chrono::steady_clock::time_point next_insert {};
    };

    explicit Reader(const std::string &port);
    ~Reader();

    bool is_valid();
    bool initialize(bool discover_chain);
    bool poll(int first_player);

private:
    const std::string port;
    HANDLE serial_handle;
    bool valid;
    std::array<Unit, 2> units {};
    size_t unit_count = 0;
    uint8_t cur_msg_id = 0;

    uint8_t gen_msg_id() { return ++cur_msg_id; }
    bool initialize_unit(Unit &unit, bool require_card_model);
    bool init_crypt(Unit &unit);
    bool read_status(Unit &unit, int player);
    bool set_comm_state(DWORD BaudRate);
    bool wait_for_handshake();
    bool msg_write(Message msg);

    std::vector<Message> msg_write_read(Message msg);
    std::vector<Message> msg_write_cmd_read(
        const Unit &unit, uint8_t cmd, const std::vector<uint8_t> &data = {});
    std::vector<Message> msg_read();
};

void start_reader_thread(const std::string &serial_str, int id, bool discover_chain = false);
void stop_reader_thread();
