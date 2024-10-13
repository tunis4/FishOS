#include <dev/input/ps2/ps2.hpp>
#include <klib/cstdio.hpp>

namespace dev::input::ps2 {
    static void set_rate(u8 rate) {
        if (!device_command_check(2, DEVICE_CMD_SAMPLE_RATE))
            goto fail;
        if (!device_command_check(2, rate))
            goto fail;
        return;
    fail:
        klib::printf("PS/2 Mouse: Failed to set rate\n");
    }

    Mouse::Mouse() {
        name = "PS/2 Mouse";
        phys = "ps2mouse/input0";
        id = { .bustype = BUS_I8042, .vendor = 2, .product = 1, .version = 1 };

        u8 identity;
        identify(2, &identity);
        ASSERT(identity == IDENTITY_MOUSE);

        set_rate(200);
        set_rate(100);
        set_rate(80);
        identify(2, &identity);
        if (identity == IDENTITY_MOUSE_WHEEL) {
            has_wheel = true;
            set_rate(200);
            set_rate(200);
            set_rate(80);
            identify(2, &identity);
            if (identity == IDENTITY_MOUSE_FIVE_BUTTONS)
                has_five_buttons = true;
        }

        set_rate(200);

        ev_bitmap.set(EV_SYN, true);

        ev_bitmap.set(EV_KEY, true);
        key_bitmap.set(BTN_LEFT, true);
        key_bitmap.set(BTN_RIGHT, true);
        key_bitmap.set(BTN_MIDDLE, true);
        if (has_five_buttons) {
            key_bitmap.set(BTN_SIDE, true);
            key_bitmap.set(BTN_EXTRA, true);
        }

        ev_bitmap.set(EV_REL, true);
        rel_bitmap.set(REL_X, true);
        rel_bitmap.set(REL_Y, true);
        if (has_wheel)
            rel_bitmap.set(REL_WHEEL, true);

        cpu::interrupts::register_irq(12, [] (void *priv, cpu::InterruptState *state) {
            ((Mouse*)priv)->irq();
        }, this);
        flush_out_buffer();
    }

    void Mouse::process_button(usize index, bool state) {
        bool old_state = key_status.get(index);
        if (state != old_state) {
            key_status.set(index, state);
            push_event(EV_KEY, index, state);
        }
    }

    void Mouse::irq() {
        defer { cpu::interrupts::eoi(); };

        data[data_count] = read_data();

        // check if packet is bad
        if ((data[0] & 8) == 0) {
            data_count = 0;
            return;
        }

        data_count++;
        if (!((has_wheel && data_count == 4) || (has_wheel == false && data_count == 3)))
            return;
        data_count = 0;

        process_button(BTN_LEFT, data[0] & 1);
        process_button(BTN_RIGHT, data[0] & 2);
        process_button(BTN_MIDDLE, data[0] & 4);
        if (has_five_buttons) {
            process_button(BTN_SIDE, data[3] & 0x10);
            process_button(BTN_EXTRA, data[3] & 0x20);
        }

        if (int x = data[1] - ((data[0] & 0x10) ? 0x100 : 0); x != 0)
            push_event(EV_REL, REL_X, x);
        if (int y = data[2] - ((data[0] & 0x20) ? 0x100 : 0); y != 0)
            push_event(EV_REL, REL_Y, -y);
        if (has_wheel)
            if (int wheel = (data[3] & 0x7) * ((data[3] & 0x8) ? 1 : -1); wheel != 0)
                push_event(EV_REL, REL_WHEEL, wheel);

        flush_events();
    }
}
