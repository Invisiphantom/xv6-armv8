/*
 * File descriptors
 */

#include "file.h"
#include "console.h"
#include "log.h"
#include "sleeplock.h"
#include "spinlock.h"
#include "types.h"

struct devsw devsw[NDEV];

struct {
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

void file_init()
{
    initlock(&ftable.lock, "ftable");
    cprintf("file_init: success.\n");
}

/*
 * Allocate a file structure.
 */
struct file* file_alloc()
{
    acquire(&ftable.lock);
    for (struct file* f = ftable.file; f < ftable.file + NFILE; ++f) {
        if (!f->ref) {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }
    release(&ftable.lock);
    return NULL;
}

/*
 * Increment ref count for file f.
 */
struct file* file_dup(struct file* f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("\tfile_dup: invalid file.\n");
    f->ref++;
    release(&ftable.lock);
    return f;
}

/*
 * Close file f. (Decrement ref count, close when reaches 0.)
 */
void file_close(struct file* f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("\tfile_close: invalid file.\n");
    if (--f->ref > 0) {
        release(&ftable.lock);
        return;
    }

    struct file ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock);

    if (ff.type == FD_INODE) {
        begin_op();
        iput(ff.ip);
        end_op();
    } else {
        panic("\tfile_close: unsupported type.\n");
    }
}

/*
 * Get metadata about file f.
 */
int file_stat(struct file* f, struct stat* st)
{
    if (f->type == FD_INODE) {
        ilock(f->ip);
        stati(f->ip, st);
        iunlock(f->ip);
        return 0;
    }
    return -1;
}

/*
 * Read from file f.
 */
ssize_t file_read(struct file* f, char* addr, ssize_t n)
{
    if (!f->readable)
        return -1;
    if (f->type == FD_INODE) {
        ilock(f->ip);
        int r = readi(f->ip, addr, f->off, n);
        if (r > 0)
            f->off += r;
        iunlock(f->ip);
        return r;
    }
    panic("\tfile_read: unsupported type.\n");
    return 0;
}

/*
 * Write to file f.
 */
ssize_t file_write(struct file* f, char* addr, ssize_t n)
{
    if (!f->writable)
        return -1;
    if (f->type == FD_INODE) {
        // Write a few blocks at a time to avoid exceeding the maximum log
        // transaction size, including i-node, indirect block, allocation
        // blocks, and 2 blocks of slop for non-aligned writes. This really
        // belongs lower down, since writei() might be writing a device like the
        // console.
        int max = ((MAXOPBLOCKS - 4) / 2) * 512;
        int i = 0;
        while (i < n) {
            int n1 = n - i;
            if (n1 > max)
                n1 = max;

            begin_op();
            ilock(f->ip);
            int r = writei(f->ip, addr + i, f->off, n1);
            if (r > 0)
                f->off += r;
            iunlock(f->ip);
            end_op();

            if (r < 0)
                break;
            if (r != n1)
                panic("\tfile_write: partial data written.\n");
            i += r;
        }
        return i == n ? n : -1;
    }
    panic("\tfile_write: unsupported type.\n");
    return 0;
}
