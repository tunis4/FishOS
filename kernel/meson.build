project('fishix', ['cpp', 'c', 'nasm'], default_options: [
    'buildtype=debug', 'warning_level=2', 'cpp_std=gnu++23'
])

fs = import('fs')

add_global_arguments([
    '-g3', '-pipe',
    '-Wno-unused-parameter', '-Wno-missing-field-initializers',

    '-ffreestanding',
    '-fstack-protector',
    '-fno-pie',
    '-fno-pic',
    '-fno-omit-frame-pointer',
    '-march=x86-64',
    '-mabi=sysv',
    '-mno-80387',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2',
    '-mno-red-zone',
    '-mgeneral-regs-only',
    '-mcmodel=kernel',
    '-MMD',
], language: ['cpp', 'c'])

add_global_arguments([
    '-fno-exceptions',
    '-fno-rtti',
    '-fno-use-cxa-atexit',
], language: 'cpp')

add_global_link_arguments([
    '-nostdlib',
    '-static',
    '-z', 'noexecstack',
    '-T' + meson.current_source_dir() / 'linker.ld',
], language: ['cpp', 'c'])

src_include_dir = include_directories('src')

dependencies = []

source_files = [
    'src/kernel.cpp',
    'src/ubsan.cpp',

    'src/acpi/tables.cpp',

    'src/cpu/cpu.cpp',

    'src/cpu/gdt/gdt.cpp',
    'src/cpu/gdt/gdt_flush.asm',

    'src/cpu/interrupts/apic.cpp',
    'src/cpu/interrupts/idt.cpp',
    'src/cpu/interrupts/interrupts.cpp',
    'src/cpu/interrupts/pic.cpp',
    'src/cpu/interrupts/gate_wrappers.asm',

    'src/cpu/syscall/syscall.cpp',
    'src/cpu/syscall/syscall_entry.asm',

    'src/dev/io_service.cpp',
    'src/dev/block.cpp',
    'src/dev/net.cpp',
    'src/dev/devnode.cpp',
    'src/dev/pci.cpp',
    'src/dev/virtio.cpp',

    'src/dev/tty/tty.cpp',
    'src/dev/tty/pty.cpp',
    'src/dev/tty/console.cpp',

    'src/dev/input/input.cpp',
    'src/dev/input/evdev.cpp',
    'src/dev/input/ps2/ps2.cpp',
    'src/dev/input/ps2/keyboard.cpp',
    'src/dev/input/ps2/mouse.cpp',

    'src/fs/initramfs.cpp',
    'src/fs/tmpfs.cpp',
    'src/fs/procfs.cpp',
    'src/fs/vfs.cpp',

    'src/gfx/framebuffer.cpp',
    'src/gfx/terminal.cpp',

    'src/klib/cppruntime.cpp',
    'src/klib/cstdio.cpp',
    'src/klib/cstdlib.cpp',
    'src/klib/cstring.cpp',
    'src/klib/mem.asm',

    'src/mem/bump.cpp',
    'src/mem/pmm.cpp',
    'src/mem/vmem.cpp',
    'src/mem/vmm.cpp',

    'src/sched/sched.cpp',
    'src/sched/context.cpp',
    'src/sched/event.cpp',
    'src/sched/time.cpp',

    'src/sched/timer/apic_timer.cpp',
    'src/sched/timer/hpet.cpp',
    'src/sched/timer/pit.cpp',

    'src/userland/elf.cpp',
    'src/userland/pipe.cpp',
    'src/userland/futex.cpp',
    'src/userland/signal.cpp',
    'src/userland/cred.cpp',
    'src/userland/pid.cpp',
    'src/userland/info.cpp',
    'src/userland/epoll.cpp',
    'src/userland/inotify.cpp',
    'src/userland/eventfd.cpp',

    'src/userland/socket/socket.cpp',
    'src/userland/socket/local/stream.cpp',
    'src/userland/socket/local/datagram.cpp',
    'src/userland/socket/udp.cpp',
    'src/userland/socket/tcp.cpp',
]

fonts = [
    'ter-u16n.psf',
    'ter-u16b.psf',
]

font_copy_targets = []
foreach font : fonts
    font_copy_targets += custom_target(
        'copy font @0@'.format(font.split('/').get(-1)),
        command: [ 'cp', '@INPUT@', '@OUTPUT@' ],
        input: font,
        output: '@PLAINNAME@'
    )
endforeach

ld = find_program('ld')
font_object_targets = []
foreach font_copy_target : font_copy_targets
    font = font_copy_target.full_path().split('/').get(-1)
    font_object_targets += custom_target(
        'font object @0@'.format(font),
        command: [ ld, '--relocatable', '--format', 'binary', '--output', '@OUTPUT@', '@INPUT@' ],
        input: font_copy_target,
        output: '@BASENAME@.o'
    )
endforeach

executable('fishix', [source_files, font_object_targets], include_directories: src_include_dir, dependencies: dependencies, install: true)
