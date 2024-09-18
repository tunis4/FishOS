project('fishos', ['cpp', 'c', 'nasm'], default_options: [
  'buildtype=debug', 'warning_level=2', 'cpp_std=gnu++23', 'b_sanitize=undefined'
])

if not meson.is_cross_build()
  error('Must be a cross build')
endif

fs = import('fs')

add_global_arguments([
  '-O0', '-g3', '-pipe',
  '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  '-ffile-prefix-map=@0@/='.format(fs.relative_to(meson.current_source_dir(), meson.build_root())),

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

  'src/dev/device.cpp',
  'src/dev/console.cpp',
  'src/dev/input/input.cpp',
  'src/dev/input/evdev.cpp',
  'src/dev/input/ps2/ps2.cpp',
  'src/dev/input/ps2/keyboard.cpp',
  'src/dev/input/ps2/mouse.cpp',

  'src/fs/initramfs.cpp',
  'src/fs/tmpfs.cpp',
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
  'src/sched/event.cpp',
  'src/sched/time.cpp',

  'src/sched/timer/apic_timer.cpp',
  'src/sched/timer/hpet.cpp',
  'src/sched/timer/pit.cpp',

  'src/userland/elf.cpp',
  'src/userland/pipe.cpp',
  'src/userland/socket.cpp',
  'src/userland/futex.cpp',
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

ld = find_program('x86_64-pc-fishos-mlibc-ld')
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

executable('fishos', [source_files, font_object_targets], include_directories: src_include_dir, dependencies: dependencies, install: true)
