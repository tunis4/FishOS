#include <gfx/framebuffer.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <linux/fb.h>

#define PSF_FONT_MAGIC 0x864ab572

namespace gfx {
    Framebuffer main_framebuffer;

    void Framebuffer::from_limine_fb(const struct limine_framebuffer *fb) {
        addr = (u8*)fb->address;
        width = (u32)fb->width;
        height = (u32)fb->height;
        pitch = (u32)fb->pitch;
        pixel_width = (u32)fb->bpp / 8;
        red_mask_size = fb->red_mask_size;
        red_mask_shift = fb->red_mask_shift;
        green_mask_size = fb->green_mask_size;
        green_mask_shift = fb->green_mask_shift;
        blue_mask_size = fb->blue_mask_size;
        blue_mask_shift = fb->blue_mask_shift;
    }

    void Framebuffer::put_pixel(u16 x, u16 y, u32 rgb) {
        if (x > width || y > height) return;
        u64 where = x * pixel_width + y * pitch;
        addr[where] = rgb & 255; // blue
        addr[where + 1] = (rgb >> 8) & 255; // green
        addr[where + 2] = (rgb >> 16) & 255; // red
    }

    void Framebuffer::fill_rect(u16 x, u16 y, u16 w, u16 h, u32 rgb) {
        u8 *where;
        u32 pw = pixel_width;
        for (u16 i = y; i < h + y; i++) {
            if (i >= height) continue;
            where = (u8*)(u64(addr) + pitch * i);
            for (u16 j = x; j < w + x; j++) {
                if (j >= width) continue;
                where[j * pw] = rgb & 255; // blue
                where[j * pw + 1] = (rgb >> 8) & 255; // green
                where[j * pw + 2] = (rgb >> 16) & 255; // red
            }
        }
    }

    FramebufferDevNode::FramebufferDevNode() {
        fb = &main_framebuffer;
        size = fb->height * fb->pitch;
    }

    isize FramebufferDevNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        if (offset >= size)
            return 0;
        usize actual_count = offset + count > size ? size - offset : count;
        memcpy(buf, fb->addr + offset, actual_count);
        return actual_count;
    }

    isize FramebufferDevNode::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        if (offset >= size)
            return 0;
        usize actual_count = offset + count > size ? size - offset : count;
        memcpy(fb->addr + offset, buf, actual_count);
        return actual_count;
    }

    isize FramebufferDevNode::mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) {
        sched::Process *process = cpu::get_current_thread()->process;
        u64 page_flags = mem::mmap_prot_to_page_flags(prot);
        process->pagemap->map_direct(addr, length, page_flags | PAGE_WRITE_COMBINING, (uptr)fb->addr - mem::hhdm);
        return addr;
    }

    isize FramebufferDevNode::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case FBIOBLANK:
        case FBIOPUT_VSCREENINFO:
            return 0;
        case FBIOGET_FSCREENINFO: {
            auto *fi = (struct fb_fix_screeninfo*)arg;
            klib::strcpy(fi->id, "Limine FB");
            fi->smem_len = fb->pitch * fb->height;
            fi->type = FB_TYPE_PACKED_PIXELS;
            fi->visual = FB_VISUAL_TRUECOLOR;
            fi->line_length = fb->pitch;
            fi->mmio_len = fb->pitch * fb->height;
        } return 0;
        case FBIOGET_VSCREENINFO: {
            auto *vi = (struct fb_var_screeninfo*)arg;
            vi->xres = fb->width;
            vi->yres = fb->height;
            vi->xres_virtual = fb->width;
            vi->yres_virtual = fb->height;
            vi->bits_per_pixel = fb->pixel_width * 8;
            vi->red.msb_right = 1;
            vi->green.msb_right = 1;
            vi->blue.msb_right = 1;
            vi->transp.msb_right = 1;
            vi->red.offset = fb->red_mask_shift;
            vi->red.length = fb->red_mask_size;
            vi->blue.offset = fb->blue_mask_shift;
            vi->blue.length = fb->blue_mask_size;
            vi->green.offset = fb->green_mask_shift;
            vi->green.length = fb->green_mask_size;
            vi->height = -1;
            vi->width = -1;
        } return 0;
        }
        return vfs::VNode::ioctl(fd, cmd, arg);
    }
}
