#pragma once

#include <dev/input/input.hpp>
#include <cpu/interrupts/interrupts.hpp>

namespace dev::input::ps2 {
    void init();

    struct Keyboard : public KeyboardDevice {
        bool extended = false;

        Keyboard();

    private:
        void irq();
    };

    struct Mouse : public MouseDevice {
        bool has_wheel = false;
        bool has_five_buttons = false;
        u8 data[4] = {};
        int data_count = 0;

        Mouse();

    private:
        void irq();
        void process_button(usize index, bool state);
    };

    constexpr u16 PORT_DATA = 0x60;
    constexpr u16 PORT_COMMAND = 0x64;
    constexpr u16 PORT_STATUS = 0x64;

    constexpr u8 ACK = 0xfa;
    constexpr u8 RESEND = 0xfe;
    constexpr u8 CTRL_SELF_TEST_OK = 0x55;
    constexpr u8 DEVICE_SELF_TEST_OK = 0xaa;
    constexpr u8 DEVICE_SELF_TEST_FAIL = 0xfc;

    constexpr u8 CONFIG_IRQ_P1 = 1 << 0;
    constexpr u8 CONFIG_IRQ_P2 = 1 << 1;
    constexpr u8 CONFIG_CLOCK_DISABLE_P1 = 1 << 4;
    constexpr u8 CONFIG_CLOCK_DISABLE_P2 = 1 << 5;
    constexpr u8 CONFIG_TRANSLATION = 1 << 6;

    constexpr u8 CTRL_CMD_SECOND_PORT_SELECT = 0xd4;
    constexpr u8 CTRL_CMD_ENABLE_P1 = 0xae;
    constexpr u8 CTRL_CMD_ENABLE_P2 = 0xa8;
    constexpr u8 CTRL_CMD_DISABLE_P1 = 0xad;
    constexpr u8 CTRL_CMD_DISABLE_P2 = 0xa7;
    constexpr u8 CTRL_CMD_READ_CONFIG = 0x20;
    constexpr u8 CTRL_CMD_WRITE_CONFIG = 0x60;
    constexpr u8 CTRL_CMD_SELF_TEST = 0xaa;
    constexpr u8 CTRL_CMD_P1_SELF_TEST = 0xab;
    constexpr u8 CTRL_CMD_P2_SELF_TEST = 0xa9;

    constexpr u8 DEVICE_CMD_IDENTIFY = 0xf2;
    constexpr u8 DEVICE_CMD_RESET_SELF_TEST = 0xff;
    constexpr u8 DEVICE_CMD_ENABLE_SCANNING = 0xf4;
    constexpr u8 DEVICE_CMD_DISABLE_SCANNING = 0xf5;
    constexpr u8 DEVICE_CMD_SAMPLE_RATE = 0xf3;

    constexpr u8 IDENTITY_MOUSE = 0;
    constexpr u8 IDENTITY_MOUSE_WHEEL = 3;
    constexpr u8 IDENTITY_MOUSE_FIVE_BUTTONS = 4;

    bool out_buffer_empty();
    bool in_buffer_full();
    void flush_out_buffer();
    void write_command(u8 cmd);
    void write_data(u8 data);
    u8 read_data();
    void device_command(int port, u8 command);
    bool device_command_check(int port, u8 command);
    bool identify(int port, u8 *result);
}
