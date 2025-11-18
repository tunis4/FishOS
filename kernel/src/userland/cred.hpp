#pragma once

#include <klib/common.hpp>
#include <klib/vector.hpp>
#include <sys/stat.h>
#include <linux/capability.h>

namespace userland {
    struct Cred {
        struct IdSet {
            uint rid = -1, eid = -1, fsid = -1, sid = -1;

            bool contains(uint id) { return rid == id || eid == id || sid == id; }
        };
        IdSet uids, gids;
        klib::Vector<gid_t> groups;

        bool has_capability(uint cap) const { return uids.eid == 0; }
    };

    isize syscall_getuid();
    isize syscall_geteuid();
    isize syscall_getgid();
    isize syscall_getegid();
    isize syscall_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
    isize syscall_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);

    isize syscall_setuid(uid_t uid);
    isize syscall_setgid(gid_t gid);
    isize syscall_setreuid(uid_t ruid, uid_t euid);
    isize syscall_setregid(gid_t rgid, gid_t egid);
    isize syscall_setresuid(uid_t ruid, uid_t euid, uid_t suid);
    isize syscall_setresgid(gid_t rgid, gid_t egid, gid_t sgid);
    isize syscall_setfsuid(uid_t fsuid);
    isize syscall_setfsgid(gid_t fsgid);

    isize syscall_getgroups(int size, gid_t *list);
    isize syscall_setgroups(int size, const gid_t *list);
}
