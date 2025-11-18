#include <userland/pid.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/cstdio.hpp>
#include <sched/sched.hpp>

namespace userland {
    isize syscall_getuid() { log_syscall("getuid()\n"); return cpu::get_current_thread()->cred.uids.rid; }
    isize syscall_geteuid() { log_syscall("geteuid()\n"); return cpu::get_current_thread()->cred.uids.eid; }
    isize syscall_getgid() { log_syscall("getgid()\n"); return cpu::get_current_thread()->cred.gids.rid; }
    isize syscall_getegid() { log_syscall("getegid()\n"); return cpu::get_current_thread()->cred.gids.eid; }

    isize syscall_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid) {
        log_syscall("getresuid(%#lX, %#lX, %#lX)\n", (uptr)ruid, (uptr)euid, (uptr)suid);
        *ruid = cpu::get_current_thread()->cred.uids.rid;
        *euid = cpu::get_current_thread()->cred.uids.eid;
        *suid = cpu::get_current_thread()->cred.uids.sid;
        return 0;
    }

    isize syscall_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid) {
        log_syscall("getresgid(%#lX, %#lX, %#lX)\n", (uptr)rgid, (uptr)egid, (uptr)sgid);
        *rgid = cpu::get_current_thread()->cred.gids.rid;
        *egid = cpu::get_current_thread()->cred.gids.eid;
        *sgid = cpu::get_current_thread()->cred.gids.sid;
        return 0;
    }

    static isize setid(bool is_gid, uint id) {
        sched::Thread *thread = cpu::get_current_thread();
        Cred::IdSet &ids = is_gid ? thread->cred.gids : thread->cred.uids;
        if (thread->cred.has_capability(is_gid ? CAP_SETGID : CAP_SETUID)) {
            if (id != ids.eid)
                thread->process->dumpable = false;
            ids.eid = id;
            ids.fsid = id;
            ids.rid = id;
            ids.sid = id;
        } else {
            if (ids.contains(id)) {
                if (id != ids.eid)
                    thread->process->dumpable = false;
                ids.eid = id;
                ids.fsid = id;
            } else {
                return -EPERM;
            }
        }
        return 0;
    }

    static isize setresid(bool is_gid, uint rid, uint eid, uint sid) {
        sched::Thread *thread = cpu::get_current_thread();
        Cred::IdSet &ids = is_gid ? thread->cred.gids : thread->cred.uids;
        Cred::IdSet new_ids = ids;

        if (thread->cred.has_capability(is_gid ? CAP_SETGID : CAP_SETUID)) {
            if (eid != (uint)-1) new_ids.eid = eid;
            if (rid != (uint)-1) new_ids.rid = rid;
            if (sid != (uint)-1) new_ids.sid = sid;
        } else {
            if (eid != (uint)-1) {
                if (!ids.contains(eid)) return -EPERM;
                new_ids.eid = eid;
            }
            if (rid != (uint)-1) {
                if (!ids.contains(rid)) return -EPERM;
                new_ids.rid = rid;
            }
            if (sid != (uint)-1) {
                if (!ids.contains(sid)) return -EPERM;
                new_ids.sid = sid;
            }
        }
        new_ids.fsid = new_ids.eid;
        if (new_ids.eid != ids.eid || new_ids.fsid != ids.fsid)
            thread->process->dumpable = false;

        ids = new_ids;
        return 0;
    }

    isize syscall_setuid(uid_t uid) {
        log_syscall("setuid(%d)\n", uid);
        return setid(false, uid);
    }

    isize syscall_setgid(gid_t gid) {
        log_syscall("setgid(%d)\n", gid);
        return setid(true, gid);
    }

    isize syscall_setreuid(uid_t ruid, uid_t euid) {
        log_syscall("setreuid(%d, %d)\n", ruid, euid);
        return setresid(false, ruid, euid, -1);
    }

    isize syscall_setregid(gid_t rgid, gid_t egid) {
        log_syscall("setregid(%d, %d)\n", rgid, egid);
        return setresid(true, rgid, egid, -1);
    }

    isize syscall_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
        log_syscall("setresuid(%d, %d, %d)\n", ruid, euid, suid);
        return setresid(false, ruid, euid, suid);
    }

    isize syscall_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
        log_syscall("setresgid(%d, %d, %d)\n", rgid, egid, sgid);
        return setresid(true, rgid, egid, sgid);
    }

    isize syscall_setfsuid(uid_t fsuid) {
        log_syscall("setfsuid(%d)\n", fsuid);
        return -ENOSYS;
    }

    isize syscall_setfsgid(gid_t fsgid) {
        log_syscall("setfsgid(%d)\n", fsgid);
        return -ENOSYS;
    }

    isize syscall_getgroups(int size, gid_t *list) {
        log_syscall("getgroups(%d, %#lX)\n", size, (uptr)list);
        auto &groups = cpu::get_current_thread()->cred.groups;
        int actual_size = groups.size();
        if (size == 0)
            return actual_size;
        if (size < actual_size)
            return -EINVAL;
        memcpy(list, groups.data(), actual_size * sizeof(gid_t));
        return actual_size;
    }

    isize syscall_setgroups(int size, const gid_t *list) {
        log_syscall("setgroups(%d, %#lX)\n", size, (uptr)list);
        auto &groups = cpu::get_current_thread()->cred.groups;
        if (size < 0) return -EINVAL;
        groups.resize(size);
        if (size > 0) {
            memcpy(groups.data(), list, size * sizeof(gid_t));
            klib::qsort(groups.data(), groups.data() + size);
        }
        return 0;
    }
}
