#include <gfx/terminal.hpp>
#include <gfx/framebuffer.hpp>
#include <cpu/cpu.hpp>
#include <mem/pmm.hpp>
#include <mem/vmm.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdlib.hpp>
#include <klib/algorithm.hpp>

namespace gfx {
    // these are linked using objcopy
    extern "C" u8 _binary_ter_u16n_psf_start[];
    extern "C" u8 _binary_ter_u16n_psf_end[];
    extern "C" u8 _binary_ter_u16b_psf_start[];
    extern "C" u8 _binary_ter_u16b_psf_end[];

    constexpr u32 rgb(u8 r, u8 g, u8 b) {
        return (r << 16) | (g << 8) | b;
    }

    static constexpr u32 color_table[16] = {
        rgb(  0,   0,   0), // Black
        rgb(205,  49,  49), // Red
        rgb( 13, 188, 121), // Green
        rgb(229, 229,  16), // Yellow
        rgb( 36, 114, 200), // Blue
        rgb(188,  63, 188), // Magenta
        rgb( 17, 168, 205), // Cyan
        rgb(229, 229, 229), // White
        rgb(102, 102, 102), // Bright Black
        rgb(241,  76,  76), // Bright Red
        rgb( 35, 209, 139), // Bright Green
        rgb(245, 245,  67), // Bright Yellow
        rgb( 59, 142, 234), // Bright Blue
        rgb(214, 112, 214), // Bright Magenta
        rgb( 41, 184, 219), // Bright Cyan
        rgb(229, 229, 229)  // Bright White
    };

    static constexpr u32 default_fg_color = 0xFCFCFC;
    static constexpr u32 default_bg_color = rgb(24, 24, 24);

    static bool terminal_ready = false;

    Terminal& kernel_terminal() {
        static Terminal term(&gfx::main_framebuffer);
        return term;
    }

    bool is_kernel_terminal_ready() {
        return terminal_ready;
    }

    void set_kernel_terminal_ready() {
        terminal_ready = true;
    }

    int Font::init(void *header) {
        auto *psf1 = (PSF1Header*)header;
        if (psf1->magic == PSF1Header::expected_magic) {
            type = PSF1;
            data = (u8*)psf1 + sizeof(PSF1Header);
            width = 8;
            height = 16;
            num_glyph = (psf1->font_mode & 1) ? 512 : 256;
            bytes_per_glyph = psf1->character_size;
            return 0;
        }
        auto *psf2 = (PSF2Header*)header;
        if (psf2->magic == PSF2Header::expected_magic) {
            type = PSF2;
            data = (u8*)psf2 + psf2->header_size;
            width = psf2->width;
            height = psf2->height;
            num_glyph = psf2->num_glyph;
            bytes_per_glyph = psf2->bytes_per_glyph;
            return 0;
        }
        return -1;
    }

    void Terminal::draw_char_at(usize c_x, usize c_y, char c, u32 fg, u32 bg) {
        Font *font = &regular_font;
        if (fg & bold_flag)
            font = &bold_font;
        draw_psf_char(font, c, c_x, c_y, actual_x, actual_y, fg & 0xFFFFFF, bg);
    }

    void Terminal::set_char_at(usize c_x, usize c_y, char c, u32 fg, u32 bg) {
        usize index = c_y * width_chars + c_x;
        if (text_buffer[index] == c && fg_color_buffer[index] == fg && bg_color_buffer[index] == bg)
            return;
        text_buffer[index] = c;
        fg_color_buffer[index] = fg;
        bg_color_buffer[index] = bg;
        draw_char_at(c_x, c_y, c, fg, bg);
        cursor_needs_undrawing = false;
    }

    void Terminal::set_char_at_cursor(char c) {
        set_char_at(cursor_x, cursor_y, c, current_fg | (is_bold ? bold_flag : 0), current_bg);
    }

    void Terminal::scroll() {
        for (int y = 0; y < height_chars - 1; y++) {
            for (int x = 0; x < width_chars; x++) {
                usize index = (y + 1) * width_chars + x;
                set_char_at(x, y, text_buffer[index], fg_color_buffer[index], bg_color_buffer[index]);
            }
        }

        for (int x = 0; x < width_chars; x++)
            set_char_at(x, height_chars - 1, ' ', default_fg_color, default_bg_color);

        cursor_needs_undrawing = true;
    }

    void Terminal::redraw_cursor() {
        if (cursor_needs_undrawing) {
            gfx::main_framebuffer.fill_rect(
                old_cursor_x * font_width + actual_x, 
                old_cursor_y * font_height + actual_y, 
                1, font_height, current_bg
            );

            usize index = old_cursor_y * width_chars + old_cursor_x;
            draw_char_at(old_cursor_x, old_cursor_y, text_buffer[index], fg_color_buffer[index], bg_color_buffer[index]);
        }
        
        gfx::main_framebuffer.fill_rect(
            cursor_x * font_width + actual_x,
            cursor_y * font_height + actual_y,
            1, font_height, current_fg
        );

        old_cursor_x = cursor_x;
        old_cursor_y = cursor_y;
    }

    void Terminal::move_cursor(int x, int y) {
        cursor_x = x;
        cursor_y = y;
        cursor_needs_undrawing = true;
        redraw_cursor();
    }

    Terminal::Terminal(Framebuffer *fb) {
        framebuffer = fb;

        regular_font.init(_binary_ter_u16n_psf_start);
        bold_font.init(_binary_ter_u16b_psf_start);

        terminal_x = 0;
        terminal_y = 0;
        terminal_width = framebuffer->width;
        terminal_height = framebuffer->height;

        padding_top = 4;
        padding_bottom = 4;
        padding_left = 4;
        padding_right = 4;

        actual_width = terminal_width - padding_left - padding_right;
        actual_height = terminal_height - padding_top - padding_bottom;
        actual_x = terminal_x + padding_left;
        actual_y = terminal_y + padding_top;

        current_fg = default_fg_color;
        current_bg = default_bg_color;
        border = 0x232627;

        font_width = 8;
        font_height = 16;

        width_chars = actual_width / font_width;
        height_chars = actual_height / font_height;

        usize buffer_size = width_chars * height_chars;
        text_buffer = new char[buffer_size];
        fg_color_buffer = new u32[buffer_size];
        bg_color_buffer = new u32[buffer_size];
        for (usize i = 0; i < buffer_size; i++) {
            text_buffer[i] = ' ';
            fg_color_buffer[i] = default_fg_color;
            bg_color_buffer[i] = default_bg_color;
        }

        cursor_x = 0;
        cursor_y = 0;

        framebuffer->fill_rect(actual_x, actual_y, actual_width, actual_height, current_bg);

        // draw the border
        framebuffer->fill_rect(terminal_x, terminal_y, terminal_width, padding_top, border);
        framebuffer->fill_rect(terminal_x, terminal_height - padding_bottom, terminal_width, padding_bottom, border);
        framebuffer->fill_rect(terminal_x, padding_top, padding_left, actual_height, border);
        framebuffer->fill_rect(terminal_width - padding_right, padding_top, padding_right, actual_height, border);

        redraw_cursor();
    }

    Terminal::~Terminal() {
        delete[] text_buffer;
        delete[] fg_color_buffer;
        delete[] bg_color_buffer;
    }
    
    void Terminal::reset_control_sequence() {
        csi_progress = 0;
        csi_arg_index = 0;
        memset(csi_args, 0, sizeof(csi_args));
        csi_question_mark = false;
    }
    
    void Terminal::apply_control_sequence(char code) {
        if (code == 'm') {
            for (int i = 0; i < csi_arg_index + 1; i++) {
                int n = csi_args[i];
                if (n == 0) {
                    current_fg = default_fg_color;
                    current_bg = default_bg_color;
                    is_bold = false;
                } else if (n == 1) {
                    is_bold = true;
                } else if (n == 7) {
                    klib::swap(current_fg, current_bg);
                } else if (n == 22) {
                    is_bold = false;
                } else if (n >= 30 && n <= 37) {
                    current_fg = color_table[n - 30];
                } else if (n >= 90 && n <= 97) {
                    current_fg = color_table[n + 8 - 90];
                } else if (n == 39) {
                    current_fg = default_fg_color;
                } else if (n >= 40 && n <= 47) {
                    current_bg = color_table[n - 40];
                } else if (n == 49) {
                    current_bg = default_bg_color;
                } else if (n >= 100 && n <= 107) {
                    current_bg = color_table[n + 8 - 100];
                }
            }
        } else if (code == 'J') {
            int n = csi_args[0];
            if (n == 0) {
                for (int y = 0; y < height_chars; y++)
                    for (int x = 0; x < width_chars; x++)
                        if ((y + 1) * width_chars + x >= (cursor_y + 1) * width_chars + cursor_x)
                            set_char_at(x, y, ' ', default_fg_color, default_bg_color);
            } else if (n == 1) {
                for (int y = 0; y < height_chars; y++)
                    for (int x = 0; x < width_chars; x++)
                        if ((y + 1) * width_chars + x <= (cursor_y + 1) * width_chars + cursor_x)
                            set_char_at(x, y, ' ', default_fg_color, default_bg_color);
            } else if (n == 2 || n == 3) {
                for (int y = 0; y < height_chars; y++)
                    for (int x = 0; x < width_chars; x++)
                        set_char_at(x, y, ' ', default_fg_color, default_bg_color);
            }
        } else if (code == 'K') {
            int n = csi_args[0];
            if (n == 0) {
                for (int x = cursor_x; x < width_chars; x++)
                    set_char_at(x, cursor_y, ' ', default_fg_color, default_bg_color);
            } else if (n == 1) {
                for (int x = cursor_x; x >= 0; x--)
                    set_char_at(x, cursor_y, ' ', default_fg_color, default_bg_color);
            } else if (n == 2) {
                for (int x = 0; x < width_chars; x++)
                    set_char_at(x, cursor_y, ' ', default_fg_color, default_bg_color);
            }
        } else if (csi_question_mark && code == 'h') {
        } else if (csi_question_mark && code == 'l') {
        } else if (code == 'A') {
            move_cursor(cursor_x, klib::clamp(cursor_y - klib::max(csi_args[0], 1), 0, height_chars - 1));
        } else if (code == 'B') {
            move_cursor(cursor_x, klib::clamp(cursor_y + klib::max(csi_args[0], 1), 0, height_chars - 1));
        } else if (code == 'C') {
            move_cursor(klib::clamp(cursor_x + klib::max(csi_args[0], 1), 0, width_chars - 1), cursor_y);
        } else if (code == 'D') {
            move_cursor(klib::clamp(cursor_x - klib::max(csi_args[0], 1), 0, width_chars - 1), cursor_y);
        } else if (code == 'E') {
            move_cursor(0, klib::clamp(cursor_y + klib::max(csi_args[0], 1), 0, height_chars - 1));
        } else if (code == 'F') {
            move_cursor(0, klib::clamp(cursor_y - klib::max(csi_args[0], 1), 0, height_chars - 1));
        } else if (code == 'G') {
            move_cursor(klib::clamp(csi_args[0] - 1, 0, width_chars - 1), cursor_y);
        } else if (code == 'H' || code == 'f') {
            move_cursor(klib::clamp(csi_args[0] - 1, 0, width_chars - 1), klib::clamp(csi_args[1] - 1, 0, height_chars - 1));
        } else if (code == 'd') {
            move_cursor(cursor_x, klib::clamp(csi_args[0] - 1, 0, height_chars - 1));
        } else if (code == 'r') {
            move_cursor(0, 0);
        }
    }

    void Terminal::write_char(char c) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(lock);

        if (csi_progress == 1) {
            if (c == '[')
                csi_progress = 2;
            else
                reset_control_sequence();
            return;
        }

        if (csi_progress == 2) {
            if (c >= '0' && c <= '9') {
                csi_args[csi_arg_index] = (csi_args[csi_arg_index] * 10) + (c - '0');
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                apply_control_sequence(c);
                reset_control_sequence();
            } else if (c == ';') {
                csi_arg_index++;
                if (csi_arg_index >= max_csi_args)
                    reset_control_sequence();
                csi_args[csi_arg_index] = 0;
            } else if (c == '?') {
                csi_question_mark = true;
            } else
                reset_control_sequence();
            return;
        }

        switch (c) {
        case '\b':
            if (cursor_x > 0) {
                cursor_x--;
                cursor_needs_undrawing = true;
                redraw_cursor();
            } else cursor_needs_undrawing = false;
            return;
        case '\t':
            cursor_x = klib::align_up<usize, 8>(cursor_x + 1);
            break;
        case '\r':
            cursor_x = 0;
            break;
        case '\n':
            cursor_x = 0;
            cursor_y++;
            cursor_needs_undrawing = true;
            break;
        case '\e':
            csi_progress = 1;
            break;
        case 0x7:
            break;
        default:
            set_char_at_cursor(c);
            cursor_x++;
        }

        if (cursor_x >= width_chars) {
            cursor_x = 0;
            cursor_y++;
        }
        
        if (cursor_y >= height_chars) {
            scroll();
            cursor_y--;
        }

        redraw_cursor();
        cursor_needs_undrawing = true;
    }

    // taken from wiki.osdev.org
    // c is a unicode character, cx and cy are cursor position in characters, offx and offy are offsets in pixels
    void Terminal::draw_psf_char(Font *font, u32 c, u16 cx, u16 cy, u16 offx, u16 offy, u32 fg, u32 bg) {
        u32 bytes_per_line = (font->width + 7) / 8;
        u8 *glyph = font->data + (c < font->num_glyph ? c : 0) * font->bytes_per_glyph;
        u64 where = ((cy * font->height + offy) * framebuffer->pitch) + ((cx * font->width + offx) * 4);

        usize line, mask;
        for (usize y = 0; y < font->height; y++) {
            line = where;
            mask = 1 << (font->width - 1);

            for (usize x = 0; x < font->width; x++) {
                *((u32*)(framebuffer->addr + line)) = *glyph & mask ? fg : bg;
                mask >>= 1;
                line += framebuffer->pixel_width;
            }

            glyph += bytes_per_line;
            where += framebuffer->pitch;
        }
    }
}
