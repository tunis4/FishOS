#include <dev/input/ps2/ps2.hpp>
#include <klib/cstdio.hpp>

namespace dev::input::ps2 {
    static bool device_reset_self_test(int port) {
        // if (!device_command_check(port, DEVICE_CMD_DISABLE_SCANNING)) {
        //     klib::printf("PS/2: Failed to disable scanning before resetting port %d\n", port);
        //     return false;
        // }
        flush_out_buffer();
        device_command(port, DEVICE_CMD_RESET_SELF_TEST);

        int resends = 0;
        while (true) {
            u8 result = read_data();
            // klib::printf("PS/2: Reset self test: read %#X\n", (u32)result);
            if (result == RESEND) {
                if (resends > 10) {
                    klib::printf("PS/2: Too many resends for reset self test\n");
                    return false;
                }
                device_command(port, DEVICE_CMD_RESET_SELF_TEST);
                resends++;
                continue;
            }
            if (result == DEVICE_SELF_TEST_OK)
                break;
            if (result == DEVICE_SELF_TEST_FAIL) {
                klib::printf("PS/2: Self test failed\n");
                return false;
            }
        }
    
        flush_out_buffer();
        return true;
    }

    void init() {
        write_command(CTRL_CMD_DISABLE_P1);
        write_command(CTRL_CMD_DISABLE_P2);
        flush_out_buffer();

        write_command(CTRL_CMD_READ_CONFIG);
        u8 config = read_data();

        config &= ~(CONFIG_IRQ_P1 | CONFIG_CLOCK_DISABLE_P1 | CONFIG_TRANSLATION);

        write_command(CTRL_CMD_WRITE_CONFIG);
        write_data(config);

        write_command(CTRL_CMD_SELF_TEST);
        u8 result = read_data();

        if (result != CTRL_SELF_TEST_OK) {
            klib::printf("PS/2: Controller self test failed (expected %#X, got %#X)\n", CTRL_SELF_TEST_OK, result);
            return;
        }

        write_command(CTRL_CMD_WRITE_CONFIG);
        write_data(config); // restore config if it was reset

        // determine if there is a second port
        bool dual_port = true;
        write_command(CTRL_CMD_ENABLE_P2);
        write_command(CTRL_CMD_READ_CONFIG);
        config = read_data();

        if (config & CONFIG_CLOCK_DISABLE_P2) { // not enabled, not dual port.
            dual_port = false;
        } else {
            write_command(CTRL_CMD_DISABLE_P2);

            config &= ~(CONFIG_IRQ_P2 | CONFIG_CLOCK_DISABLE_P2);

            write_command(CTRL_CMD_WRITE_CONFIG);
            write_data(config);
        }

        // do port self tests
        int working_flag = 0;

        write_command(CTRL_CMD_P1_SELF_TEST);
        result = read_data();
        if (result == 0)
            working_flag |= 1;
        else
            klib::printf("PS/2: Port 1 self test failed (expected 0, got %#X)\n", result);

        if (dual_port) {
            write_command(CTRL_CMD_P2_SELF_TEST);
            result = read_data();
            if (result == 0)
                working_flag |= 2;
            else
                klib::printf("PS/2: Port 2 self test failed (expected 0, got %#X)\n", result);
        }

        if (working_flag == 0) {
            klib::printf("PS/2: No working ports\n");
            return;
        }

        klib::printf("PS/2: Controller with %u ports\n", dual_port ? 2 : 1);

        // reset and self test devices
        // the ports leave this with scanning disabled

        if (working_flag & 1)
            write_command(CTRL_CMD_ENABLE_P1);
        if (working_flag & 2)
            write_command(CTRL_CMD_ENABLE_P2);

        write_command(CTRL_CMD_READ_CONFIG);
        config = read_data();
        if (working_flag & 1)
            config |= CONFIG_IRQ_P1;
        if (working_flag & 2)
            config |= CONFIG_IRQ_P2;
        write_command(CTRL_CMD_WRITE_CONFIG);
        write_data(config);

        int connected_flag = 0;

        if (working_flag & 1) {
            if (!device_reset_self_test(1))
                klib::printf("PS/2: Device self test for port 1 failed\n");
            else
                connected_flag |= 1;
        }

        if (working_flag & 2) {
            if (!device_reset_self_test(2))
                klib::printf("PS/2: Device self test for port 2 failed\n");
            else
                connected_flag |= 2;
        }

        if (connected_flag == 0) {
            klib::printf("PS/2: No working devices\n");
            return;
        }

        int ok_flag = 0;

        // now initialize and enable the device stuff
        if (connected_flag & 1) {
            u8 identity;
            if (!identify(1, &identity)) {
                klib::printf("PS/2: Failed to identify port 1\n");
            } else {
                main_keyboard = new ps2::Keyboard();
                ok_flag |= 1;
            }
        }

        if (connected_flag & 2) {
            u8 identity;
            if (!identify(2, &identity)) {
                klib::printf("PS/2: Failed to identify port 2\n");
            } else {
                main_mouse = new ps2::Mouse();
                ok_flag |= 2;
            }
        }

        if (ok_flag & 1)
            config |= CONFIG_IRQ_P1;
        if (ok_flag & 2)
            config |= CONFIG_IRQ_P2;
        config |= CONFIG_TRANSLATION;
        write_command(CTRL_CMD_WRITE_CONFIG);
        write_data(config);
    }

    bool out_buffer_empty() {
        return (cpu::in<u8>(PORT_STATUS) & 1) == 0;
    }

    bool in_buffer_full() {
        return (cpu::in<u8>(PORT_STATUS) & 2) == 2;
    }

    void flush_out_buffer() {
        while (!out_buffer_empty())
            cpu::in<u8>(PORT_DATA);
    }

    void write_command(u8 cmd) {
        while (in_buffer_full());
        cpu::out<u8>(PORT_COMMAND, cmd);
    }

    void write_data(u8 data) {
        while (in_buffer_full());
        cpu::out<u8>(PORT_DATA, data);
    }

    u8 read_data() {
        while (out_buffer_empty());
        return cpu::in<u8>(PORT_DATA);
    }

    void device_command(int port, u8 command) {
        if (port == 2)
            write_command(CTRL_CMD_SECOND_PORT_SELECT);
        write_data(command);
    }

    bool device_command_check(int port, u8 command) {
        device_command(port, command);
        int resends = 0;
        while (true) {
            u8 data = read_data();
            if (data == RESEND) {
                if (resends++ > 5)
                    return false;
                device_command(port, command);
            }
            if (data == ACK)
                return true;
        }
    }

    bool identify(int port, u8 *result) {
        if (!device_command_check(port, DEVICE_CMD_DISABLE_SCANNING)) {
            klib::printf("PS/2: Failed to disable scanning before identifying port %d\n", port);
            return false;
        }
        flush_out_buffer();

        if (!device_command_check(port, DEVICE_CMD_IDENTIFY))
            return false;
        *result = read_data();

        flush_out_buffer();
        if (!device_command_check(port, DEVICE_CMD_ENABLE_SCANNING)) {
            klib::printf("PS/2: Failed to enable scanning after identifying port %d\n", port);
            return false;
        }

        flush_out_buffer();
        return true;
    }
}
