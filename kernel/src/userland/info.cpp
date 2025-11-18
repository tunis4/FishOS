#include <userland/info.hpp>
#include <sched/time.hpp>
#include <sched/sched.hpp>
#include <mem/pmm.hpp>
#include <klib/cstring.hpp>
#include <cpu/syscall/syscall.hpp>

namespace userland {
    isize syscall_uname(struct utsname *buf) {
        log_syscall("uname(%#lX)\n", (uptr)buf);
        sched::Thread *thread = cpu::get_current_thread();
        bool fake_linux = true;
        if (klib::strcmp(thread->name, "uname") == 0 || klib::strcmp(thread->name, "fastfetch") == 0)
            fake_linux = false;
        if (fake_linux) {
            klib::strncpy(buf->sysname, "Linux", sizeof(buf->sysname));
            klib::strncpy(buf->release, "3.0.1-fishix", sizeof(buf->release));
            klib::strncpy(buf->version, "#1 SMP PREEMPT_DYNAMIC " __DATE__ " " __TIME__, sizeof(buf->version));
        } else {
            klib::strncpy(buf->sysname, "Fishix", sizeof(buf->sysname));
            klib::strncpy(buf->release, "1.0.0", sizeof(buf->release));
            klib::strncpy(buf->version, __DATE__ " " __TIME__, sizeof(buf->version));
        }
        klib::strncpy(buf->nodename, "fishpc", sizeof(buf->nodename));
        klib::strncpy(buf->machine, "x86_64", sizeof(buf->machine));
        klib::strncpy(buf->domainname, "(none)", sizeof(buf->domainname));
        return 0;
    }

    isize syscall_sysinfo(struct sysinfo *info) {
        log_syscall("sysinfo(%#lX)\n", (uptr)info);
        info->uptime = sched::get_clock(CLOCK_MONOTONIC).seconds;
        info->loads[0] = 0;
        info->loads[1] = 0;
        info->loads[2] = 0;
        info->totalram = pmm::stats.total_pages_usable;
        info->freeram = pmm::stats.total_free_pages;
        info->sharedram = 0;
        info->bufferram = 0;
        info->totalswap = 0;
        info->freeswap = 0;
        info->procs = 0;
        info->totalhigh = 0;
        info->freehigh = 0;
        info->mem_unit = 0x1000;
        return 0;
    }
}
