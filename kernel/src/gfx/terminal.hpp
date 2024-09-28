#pragma once

#include <klib/common.hpp>
#include <klib/lock.hpp>
#include <gfx/framebuffer.hpp>
#include <dev/device.hpp>

namespace gfx {
    struct Font {
        struct [[gnu::packed]] PSF1Header {
            static constexpr u16 expected_magic = 0x0436;
            u16 magic;
            u8 font_mode;
            u8 character_size;
        };

        struct [[gnu::packed]] PSF2Header {
            static constexpr u32 expected_magic = 0x864ab572;
            u32 magic;
            u32 version;
            u32 header_size;
            u32 flags;
            u32 num_glyph;
            u32 bytes_per_glyph;
            u32 height;
            u32 width;
        };

        enum Type {
            PSF1, PSF2
        };

        Type type;
        u8 *data;
        u32 width, height;
        u32 num_glyph, bytes_per_glyph;

        int init(void *header);
    };

    struct TerminalEmulator {
        static constexpr u32 bold_flag = 1 << 24;

        Framebuffer *framebuffer;
        Font regular_font, bold_font;
        klib::Spinlock lock;

        char *text_buffer; // represents the currently displayed characters
        u32 *fg_color_buffer; // represents the currently displayed foreground colors
        u32 *bg_color_buffer; // represents the currently displayed background colors

        // pixel area of the terminal
        int terminal_width, terminal_height;
        int terminal_x, terminal_y;

        // size of terminal in chars
        int width_chars, height_chars;

        // terminal padding in pixels
        int padding_top, padding_bottom;
        int padding_left, padding_right;

        // pixel area of the terminal, accounting for padding
        int actual_width, actual_height;
        int actual_x, actual_y;

        // color of the terminal
        u32 current_fg, current_bg, border;
        bool is_bold = false;

        // size of the used font
        int font_width, font_height;

        // where to write chars at
        int cursor_x, cursor_y;
        int old_cursor_x, old_cursor_y;

        bool cursor_needs_undrawing = false;

        static constexpr int max_csi_args = 16;
        int csi_progress = 0;
        int csi_args[max_csi_args] = {};
        int csi_arg_index = 0;
        bool csi_question_mark = false;

        void draw_psf_char(Font *font, u32 c, u16 cx, u16 cy, u16 offx, u16 offy, u32 fg, u32 bg);
        void draw_char_at(usize cx, usize cy, char c, u32 fg, u32 bg);
        void set_char_at(usize cx, usize cy, char c, u32 fg, u32 bg);
        void set_char_at_cursor(char c);

        // scrolls down by 1 line
        void scroll();

        void redraw_cursor();
        void move_cursor(int x, int y);

        void reset_control_sequence();
        void apply_control_sequence(char code);

        TerminalEmulator(Framebuffer *fb);
        ~TerminalEmulator();
        
        void write_char(char c);
    };

    TerminalEmulator& kernel_terminal();
    bool is_kernel_terminal_ready();
    void set_kernel_terminal_ready();
}
