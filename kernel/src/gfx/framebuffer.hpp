#pragma once

#include <klib/common.hpp>
#include <klib/cstring.hpp>
#include <dev/devnode.hpp>
#include <limine.hpp>

namespace gfx {
    struct Framebuffer {
        u8 *addr;
        u32 width, height, pitch, pixel_width;
        u8 red_mask_size, red_mask_shift;
        u8 green_mask_size, green_mask_shift;
        u8 blue_mask_size, blue_mask_shift;

        void from_limine_fb(const struct limine_framebuffer *fb);
        void put_pixel(u16 x, u16 y, u32 color);
        void fill_rect(u16 x, u16 y, u16 w, u16 h, u32 color);
    };

    extern Framebuffer main_framebuffer;

    struct FramebufferDevNode final : public dev::SeekableCharDevNode {
        Framebuffer *fb;
        usize size;

        FramebufferDevNode();
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg) override;
        isize mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) override;
    };
}
