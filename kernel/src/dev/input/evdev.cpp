#include <dev/input/evdev.hpp>
#include <klib/cstdio.hpp>

namespace dev::input {
    EventDevNode::EventDevNode(InputDevice *input_device) {
        this->input_device = input_device;
        event = &input_device->event;
    }

    EventDevNode::~EventDevNode() {}

    isize EventDevNode::open(vfs::FileDescription *fd) {
        fd->priv = input_device->create_listener("evdev");
        return 0;
    }

    void EventDevNode::close(vfs::FileDescription *fd) {
        input_device->remove_listener((InputListener*)fd->priv);
    }

    isize EventDevNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        InputListener *listener = (InputListener*)fd->priv;
        bool blocking = !(fd->flags & O_NONBLOCK);
        isize ret = listener->read(input_device, (InputEvent*)buf, count / sizeof(InputEvent), blocking);
        if (ret < 0)
            return ret;
        return ret * sizeof(InputEvent);
    }

    isize EventDevNode::poll(vfs::FileDescription *fd, isize events) {
        InputListener *listener = (InputListener*)fd->priv;
        return listener->poll(input_device, events);
    }

    static usize copy_string(void *target, usize target_size, const char *str) {
        klib::strncpy((char*)target, str, target_size);
        return klib::min(klib::strlen(str) + 1, target_size);
    }

    static usize copy_bitmap(void *target, usize target_size, usize *bitmap, usize bitmap_len) {
        usize copied = klib::min(klib::bits_to<u8>(bitmap_len), target_size);
        memcpy(target, bitmap, copied);
        return copied;
    }

    isize EventDevNode::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case EVIOCGVERSION:
            *(int*)arg = 0x010001;
            return 0;
        case EVIOCGID:
            memcpy(arg, &input_device->id, sizeof(struct input_id));
            return 0;
        case EVIOCGEFFECTS:
        case EVIOCGRAB:
            return 0;
        }

        usize size = _IOC_SIZE(cmd);
#define EVIOC_MASK_SIZE(nr) ((nr) & ~(_IOC_SIZEMASK << _IOC_SIZESHIFT))
        switch (EVIOC_MASK_SIZE(cmd)) {
        case EVIOCGNAME(0): return copy_string(arg, size, input_device->name);
        case EVIOCGPHYS(0): return copy_string(arg, size, input_device->phys);
        case EVIOCGUNIQ(0): return copy_string(arg, size, input_device->uniq);
        case EVIOCGPROP(0): return copy_bitmap(arg, size, input_device->prop_bitmap.data, INPUT_PROP_CNT);
        case  EVIOCGKEY(0): return copy_bitmap(arg, size, input_device->key_status.data, KEY_CNT);
        case  EVIOCGLED(0): return copy_bitmap(arg, size, input_device->led_status.data, LED_CNT);
        case  EVIOCGSND(0): return copy_bitmap(arg, size, input_device->snd_status.data, SND_CNT);
        case   EVIOCGSW(0): return copy_bitmap(arg, size, input_device->sw_status.data, SW_CNT);
        }

        if (_IOC_TYPE(cmd) != 'E')
            return -EINVAL;

        if (_IOC_DIR(cmd) == _IOC_READ && (_IOC_NR(cmd) & ~EV_MAX) == _IOC_NR(EVIOCGBIT(0, 0))) {
            usize len, *bitmap;
            switch (_IOC_NR(cmd) & EV_MAX) {
            case      0: bitmap = input_device->ev_bitmap.data;  len =  EV_MAX; break;
            case EV_KEY: bitmap = input_device->key_bitmap.data; len = KEY_MAX; break;
            case EV_REL: bitmap = input_device->rel_bitmap.data; len = REL_MAX; break;
            case EV_ABS: bitmap = input_device->abs_bitmap.data; len = ABS_MAX; break;
            case EV_MSC: bitmap = input_device->msc_bitmap.data; len = MSC_MAX; break;
            case EV_LED: bitmap = input_device->led_bitmap.data; len = LED_MAX; break;
            case EV_SND: bitmap = input_device->snd_bitmap.data; len = SND_MAX; break;
            case  EV_FF: bitmap = input_device->ff_bitmap.data;  len =  FF_MAX; break;
            case  EV_SW: bitmap = input_device->sw_bitmap.data;  len =  SW_MAX; break;
            default: return -EINVAL;
            }
            return copy_bitmap(arg, size, bitmap, len);
        }

        klib::printf("unimplemented evdev ioctl (masked cmd: %#lX, arg: %#lX)\n", EVIOC_MASK_SIZE(cmd), (uptr)arg);
        return -EINVAL;
    }
}
