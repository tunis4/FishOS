#include <cpu/cpu.hpp>
#include <klib/cstdio.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <mem/vmm.hpp>
#include <fs/vfs.hpp>
#include <userland/pipe.hpp>
#include <userland/socket.hpp>
#include <userland/futex.hpp>

namespace cpu::syscall {
    extern "C" { void *__syscall_table[46]; }
    
    void init_syscall_table() {
        __syscall_table[0]  = (void*)&sched::syscall_exit;
        __syscall_table[1]  = (void*)&vfs::syscall_open;
        __syscall_table[2]  = (void*)&vfs::syscall_mkdir;
        __syscall_table[3]  = (void*)&vfs::syscall_close;
        __syscall_table[4]  = (void*)&vfs::syscall_read;
        __syscall_table[5]  = (void*)&vfs::syscall_pread;
        __syscall_table[6]  = (void*)&vfs::syscall_write;
        __syscall_table[7]  = (void*)&vfs::syscall_pwrite;
        __syscall_table[8]  = (void*)&vfs::syscall_seek;
        __syscall_table[9]  = (void*)&vfs::syscall_getcwd;
        __syscall_table[10] = (void*)&vfs::syscall_chdir;
        __syscall_table[11] = (void*)&mem::vmm::syscall_mmap;
        __syscall_table[12] = (void*)&mem::vmm::syscall_munmap;
        __syscall_table[13] = (void*)&sched::syscall_fork;
        __syscall_table[14] = (void*)&sched::syscall_execve;
        __syscall_table[15] = (void*)&sched::syscall_sleep;
        __syscall_table[16] = (void*)&sched::syscall_set_fs_base;
        __syscall_table[17] = (void*)&vfs::syscall_readdir;
        __syscall_table[18] = (void*)&vfs::syscall_unlink;
        __syscall_table[19] = (void*)&vfs::syscall_fcntl;
        __syscall_table[20] = (void*)&vfs::syscall_dup;
        __syscall_table[21] = (void*)&vfs::syscall_stat;
        __syscall_table[22] = (void*)&sched::syscall_waitpid;
        __syscall_table[23] = (void*)&sched::syscall_uname;
        __syscall_table[24] = (void*)&vfs::syscall_rename;
        __syscall_table[25] = (void*)&vfs::syscall_poll;
        __syscall_table[26] = (void*)&userland::syscall_pipe;
        __syscall_table[27] = (void*)&vfs::syscall_readlink;
        __syscall_table[28] = (void*)&vfs::syscall_ioctl;
        __syscall_table[29] = (void*)&sched::syscall_clock_gettime;
        __syscall_table[30] = (void*)&sched::syscall_clock_getres;
        __syscall_table[31] = (void*)&userland::syscall_socket;
        __syscall_table[32] = (void*)&userland::syscall_socketpair;
        __syscall_table[33] = (void*)&userland::syscall_bind;
        __syscall_table[34] = (void*)&userland::syscall_connect;
        __syscall_table[35] = (void*)&userland::syscall_listen;
        __syscall_table[36] = (void*)&userland::syscall_accept;
        __syscall_table[37] = (void*)&userland::syscall_recvmsg;
        __syscall_table[38] = (void*)&userland::syscall_sendmsg;
        __syscall_table[39] = (void*)&userland::syscall_shutdown;
        __syscall_table[40] = (void*)&vfs::syscall_link;
        __syscall_table[41] = (void*)&vfs::syscall_symlink;
        __syscall_table[42] = (void*)&sched::syscall_thread_spawn;
        __syscall_table[43] = (void*)&sched::syscall_thread_exit;
        __syscall_table[44] = (void*)&userland::syscall_futex_wait;
        __syscall_table[45] = (void*)&userland::syscall_futex_wake;
    }
}
