#include <syscall.h>

#include "console.h"
#include "proc.h"
#include "string.h"
#include "syscall1.h"
#include "types.h"

/*
 * User code makes a system call with SVC.
 * System call number in r8.
 * Arguments on the stack, from the user call to the C
 * library system call function.
 */

/* Fetch the int at addr from the current process. */
int fetchint(uint64_t addr, int64_t* ip) {
    struct proc* p = thisproc();
    if (addr >= p->sz || addr + 8 > p->sz)
        return -1;

    *ip = *(int64_t*)(addr);
    return 0;
}

/*
 * Fetch the nul-terminated string at addr from the current process.
 * Doesn't actually copy the string - just sets *pp to point at it.
 * Returns length of string, not including nul.
 */
int fetchstr(uint64_t addr, char** pp) {
    struct proc* p = thisproc();
    if (addr >= p->sz)
        return -1;

    *pp = (char*)addr;
    char* ep = (char*)p->sz;
    for (char* s = *pp; s < ep; ++s) {
        if (*s == '\0')
            return s - *pp;
    }
    return -1;
}

/*
 * Fetch the nth (starting from 0) 32-bit system call argument.
 * In our ABI, r0 contains system call index, r1-r4 contain parameters.
 * now we support system calls with at most 4 parameters.
 */
int argint(int n, uint64_t* ip) {
    if (n > 3)
        panic("\targint: too many system call parameters.\n");
    struct proc* p = thisproc();

    *ip = *(&p->tf->x1 + n);
    return 0;
}

/*
 * Fetch the nth word-sized system call argument as a pointer
 * to a block of memory of size n bytes.  Check that the pointer
 * lies within the process address space.
 */
int argptr(int n, char** pp, int size) {
    uint64_t i;
    if (argint(n, &i) < 0)
        return -1;

    struct proc* p = thisproc();
    if ((uint64_t)i >= p->sz || (uint64_t)i + size > p->sz)
        return -1;

    *pp = (char*)i;
    return 0;
}

/*
 * Fetch the nth word-sized system call argument as a string pointer.
 * Check that the pointer is valid and the string is nul-terminated.
 * (There is no shared writable memory, so the string can't change
 * between this check and being used by the kernel.)
 */
int argstr(int n, char** pp) {
    uint64_t addr;
    if (argint(n, &addr) < 0)
        return -1;
    cprintf("argstr: n=%d addr=%lld\n", n, addr);
    return fetchstr(addr, pp);
}

static func syscalls[] = {
    [SYS_set_tid_address] = sys_gettid,
    [SYS_gettid] = sys_gettid,
    [SYS_ioctl] = sys_ioctl,
    [SYS_rt_sigprocmask] = sys_rt_sigprocmask,
    [SYS_brk] = (func)sys_brk,
    [SYS_execve] = sys_exec,
    [SYS_sched_yield] = sys_yield,
    [SYS_clone] = sys_clone,
    [SYS_wait4] = sys_wait4,
    // FIXME: exit_group should kill every thread in the current thread group.
    [SYS_exit_group] = sys_exit,
    [SYS_exit] = sys_exit,
    [SYS_dup] = sys_dup,
    [SYS_chdir] = sys_chdir,
    [SYS_fstat] = sys_fstat,
    [SYS_newfstatat] = sys_fstatat,
    [SYS_mkdirat] = sys_mkdirat,
    [SYS_mknodat] = sys_mknodat,
    [SYS_openat] = sys_openat,
    [SYS_writev] = (func)sys_writev,
    [SYS_read] = (func)sys_read,
    [SYS_close] = sys_close,
};

int syscall1(struct trapframe* tf) {
    struct proc* p = thisproc();

    p->tf = tf;
    uint64_t sysno = tf->x8;  // 系统调用编号

    // 如果num在合法范围内, 并且对应的系统调用函数存在
    // 通过编号来访问系统调用, 并将返回值存储在x0中
    if (sysno >= 0 && sysno < ARRAY_SIZE(syscalls) && syscalls[sysno]) {
        cprintf("syscall: syscall %d from proc %d\n", sysno, p->pid);
        tf->x0 = syscalls[sysno]();
        return tf->x0;
    }

    else {
        cprintf("syscall: unknown syscall %d from proc %d\n", sysno, p->pid);
        while (1)
            ;
        return -1;
    }
    return 0;
}
