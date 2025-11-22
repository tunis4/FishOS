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
#include <userland/futex.hpp>
#include <fs/vfs.hpp>
#include <fs/procfs.hpp>
#include <fs/initramfs.hpp>
#include <dev/io_service.hpp>
#include <dev/devnode.hpp>
#include <dev/input/input.hpp>
#include <dev/pci.hpp>
#include <dev/net.hpp>

[[gnu::used, gnu::section(".limine_requests")]]
static volatile LIMINE_BASE_REVISION(LIMINE_API_REVISION);

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_mp_request smp_req = {
    .id = LIMINE_MP_REQUEST,
    .revision = 0,
    .flags = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_kernel_address_request kernel_addr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_kernel_file_request kernel_file_req = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests")]]
static volatile limine_boot_time_request boot_time_req = {
    .id = LIMINE_BOOT_TIME_REQUEST,
    .revision = 0
};

[[gnu::used, gnu::section(".limine_requests_start")]]
static volatile LIMINE_REQUESTS_START_MARKER;

[[gnu::used, gnu::section(".limine_requests_end")]]
static volatile LIMINE_REQUESTS_END_MARKER;

extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

[[noreturn]] void panic(const char *format, ...) {
    // gfx::kernel_terminal_enabled = true;
    klib::printf_unlocked("\nKernel Panic: ");
    va_list list;
    va_start(list, format);
    klib::vprintf_unlocked(format, list);
    va_end(list);
    StackFrame *frame = (StackFrame*)__builtin_frame_address(0);
    klib::printf_unlocked("\nStacktrace:\n");
    while (true) {
        if (frame == nullptr || frame->ip == 0)
            break;
        
        klib::printf_unlocked("%#lX\n", frame->ip);
        frame = frame->next;
    }
    abort();
}

[[noreturn]] void kernel_thread();

extern "C" [[noreturn]] void kmain() {
    if (!fb_req.response || fb_req.response->framebuffer_count == 0 || !hhdm_req.response)
        panic("Did not receive enough limine features to initialise early boot terminal");

    uptr hhdm = hhdm_req.response->offset;

    gfx::main_framebuffer.from_limine_fb(fb_req.response->framebuffers[0]);

    if (!memmap_req.response) panic("Did not receive Limine memmap feature response");
    if (!kernel_file_req.response) panic("Did not receive Limine kernel file feature response");
    if (!module_req.response) panic("Did not receive Limine module feature response");
    if (!kernel_addr_req.response) panic("Did not receive Limine kernel address feature response");
    if (!rsdp_req.response) panic("Did not receive Limine RSDP feature response");
    if (!smp_req.response) panic("Did not receive Limine SMP feature response");

    pmm::init(hhdm, memmap_req.response);
    klib::printf("PMM: Initialized\n");
    
    cpu::early_init();
    mem::vmem::early_init();

    alignas(alignof(mem::VMM)) static u8 vmm_data[sizeof(mem::VMM)]; // FIXME: insanely cursed, needed for global constructors to work but there has to be a better way
    mem::vmm = new (&vmm_data) mem::VMM();
    mem::vmm->init(hhdm, memmap_req.response, kernel_addr_req.response);
    klib::printf("VMM: Initialized\n");

    mem::bump::init(mem::vmm->heap_base, mem::vmm->heap_size);
    klib::printf("Allocator: Initialized\n");

    // call global constructors
    for (auto ctor = __init_array_start; ctor < __init_array_end; ctor++)
        (*ctor)();

    gfx::kernel_terminal();
    gfx::kernel_terminal_enabled = true;
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

    procfs::kernel_cmdline = kernel_file_req.response->kernel_file->cmdline;
    vfs::init();
    klib::printf("VFS: Initialized\n");

    sched::new_kernel_thread(kernel_thread, true, "Kernel main thread");
    sched::start();
    
    asm volatile("sti");
    while (true) asm volatile("hlt");

    panic("Reached end of kmain");
}

static void create_device_file(const char *path, uint major, uint minor, bool is_char) {
    auto *entry = vfs::path_to_entry(path, nullptr);
    ASSERT(entry->vnode == nullptr);
    ASSERT(entry->parent != nullptr);
    if (is_char)
        entry->vnode = dev::CharDevNode::create_node(dev::make_dev_id(major, minor));
    else
        entry->vnode = dev::BlockDevNode::create_node(dev::make_dev_id(major, minor));
    entry->create(is_char ? vfs::NodeType::CHAR_DEVICE : vfs::NodeType::BLOCK_DEVICE, 0, 0, 0664);
}

[[noreturn]] void kernel_thread() {
    dev::init_io_service();
    dev::init_devices();
    new net::LoopbackInterface();

    dev::pci::init();
    klib::printf("PCI: Initialized\n");

    dev::input::init();
    klib::printf("Input: Initialized\n");

    auto module_res = module_req.response;
    if (module_res->module_count == 0) panic("No initramfs Limine module loaded");
    if (module_res->module_count > 1) panic("Too many Limine modules loaded");
    auto initramfs_module = module_res->modules[0];
    klib::printf("Loading initramfs file %s (size: %ld KiB)\n", initramfs_module->path, initramfs_module->size / 1024);
    initramfs::load_into(vfs::get_root_entry(), initramfs_module->address, initramfs_module->size);

    auto *dev_dir = vfs::path_to_entry("/dev");
    if (dev_dir->vnode == nullptr)
        dev_dir->create(vfs::NodeType::DIRECTORY, 0, 0, 0755);

    create_device_file("/dev/mem",      1, 1, true);
    create_device_file("/dev/null",     1, 3, true);
    create_device_file("/dev/port",     1, 4, true);
    create_device_file("/dev/zero",     1, 5, true);
    create_device_file("/dev/full",     1, 7, true);
    create_device_file("/dev/random",   1, 8, true);
    create_device_file("/dev/urandom",  1, 9, true);

    create_device_file("/dev/tty",      5, 0, true);
    create_device_file("/dev/console",  5, 1, true);

    create_device_file("/dev/fb0",     29, 0, true);
    create_device_file("/dev/vda",      8, 0, false);

    auto *input_dir = vfs::path_to_entry("/dev/input");
    ASSERT(input_dir->vnode == nullptr);
    ASSERT(input_dir->parent != nullptr);
    input_dir->create(vfs::NodeType::DIRECTORY, 0, 0, 0755);
    if (dev::input::main_keyboard)
        create_device_file("/dev/input/event0", 13, 64, true);
    if (dev::input::main_mouse)
        create_device_file("/dev/input/event1", 13, 65, true);

    auto *pts_dir = vfs::path_to_entry("/dev/pts");
    ASSERT(pts_dir->vnode == nullptr);
    ASSERT(pts_dir->parent != nullptr);
    pts_dir->create(vfs::NodeType::DIRECTORY, 0, 0, 0755);
    create_device_file("/dev/pts/ptmx", 5, 2, true);
    create_device_file("/dev/ptmx",     5, 2, true);

    const char *init_path = "/usr/bin/init_wrapper";
    if (vfs::path_to_entry(init_path)->vnode == nullptr)
        panic("Failed to find %s", init_path);

    klib::printf("Loading executable %s\n", init_path);
    sched::Process *init_process;
    if (klib::strstr(kernel_file_req.response->kernel_file->cmdline, "startwm")) {
        gfx::kernel_terminal_enabled = false;
        char *argv[] = { (char*)init_path, (char*)"--startwm", nullptr };
        init_process = sched::create_init_process(init_path, 2, argv);
    } else {
        char *argv[] = { (char*)init_path, nullptr };
        init_process = sched::create_init_process(init_path, 1, argv);
    }

    while (true) {
        if (init_process->get_main_thread()->state == sched::Thread::ZOMBIE) {
            klib::printf("Init process died\n");
            // sched::timer::hpet::stall_ms(5000);
            // cpu::write_cr3(0);
        }
        init_process->zombie_event.wait();
    }
}
