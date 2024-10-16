#include <dev/input/input.hpp>
#include <dev/input/ps2/ps2.hpp>
#include <sched/time.hpp>
#include <sys/poll.h>
#include <errno.h>

namespace dev::input {
    KeyboardDevice *main_keyboard = nullptr;
    MouseDevice *main_mouse = nullptr;

    void init() {
        ps2::init();
    }

    InputDevice::InputDevice() {
        listeners_list.init();
    }

    InputListener* InputDevice::create_listener(const char *name, void (*callback)(void *priv), void *priv) {
        auto *listener = new InputListener();
        listener->name = name;
        listener->callback = callback;
        listener->priv = priv;
        listeners_list.add_before(&listener->listeners_link);
        return listener;
    }

    void InputDevice::remove_listener(InputListener *listener) {
        listener->listeners_link.remove();
        delete listener;
    }

    isize InputListener::read(InputDevice *device, InputEvent *buf, usize count, bool blocking) {
        while (event_buffer.is_empty()) {
            if (!blocking)
                return -EWOULDBLOCK;
            if (device->event.await() == -EINTR)
                return -EINTR;
        }
        return event_buffer.read(buf, count);
    }

    isize InputListener::poll(InputDevice *device, isize events) {
        if (events & POLLIN)
            if (!event_buffer.is_empty())
                return POLLIN;
        return 0;
    }
    
    void InputDevice::push_event(u16 type, u16 code, u32 value) {
        InputEvent input_event;
        input_event.time = sched::get_clock(CLOCK_REALTIME).to_timeval();
        input_event.type = type;
        input_event.code = code;
        input_event.value = value;
        event_buffer.write_truncate(&input_event, 1);
    }

    void InputDevice::flush_events() {
        if (!event_buffer.is_empty())
            push_event(EV_SYN, SYN_REPORT, 0);
        InputEvent events[128];
        usize num_events = event_buffer.read(events, sizeof(events) / sizeof(InputEvent));
        InputListener *listener;
        LIST_FOR_EACH(listener, &listeners_list, listeners_link) {
            if (listener->event_buffer.free_count() < num_events) {
                listener->event_buffer.truncate(listener->event_buffer.data_count());
                InputEvent input_event;
                input_event.time = sched::get_clock(CLOCK_REALTIME).to_timeval();
                input_event.type = EV_SYN;
                input_event.code = SYN_DROPPED;
                input_event.value = 0;
                listener->event_buffer.write(&input_event, 1);
            }
            listener->event_buffer.write(events, num_events);
            if (listener->callback)
                listener->callback(listener->priv);
        }
        event.trigger();
    }
}
