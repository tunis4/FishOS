#include <klib/common.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdio.hpp>
#include <klib/bitmap.hpp>
#include <klib/cstdlib.hpp>
#include <limine.hpp>
#include <cpu/cpu.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/interrupts/idt.hpp>
#include <cpu/interrupts/pic.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <cpu/syscall/syscall.hpp>
#include <gfx/framebuffer.hpp>
#include <gfx/terminal.hpp>
#include <mem/pmm.hpp>
#include <mem/vmm.hpp>
#include <mem/vmem.hpp>
#include <mem/bump.hpp>
#include <panic.hpp>
#include <acpi/tables.hpp>
#include <sched/timer/pit.hpp>
#include <sched/timer/hpet.hpp>
#include <sched/timer/apic_timer.hpp>
#include <sched/time.hpp>
#include <sched/sched.hpp>
#include <userland/elf.hpp>
#include <fs/vfs.hpp>
#include <fs/initramfs.hpp>
#include <dev/device.hpp>
#include <dev/input/input.hpp>

static volatile limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static volatile limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

static volatile limine_smp_request smp_req = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};

static volatile limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

static volatile limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

static volatile limine_kernel_address_request kernel_addr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

static volatile limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

static volatile limine_boot_time_request boot_time_req = {
    .id = LIMINE_BOOT_TIME_REQUEST,
    .revision = 0
};

struct StackFrame {
    StackFrame *next;
    uptr ip;
};

[[noreturn]] void panic(const char *format, ...) {
    klib::panic_printf("\nKernel Panic: ");
    va_list list;
    va_start(list, format);
    klib::panic_vprintf(format, list);
    va_end(list);
    StackFrame *frame = (StackFrame*)__builtin_frame_address(0);
    klib::panic_printf("\nStacktrace:\n");
    while (true) {
        if (frame == nullptr || frame->ip == 0)
            break;
        
        klib::panic_printf("%#lX\n", frame->ip);
        frame = frame->next;
    }
    abort();
}

[[noreturn]] void kernel_thread();

extern "C" [[noreturn]] void _start() {
    if (!fb_req.response || fb_req.response->framebuffer_count == 0 || !hhdm_req.response)
        panic("Did not receive enough limine features to initialise early boot terminal");

    uptr hhdm = hhdm_req.response->offset;

    gfx::main_framebuffer.from_limine_fb(fb_req.response->framebuffers[0]);

    if (!memmap_req.response) panic("Did not receive Limine memmap feature response");
    if (!module_req.response) panic("Did not receive Limine module feature response");
    if (!kernel_addr_req.response) panic("Did not receive Limine kernel address feature response");
    if (!rsdp_req.response) panic("Did not receive Limine RSDP feature response");
    if (!smp_req.response) panic("Did not receive Limine SMP feature response");

    mem::pmm::init(hhdm, memmap_req.response);
    klib::printf("PMM: Initialized\n");
    
    cpu::early_init();
    mem::vmem::early_init();

    mem::vmm::init(hhdm, memmap_req.response, kernel_addr_req.response);
    klib::printf("VMM: Initialized\n");

    mem::bump::init(mem::vmm::heap_base, mem::vmm::heap_size);
    klib::printf("Allocator: Initialized\n");

    gfx::kernel_terminal();
    gfx::set_kernel_terminal_ready();
    klib::printf("Terminal: Initialized\n");

    cpu::smp_init(smp_req.response);

    klib::printf("ACPI: Parsing ACPI tables and enabling APIC\n");
    acpi::parse_rsdp((uptr)rsdp_req.response->address);

    cpu::syscall::init_syscall_table();

    sched::timer::apic_timer::init();
    klib::printf("APIC Timer: Initialized\n");

    sched::init();
    sched::init_time(boot_time_req.response);
    klib::printf("Scheduler: Initialized\n");

    vfs::init();
    klib::printf("VFS: Initialized\n");

    sched::new_kernel_thread(kernel_thread, true);
    sched::start();
    
    asm volatile("sti");
    while (true) asm volatile("hlt");

    panic("Reached end of _start");
}

static void create_device_file(const char *path, uint major, uint minor, bool is_char) {
    auto *entry = vfs::path_to_entry(path, nullptr);
    ASSERT(entry->vnode == nullptr);
    if (is_char)
        entry->vnode = dev::CharDevNode::create_node(dev::make_dev_id(major, minor));
    else
        entry->vnode = dev::BlockDevNode::create_node(dev::make_dev_id(major, minor));
    entry->create();
}

[[noreturn]] void kernel_thread() {
    dev::init_devices();

    dev::input::init();
    klib::printf("Input: Initialized\n");
    
    auto module_res = module_req.response;
    if (module_res->module_count == 0) panic("No initramfs Limine module loaded");
    if (module_res->module_count > 1) panic("Too many Limine modules loaded");
    auto initramfs_module = module_res->modules[0];
    klib::printf("Loading initramfs file %s (size: %ld KiB)\n", initramfs_module->path, initramfs_module->size / 1024);
    initramfs::load_into(vfs::get_root_entry(), initramfs_module->address, initramfs_module->size);

    create_device_file("/dev/mem",      1, 1, true);
    create_device_file("/dev/null",     1, 3, true);
    create_device_file("/dev/port",     1, 4, true);
    create_device_file("/dev/zero",     1, 5, true);
    create_device_file("/dev/full",     1, 7, true);
    create_device_file("/dev/console",  5, 1, true);
    create_device_file("/dev/fb0",     29, 0, true);
    auto *entry = vfs::path_to_entry("/dev/input", nullptr);
    ASSERT(entry->vnode == nullptr);
    entry->create(vfs::NodeType::DIRECTORY);
    if (dev::input::main_keyboard)
        create_device_file("/dev/input/event0", 13, 64, true);
    if (dev::input::main_mouse)
        create_device_file("/dev/input/event1", 13, 65, true);

    auto *init_entry = vfs::path_to_entry("/usr/bin/init");
    if (init_entry->vnode == nullptr)
        panic("Failed to find /usr/bin/init");

    klib::printf("Loading executable /usr/bin/init\n");
    sched::Process *init_process = sched::new_user_process(init_entry->vnode, true);
    
    while (true) {
        if (init_process->is_zombie) {
            klib::printf("Init process died, rebooting in 5 seconds\n");
            sched::timer::hpet::stall_ms(5000);
            cpu::write_cr3(0);
        }
        init_process->event.await();
    }
    // sched::dequeue_and_die();
}
