#pragma once

#include <klib/timespec.hpp>
#include <klib/bitmap.hpp>
#include <klib/ring_buffer.hpp>
#include <sched/event.hpp>
#include <linux/input.h>

namespace dev::input {
    struct InputEvent {
        struct timeval time;
        u16 type;
        u16 code;
        u32 value;
    };
    static_assert(sizeof(InputEvent) == 24);

    struct InputDevice;

    struct InputListener {
        const char *name;
        void (*callback)(void *priv);
        void *priv;
        klib::ListHead listeners_link;
        klib::RingBuffer<InputEvent, 128> event_buffer;

        isize read(InputDevice *device, InputEvent *buf, usize count, bool blocking = true);
        isize poll(InputDevice *device, isize events);
    };

    struct InputDevice {
        const char *name = "(unspecified)";
        const char *phys = "(unspecified)";
        const char *uniq = "(unspecified)";
        struct input_id id;

        sched::Event event;
        klib::ListHead listeners_list;
        klib::RingBuffer<InputEvent, 128> event_buffer;

        klib::Bitmap<INPUT_PROP_CNT> prop_bitmap;

        klib::Bitmap<EV_CNT>   ev_bitmap;
        klib::Bitmap<KEY_CNT> key_bitmap;
        klib::Bitmap<REL_CNT> rel_bitmap;
        klib::Bitmap<ABS_CNT> abs_bitmap;
        klib::Bitmap<MSC_CNT> msc_bitmap;
        klib::Bitmap<LED_CNT> led_bitmap;
        klib::Bitmap<SND_CNT> snd_bitmap;
        klib::Bitmap<FF_CNT>   ff_bitmap;
        klib::Bitmap<SW_CNT>   sw_bitmap;

        klib::Bitmap<KEY_CNT> key_status;
        klib::Bitmap<LED_CNT> led_status;
        klib::Bitmap<SND_CNT> snd_status;
        klib::Bitmap<SW_CNT>   sw_status;

        InputDevice();

        InputListener* create_listener(const char *name, void (*callback)(void *priv) = nullptr, void *priv = nullptr);
        void remove_listener(InputListener *listener);

    protected:
        void push_event(u16 type, u16 code, u32 value);
        void flush_events();
    };

    struct KeyboardDevice : public InputDevice {
        bool caps_lock = false;

        inline bool is_ctrl()  { return key_status.get(KEY_LEFTCTRL)  || key_status.get(KEY_RIGHTCTRL);  }
        inline bool is_shift() { return key_status.get(KEY_LEFTSHIFT) || key_status.get(KEY_RIGHTSHIFT); }
        inline bool is_alt()   { return key_status.get(KEY_LEFTALT)   || key_status.get(KEY_RIGHTALT);   }
        inline bool is_caps()  { return caps_lock ^ is_shift(); }

        KeyboardDevice() {}
    };

    struct MouseDevice : public InputDevice {
        MouseDevice() {}
    };

    extern KeyboardDevice *main_keyboard;
    extern MouseDevice *main_mouse;

    void init();
}
