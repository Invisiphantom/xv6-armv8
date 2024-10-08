#include "proc.h"

#include "arm.h"
#include "console.h"
#include "file.h"
#include "kalloc.h"
#include "log.h"
#include "mmu.h"
#include "spinlock.h"
#include "string.h"
#include "trap.h"
#include "types.h"
#include "vm.h"

struct cpu cpus[NCPU];

struct {
    struct proc proc[NPROC];
} ptable;

static struct proc* initproc;

int nextpid = 1;
struct spinlock pid_lock;

struct spinlock wait_lock;

void forkret();
extern void usertrapret(struct trapframe*);
extern void trapret();
void swtch(struct context**, struct context*);

static int pid_next() {
    acquire(&pid_lock);
    int pid = nextpid++;
    release(&pid_lock);
    return pid;
}

/*
 * Initialize the spinlock for ptable to serialize the access to ptable
 */
void proc_init() {
    initlock(&wait_lock, "wait_lock");
    initlock(&pid_lock, "pid_lock");
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
        initlock(&p->lock, "proc_lock");
    }
    cprintf("proc_init: success.\n");
}

/*
 * Free a proc structure and the data hanging from it,
 * including user pages.
 * p->lock must be held.
 */
static void proc_free(struct proc* p) {
    p->chan = NULL;
    p->killed = 0;
    p->xstate = 0;
    p->pid = 0;
    p->parent = NULL;
    if (p->kstack)
        kfree(p->kstack);
    p->kstack = NULL;
    p->sz = 0;
    if (p->pgdir)
        vm_free(p->pgdir, 4);
    p->pgdir = NULL;
    p->tf = NULL;
    p->name[0] = '\0';
    p->state = UNUSED;
}

/*
 * Look through the process table for an UNUSED proc.
 * If found, change state to EMBRYO and initialize
 * state required to run in the kernel.
 * Otherwise return 0.
 */
static struct proc* proc_alloc() {
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
        acquire(&p->lock);
        if (p->state != UNUSED) {
            release(&p->lock);
            continue;
        }

        p->pid = pid_next();

        // Allocate kernel stack.
        if (!(p->kstack = kalloc())) {
            proc_free(p);
            release(&p->lock);
            return NULL;
        }
        char* sp = p->kstack + KSTACKSIZE;

        // Leave room for trapframe.
        sp -= sizeof(*p->tf);
        p->tf = (struct trapframe*)sp;

        // Set up new context to start executing at forkret.
        sp -= sizeof(*p->context);
        p->context = (struct context*)sp;
        memset(p->context, 0, sizeof(*p->context));
        p->context->x30 = (uint64_t)forkret;

        p->state = EMBRYO;
        cprintf("proc_alloc: proc %d success.\n", p->pid);
        return p;
    }
    return NULL;
}

/*
 * Set up first user process (only used once).
 * Set trapframe for the new process to run
 * from the beginning of the user process determined
 * by uvm_init
 */
void user_init() {
    extern char _binary_obj_user_initcode_start[];
    extern char _binary_obj_user_initcode_size[];

    struct proc* p = proc_alloc();
    if (!p)
        panic("\tuser_init: process failed to allocate.\n");
    initproc = p;

    // Allocate a user page table.
    if (!(p->pgdir = pgdir_init()))
        panic("\tuser_init: page table failed to allocate.\n");
    p->sz = PGSIZE;

    // Copy initcode into the page table.
    uvm_init(p->pgdir, _binary_obj_user_initcode_start, (uint64_t)_binary_obj_user_initcode_size);

    // Set up trapframe to prepare for the first "return" from kernel to user.
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->x30 = 0;          // initcode start address
    p->tf->sp_el0 = PGSIZE;  // user stack pointer
    p->tf->spsr_el1 = 0;     // program status register
    p->tf->elr_el1 = 0;      // exception link register

    strncpy(p->name, "initproc", sizeof(p->name));
    p->state = RUNNABLE;
    p->cwd = namei("/");
    release(&p->lock);

    cprintf("user_init: proc %d (%s) success.\n", p->pid, p->name, cpuid());
}

/*
 * Per-CPU process scheduler
 * Each CPU calls scheduler() after setting itself up.
 * Scheduler never returns. It loops, doing:
 *  - choose a process to run
 *  - swtch to start running that process
 *  - eventually that process transfers control
 *    via swtch back to the scheduler.
 */
void scheduler() {
    struct cpu* c = thiscpu;
    c->proc = NULL;

    while (1) {
        // Loop over process table looking for process to run.
        for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
            acquire(&p->lock);
            if (p->state != RUNNABLE) {
                release(&p->lock);
                continue;
            }

            // Switch to chosen process. It is the process's job
            // to release its lock and then reacquire it
            // before jumping back to us.
            c->proc = p;
            uvm_switch(p);
            p->state = RUNNING;
            // cprintf("scheduler: run proc %d at CPU %d.\n", p->pid, cpuid());

            swtch(&c->scheduler, p->context);

            // Process is done running for now.
            // It should have changed its p->state before coming back.
            c->proc = NULL;
            release(&p->lock);
        }
    }
}

/*
 * Enter scheduler. Must hold only p->lock
 * and have changed p->state.
 */
void sched() {
    struct cpu* c = thiscpu;
    struct proc* p = c->proc;

    if (!holding(&p->lock))
        panic("\tsched: process not locked.\n");
    if (p->state == RUNNING)
        panic("\tsched: process running.\n");

    swtch(&p->context, c->scheduler);
}

/*
 * A fork child's very first scheduling by scheduler()
 * will swtch to forkret. "Return" to user space.
 */
void forkret() {
    volatile static int first = 1;
    struct proc* p = thisproc();
    struct trapframe* tf = p->tf;

    // Still holding p->lock from scheduler.
    release(&p->lock);

    if (first) {
        // Some initialization functions must be run in the context
        // of a regular process (e.g., they call sleep), and thus cannot
        // be run from main().
        first = 0;
        iinit(ROOTDEV);
        initlog(ROOTDEV);
    }

    // Pass trapframe pointer as an argument when calling trapret.
    usertrapret(tf);
}

/*
 * Pass p's abandoned children to initproc.
 * Caller must hold wait_lock.
 */
void reparent(struct proc* p) {
    for (struct proc* pc = ptable.proc; pc < &ptable.proc[NPROC]; ++pc) {
        if (pc->parent == p) {
            pc->parent = initproc;
        }
    }
}

/*
 * Exit the current process. Does not return.
 * An exited process remains in the zombie state
 * until its parent calls wait() to find out it exited.
 */
void exit(int status) {
    struct proc* p = thisproc();

    if (p == initproc)
        panic("\texit: initproc exiting.\n");

    for (int fd = 0; fd < NOFILE; ++fd) {
        if (p->ofile[fd]) {
            file_close(p->ofile[fd]);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    p->cwd = 0;
    end_op();

    acquire(&wait_lock);

    // Give any children to init.
    reparent(p);

    acquire(&p->lock);
    p->xstate = status;
    p->state = ZOMBIE;

    release(&wait_lock);

    // Jump into the scheduler, never return.
    sched();
    panic("\texit: zombie returned!\n");
}

/*
 * Atomically release lock and sleep on chan.
 * Reacquires lock when awakened.
 */
void sleep(void* chan, struct spinlock* lk) {
    struct proc* p = thisproc();

    // Must acquire p->lock in order to
    // change p->state and then call sched.
    // Once we hold p->lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup locks p->lock),
    // so it's okay to release lk.

    acquire(&p->lock);
    release(lk);

    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;
    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    release(&p->lock);
    acquire(lk);
}

/*
 * Wake up all processes sleeping on chan.
 * Must be called without any p->lock.
 */
void wakeup(void* chan) {
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
        if (p != thisproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->chan == chan) {
                p->state = RUNNABLE;
            }
            release(&p->lock);
        }
    }
}

/*
 * Give up the CPU for one scheduling round.
 */
void yield() {
    struct proc* p = thisproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    // cprintf("yield: proc %d gives up CPU %d.\n", p->pid, cpuid());
    sched();
    release(&p->lock);
}

/*
 * Grow current process's memory by n bytes.
 * Return 0 on success, -1 on failure.
 */
int growproc(int n) {
    struct proc* p = thisproc();
    uint64_t sz = p->sz;
    if (n > 0) {
        if (!(sz = uvm_alloc(p->pgdir, sz, sz + n)))
            return -1;
    } else if (n < 0) {
        if (!(sz = uvm_dealloc(p->pgdir, sz, sz + n)))
            return -1;
    }
    p->sz = sz;
    uvm_switch(p);
    return 0;
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 * Caller must set state of returned proc to RUNNABLE.
 */
int fork() {
    // Allocate process
    struct proc* np = proc_alloc();
    if (!np)
        return -1;

    struct proc* p = thisproc();

    // Copy user memory from parent to child
    if (uvm_copy(p->pgdir, np->pgdir, p->sz) < 0) {
        proc_free(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    // Copy saved user registers
    memcpy(np->tf, p->tf, sizeof(*p->tf));

    // Cause fork to return 0 in the child
    np->tf->x0 = 0;

    // Increment reference counts on open file descriptors
    for (int i = 0; i < NOFILE; ++i) {
        if (p->ofile[i])
            np->ofile[i] = file_dup(p->ofile[i]);
    }
    np->cwd = idup(p->cwd);

    strncpy(np->name, p->name, sizeof(p->name));

    int pid = np->pid;

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);

    return pid;
}

/*
 * Wait for a child process to exit and return its pid.
 * Return -1 if this process has no children.
 */
int wait() {
    struct proc* p = thisproc();
    acquire(&wait_lock);

    while (1) {
        // Scan through table looking for exited children.
        int havekids = 0;
        for (struct proc* np = ptable.proc; np < &ptable.proc[NPROC]; ++np) {
            if (np->parent != p)
                continue;
            havekids = 1;
            if (np->state == ZOMBIE) {
                // Found one.
                int pid = np->pid;
                proc_free(np);
                release(&wait_lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || p->killed) {
            release(&wait_lock);
            return -1;
        }

        // Wait for children to exit.
        sleep(p, &wait_lock);
    }
}

/*
 * Print a process listing to console. For debugging.
 * No lock to avoid wedging a stuck machine further.
 */
void proc_dump() {
    static char* states[] = {
        [UNUSED] "UNUSED  ", [SLEEPING] "SLEEPING", [RUNNABLE] "RUNNABLE", [RUNNING] "RUNNING ", [ZOMBIE] "ZOMBIE  ",
    };

    cprintf("\n====== PROCESS DUMP ======\n");
    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; ++p) {
        if (p->state == UNUSED)
            continue;
        char* state =
            (p->state >= 0 && p->state < ARRAY_SIZE(states) && states[p->state]) ? states[p->state] : "UNKNOWN ";
        cprintf("[%s] %d (%s)\n", state, p->pid, p->name);
    }
    cprintf("====== DUMP END ======\n\n");
}

/*
 * Print the trap frame of a process to console. For debugging.
 * No lock to avoid wedging a stuck machine further.
 */
void trapframe_dump(struct proc* p) {
    cprintf("\n====== TRAP FRAME DUMP ======\n");
    cprintf("sp:\t%lld\n", p->tf->sp_el0);
    cprintf("spsr:\t%lld\n", p->tf->spsr_el1);
    cprintf("elr:\t%lld\n", p->tf->elr_el1);
    cprintf("x0:\t%lld\n", p->tf->x0);
    cprintf("x1:\t%lld\n", p->tf->x1);
    cprintf("x2:\t%lld\n", p->tf->x2);
    cprintf("x3:\t%lld\n", p->tf->x3);
    cprintf("x4:\t%lld\n", p->tf->x4);
    cprintf("x5:\t%lld\n", p->tf->x5);
    cprintf("x6:\t%lld\n", p->tf->x6);
    cprintf("x7:\t%lld\n", p->tf->x7);
    cprintf("x8:\t%lld\n", p->tf->x8);
    cprintf("x9:\t%lld\n", p->tf->x9);
    cprintf("x10:\t%lld\n", p->tf->x10);
    cprintf("x11:\t%lld\n", p->tf->x11);
    cprintf("x12:\t%lld\n", p->tf->x12);
    cprintf("x13:\t%lld\n", p->tf->x13);
    cprintf("x14:\t%lld\n", p->tf->x14);
    cprintf("x15:\t%lld\n", p->tf->x15);
    cprintf("x16:\t%lld\n", p->tf->x16);
    cprintf("x17:\t%lld\n", p->tf->x17);
    cprintf("x18:\t%lld\n", p->tf->x18);
    cprintf("x19:\t%lld\n", p->tf->x19);
    cprintf("x20:\t%lld\n", p->tf->x20);
    cprintf("x21:\t%lld\n", p->tf->x21);
    cprintf("x22:\t%lld\n", p->tf->x22);
    cprintf("x23:\t%lld\n", p->tf->x23);
    cprintf("x24:\t%lld\n", p->tf->x24);
    cprintf("x25:\t%lld\n", p->tf->x25);
    cprintf("x26:\t%lld\n", p->tf->x26);
    cprintf("x27:\t%lld\n", p->tf->x27);
    cprintf("x28:\t%lld\n", p->tf->x28);
    cprintf("x29:\t%lld\n", p->tf->x29);
    cprintf("x30:\t%lld\n", p->tf->x30);
    cprintf("====== DUMP END ======\n\n");
}
