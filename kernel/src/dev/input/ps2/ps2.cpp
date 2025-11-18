// This file uses code from the Astral project
// See NOTICE.md for the license of Astral

#include <dev/input/ps2/ps2.hpp>
#include <sched/timer/hpet.hpp>
#include <klib/cstdio.hpp>

namespace dev::input::ps2 {
    // static bool device_reset_self_test(int port) {
    //     // if (!device_command_check(port, DEVICE_CMD_DISABLE_SCANNING)) {
    //     //     klib::printf("PS/2: Failed to disable scanning before resetting port %d\n", port);
    //     //     return false;
    //     // }
    //     flush_out_buffer();
    //     device_command(port, DEVICE_CMD_RESET_SELF_TEST);

    //     int resends = 0;
    //     while (true) {
    //         u8 result = read_data();
    //         // klib::printf("PS/2: Reset self test: read %#X\n", (u32)result);
    //         if (result == RESEND) {
    //             if (resends > 10) {
    //                 klib::printf("PS/2: Too many resends for reset self test\n");
    //                 return false;
    //             }
    //             device_command(port, DEVICE_CMD_RESET_SELF_TEST);
    //             resends++;
    //             continue;
    //         }
    //         if (result == DEVICE_SELF_TEST_OK)
    //             break;
    //         if (result == DEVICE_SELF_TEST_FAIL) {
    //             klib::printf("PS/2: Self test failed\n");
    //             return false;
    //         }
    //     }
    
    //     flush_out_buffer();
    //     return true;
    // }

    void init() {
        write_command(CTRL_CMD_DISABLE_P1);
        write_command(CTRL_CMD_DISABLE_P2);
        flush_out_buffer();

        write_command(CTRL_CMD_READ_CONFIG);
        u8 config = read_data();

        config &= ~(CONFIG_IRQ_P1 | CONFIG_IRQ_P2 | CONFIG_TRANSLATION);

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

            config &= ~CONFIG_IRQ_P2;
            config |= CONFIG_CLOCK_DISABLE_P2;

            write_command(CTRL_CMD_WRITE_CONFIG);
            write_data(config);
        }

        // do port self tests
        int working_flag = 0;

        write_command(CTRL_CMD_P1_SELF_TEST);
        result = read_data();
        if (result == 0) {
            working_flag |= 1;
        } else
            klib::printf("PS/2: Port 1 self test failed (expected 0, got %#X)\n", result);

        if (dual_port) {
            write_command(CTRL_CMD_P2_SELF_TEST);
            result = read_data();
            if (result == 0) {
                working_flag |= 2;
            } else
                klib::printf("PS/2: Port 2 self test failed (expected 0, got %#X)\n", result);
        }

        if (working_flag == 0) {
            klib::printf("PS/2: No working ports\n");
            return;
        }

        klib::printf("PS/2: Controller with %u ports\n", dual_port ? 2 : 1);

        if (working_flag & 1)
            write_command(CTRL_CMD_ENABLE_P1);
        if (working_flag & 2)
            write_command(CTRL_CMD_ENABLE_P2);

        write_command(CTRL_CMD_READ_CONFIG);
        config = read_data();
        if (working_flag & 1)
            config |= CONFIG_IRQ_P1 | CONFIG_TRANSLATION;
        if (working_flag & 2)
            config |= CONFIG_IRQ_P2;
        write_command(CTRL_CMD_WRITE_CONFIG);
        write_data(config);
        main_keyboard = new ps2::Keyboard();
        main_mouse = new ps2::Mouse();
    }

    bool out_buffer_empty() {
        return (cpu::in<u8>(PORT_STATUS) & 1) == 0;
    }

    bool in_buffer_full() {
        return (cpu::in<u8>(PORT_STATUS) & 2) == 2;
    }

    void flush_out_buffer() {
        u8 status;
        while ((status = cpu::in<u8>(PORT_STATUS)) & (1|2))
            cpu::in<u8>(PORT_DATA);
    }

    int write_command(u8 cmd) {
        // klib::printf("write_command(%#X)\n", cmd);
        int i = 0;
        while (in_buffer_full() && i < TIMEOUT) {
            sched::timer::hpet::stall_µs(50);
            i += 50;
        }
        if (i == TIMEOUT) {
            klib::printf("PS/2: Command write timeout\n");
            return -1;
        }
        cpu::out<u8>(PORT_COMMAND, cmd);
        return 0;
    }

    int write_data(u8 data) {
        // klib::printf("write_data(%#X)\n", data);
        int i = 0;
        while (in_buffer_full() && i < TIMEOUT) {
            sched::timer::hpet::stall_µs(50);
            i += 50;
        }
        if (i == TIMEOUT) {
            klib::printf("PS/2: Data write timeout\n");
            return -1;
        }
        cpu::out<u8>(PORT_DATA, data);
        return 0;
    }

    int read_data() {
        int i = 0;
        while (out_buffer_empty() && i < TIMEOUT) {
            sched::timer::hpet::stall_µs(50);
            i += 50;
        }
        if (i == TIMEOUT) {
            klib::printf("PS/2: Data read timeout\n");
            return -1;
        }
        u8 data = cpu::in<u8>(PORT_DATA);
        // klib::printf("read_data() = %#X\n", data);
        return data;
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
