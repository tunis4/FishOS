#include <dev/input/ps2/ps2.hpp>

namespace dev::input::ps2 {
    Keyboard::Keyboard() {
        name = "PS/2 Keyboard";
        phys = "ps2kbd/input0";
        id = { .bustype = BUS_I8042, .vendor = 1, .product = 1, .version = 1 };

        ev_bitmap.set(EV_SYN, true);

        ev_bitmap.set(EV_KEY, true);
        for (usize key = 1; key < 128; key++)
            if (key != 84)
                key_bitmap.set(key, true);

        ev_bitmap.set(EV_LED, true);
        led_bitmap.set(LED_NUML, true);
        led_bitmap.set(LED_CAPSL, true);
        led_bitmap.set(LED_SCROLLL, true);

        cpu::interrupts::register_irq(1, [] (void *priv, cpu::InterruptState *state) {
            ((Keyboard*)priv)->irq();
        }, this);
        flush_out_buffer();
    }

    void Keyboard::irq() {
        defer { cpu::interrupts::eoi(); };

        u8 scancode = read_data();
        if (scancode == 0xE0) {
            extended = true;
            return;
        }

        bool release = scancode & 128;
        u16 keycode = scancode & 127;

        if (extended) {
            switch (keycode) {
            case 0x1C: keycode = KEY_KPENTER; break;
            case 0x1D: keycode = KEY_RIGHTCTRL; break;
            case 0x35: keycode = KEY_KPSLASH; break;
            case 0x38: keycode = KEY_RIGHTALT; break;
            case 0x47: keycode = KEY_HOME; break;
            case 0x48: keycode = KEY_UP; break;
            case 0x49: keycode = KEY_PAGEUP; break;
            case 0x4B: keycode = KEY_LEFT; break;
            case 0x4D: keycode = KEY_RIGHT; break;
            case 0x4F: keycode = KEY_END; break;
            case 0x50: keycode = KEY_DOWN; break;
            case 0x51: keycode = KEY_PAGEDOWN; break;
            case 0x52: keycode = KEY_INSERT; break;
            case 0x53: keycode = KEY_DELETE; break;
            }
            extended = false;
        }

        bool is_repeat = !release && key_status.get(keycode);
        if (!is_repeat)
            key_status.set(keycode, !release);

        push_event(EV_KEY, keycode, is_repeat ? 2 : !release);

        flush_events();
    }
}
