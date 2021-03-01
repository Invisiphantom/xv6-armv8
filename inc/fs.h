/*
 * On-disk file system format.
 * Both the kernel and user programs use this header file.
 */
#ifndef INC_FS_H_
#define INC_FS_H_

#include <stdint.h>

#include "types.h"

// Kernel only
#define NDEV        10                 // Maximum major device number
#define NINODE      50                 // Maximum number of active i-nodes
#define MAXOPBLOCKS 10                 // Max # of blocks any FS op writes
#define NBUF        (MAXOPBLOCKS * 3)  // Size of disk block cache

// mkfs only
#define FSSIZE 1000  // Size of file system in blocks

// Belows are used by both
#define LOGSIZE (MAXOPBLOCKS * 3)  // Max data blocks in on-disk log
#define ROOTDEV 1                  // Device number of file system root disk
#define ROOTINO 1                  // Root i-number

#define BSIZE 512  // Block size

/*
 * Disk layout:
 * [boot block | super block | log | inode blocks | free bit map | data blocks]
 *
 * mkfs computes the super block and builds an initial file system.
 * The super block describes the disk layout:
 */
struct superblock {
    uint32_t size;        // Size of file system image (blocks)
    uint32_t nblocks;     // Number of data blocks
    uint32_t ninodes;     // Number of inodes
    uint32_t nlog;        // Number of log blocks
    uint32_t logstart;    // Block number of first log block
    uint32_t inodestart;  // Block number of first inode block
    uint32_t bmapstart;   // Block number of first free map block
};

#define NDIRECT   12
#define NINDIRECT (BSIZE / sizeof(uint32_t))
#define MAXFILE   (NDIRECT + NINDIRECT)

/* On-disk inode structure. */
struct dinode {
    uint16_t type;                // File type
    uint16_t major;               // Major device number (T_DEV only)
    uint16_t minor;               // Minor device number (T_DEV only)
    uint16_t nlink;               // Number of links to inode in file system
    uint32_t size;                // Size of file (bytes)
    uint32_t addrs[NDIRECT + 1];  // Data block addresses
};

/* Inodes per block. */
#define IPB (BSIZE / sizeof(struct dinode))

/* Block containing inode i. */
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

/* Bitmap bits per block. */
#define BPB (BSIZE * 8)

/* Block of free map containing bit for block b. */
#define BBLOCK(b, sb) (b / BPB + sb.bmapstart)

/* Directory is a file containing a sequence of dirent structures. */
#define DIRSIZ 14

struct dirent {
    uint16_t inum;
    char name[DIRSIZ];
};

struct stat;

#define T_DIR  1  // Directory
#define T_FILE 2  // File
#define T_DEV  3  // Device

void readsb(int, struct superblock*);

void iinit(int);
struct inode* ialloc(uint32_t, uint16_t);
void iupdate(struct inode*);
struct inode* idup(struct inode*);
void ilock(struct inode*);
void iunlock(struct inode*);
void iput(struct inode*);
void iunlockput(struct inode*);
void stati(struct inode*, struct stat*);
ssize_t readi(struct inode*, char*, size_t, size_t);
ssize_t writei(struct inode*, char*, size_t, size_t);

int namecmp(const char*, const char*);
struct inode* dirlookup(struct inode*, char*, size_t*);
int dirlink(struct inode*, char*, uint32_t);

struct inode* namei(char*);
struct inode* nameiparent(char*, char*);

#endif  // INC_FS_H_
