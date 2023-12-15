#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "vsfs.h"

#define MAX_BLOCK_COUNT 4096
#define FREEBLOCK_BITVECTOR_SIZE MAX_BLOCK_COUNT / (16)
#define NO_START_BLOCK 0
#define FAT_LIST_NULL 0

// log levels
// #define ASSERT
// #define ERROR
// #define INFO

#ifdef ERROR
#define vsfs_err(...)            \
    fprintf(stderr, "[ERROR] "); \
    fprintf(stderr, ##__VA_ARGS__)
#else
#define vsfs_err(...) (void)0
#endif

#ifdef INFO
#define vsfs_info(...)          \
    fprintf(stdout, "[INFO] "); \
    fprintf(stdout, ##__VA_ARGS__)
#else
#define vsfs_info(...) (void)0
#endif

#ifdef ASSERT
#define vsfs_assert(expr) \
    assert(expr);         \
    fprintf(stdout, "[ASSERT] passed\n")
#else
#define vsfs_assert(...) (void)0
#endif

// type declarations ======================================
typedef struct data_block
{
    // a byte array
    uint8_t data[BLOCKSIZE];
} data_block;
/**
 * fat table entry is 4 bytes / 32 bits
 * since a block allocated for fat table can have (2048 / 4) = 512 entries
 * and there are 32 blocks allocated, this means that the FAT is sparse
 * consider block number 4095 = ....00001111 | 11111111
 * the lower byte can be used to index FAT table block 0 to 511
 * This means that the FAT entry for block 4095 will be in FAT block 15
 * at offset 511
 */
#define FAT_OFFSET(blocknumber) blocknumber & 0x000000ff
#define FAT_BLOCK(blocknumber) (blocknumber & 0xffffff00) >> 8

typedef struct fat_table_block
{
    uint32_t entries[512];
} fat_table_block;

typedef struct super_block
{
    int blockcount;
    uint16_t blocksize;
    /**
     * Max block count is 4096, so we need a bit vector
     * of a bit vector of length 4096 bits
     * blocks
     */
    uint16_t freeblock_bitvector[FREEBLOCK_BITVECTOR_SIZE];
    uint8_t padding[1528];
} super_block;

typedef struct directory_entry
{
    bool isoccupied;
    char filename[30];
    uintmax_t filesize;
    uint32_t startblock;
    uint8_t freebytes[80]; // must be 128 bytes
} directory_entry;

typedef struct openfiletable_entry
{
    directory_entry *entry;
    int mode;
    bool free;
} openfiletable_entry;

typedef struct root_dir_block
{
    directory_entry entries[16];
} root_dir_block;

// ========================================================

// globals  =======================================
static int vs_fd; // file descriptor of the Linux file that acts as virtual disk.
                  // this is not visible to an application.
static super_block superblock;
static fat_table_block fattable[32];
static root_dir_block rootdir[8];
static openfiletable_entry openfiletable[128];
// ========================================================

// read block k from disk (virtual disk) into buffer block.
// size of the block is BLOCKSIZE.
// space for block must be allocated outside of this function.
// block numbers start from 0 in the virtual disk.
int read_block(void *block, int k)
{
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(vs_fd, (off_t)offset, SEEK_SET);

    n = read(vs_fd, block, BLOCKSIZE);

    if (n != BLOCKSIZE)
    {
        printf("read error\n");
        return -1;
    }
    return (0);
}

// write block k into the virtual disk.
int write_block(void *block, int k)
{
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(vs_fd, (off_t)offset, SEEK_SET);
    n = write(vs_fd, block, BLOCKSIZE);
    if (n != BLOCKSIZE)
    {
        printf("write error\n");
        return (-1);
    }
    return 0;
}

uint32_t get_nextfreeblock()
{
    uint32_t from_basedatablock = 41;
    for (int i = 0; i < FREEBLOCK_BITVECTOR_SIZE; i++)
    {
        uint16_t currentportion = superblock.freeblock_bitvector[i];
        for (int j = 0x0001; j <= 0x8000; j = j << 1)
        {
            // cycle through all bits
            // first ...0001 then ...0010 then ...0100 then ...1000
            if ((uint16_t)(j)&currentportion)
            {
                // this block is available, mark unavailable
                superblock.freeblock_bitvector[i] = currentportion & ~j;
                return from_basedatablock;
            }
            from_basedatablock++;
            // if reached max blocks count!
            if (from_basedatablock == superblock.blockcount)
                return 0;
        }
    }
    return 0;
}

int get_freeblockcount()
{
    uint32_t from_basedatablock = 41;
    int freeblockcount = 0;
    for (int i = 0; i < FREEBLOCK_BITVECTOR_SIZE; i++)
    {
        uint16_t currentportion = superblock.freeblock_bitvector[i];
        for (int j = 0x0001; j <= 0x8000; j = j << 1)
        {
            if ((uint16_t)(j)&currentportion)
            {
                freeblockcount++;
            }
            from_basedatablock++;
            if (from_basedatablock == superblock.blockcount)
                break;
        }
    }
    return freeblockcount;
}

int get_freesize()
{
    return get_freeblockcount() * BLOCKSIZE;
}

uint32_t get_lastallocatedblock(uint32_t startblock)
{
    if (startblock == NO_START_BLOCK)
        return NO_START_BLOCK;
    uint32_t fatblock = FAT_BLOCK(startblock);
    uint32_t fatoffset = FAT_OFFSET(startblock);
    uint32_t currfatentry = fattable[fatblock].entries[fatoffset];
    uint32_t prevfatentry = startblock;
    while (currfatentry != FAT_LIST_NULL)
    {
        fatblock = FAT_BLOCK(prevfatentry);
        fatoffset = FAT_OFFSET(prevfatentry);
        prevfatentry = currfatentry;
        currfatentry = fattable[fatblock].entries[fatoffset];
    }
    return prevfatentry;
}

/**********************************************************************
  Utility Functions
***********************************************************************/
// #define PRINT_ROOTDIR
#ifdef PRINT_ROOTDIR
#define print_dir(func, ...) func(##__VA_ARGS__)
#else
#define print_dir(func) (void)0
#endif
void print_rootdir(void)
{
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 16; j++)
        {
            directory_entry entry = rootdir[i].entries[j];
            vsfs_info("formatted entry: isoccupied: %d, start block: %d, filesize: %ld, filename: %s\n",
                      entry.isoccupied, entry.startblock, entry.filesize, entry.filename);
        }
    }
}

// #define PRINT_FATTABLE
#ifdef PRINT_FATTABLE
#define print_table(func, ...) func(##__VA_ARGS__)
#else
#define print_table(func) (void)0
#endif
void print_fattable(void)
{
    for (int i = 0; i < 32; i++)
    {
        for (int j = 0; j < 512; j++)
        {
            uint32_t entry = fattable[i].entries[j];
            vsfs_info("entry in FAT block %d, offset %d is %u\n", i, j, entry);
        }
    }
}

data_block *new_datablock()
{
    data_block *block = (data_block *)calloc(1, sizeof(data_block));
    return block;
}
/**********************************************************************
   The following functions are to be called by applications directly.
***********************************************************************/
int format_datablocks(int count)
{
    vsfs_assert(sizeof(data_block) == BLOCKSIZE);
    data_block *newblock = new_datablock();
    for (int i = 41; i < count; i++)
    {
        int status = write_block((void *)newblock, i);
        if (status == -1)
            return -1;
    }
    free(newblock);
    return 0;
}

int format_fattable()
{
    vsfs_assert(sizeof(fat_table_block) == BLOCKSIZE);
    size_t fat_entry_size = sizeof(uint32_t);
    vsfs_assert(fat_entry_size == 4);
    int perblockentry = BLOCKSIZE / fat_entry_size;
    vsfs_assert(perblockentry == 512);
    fat_table_block *block = (fat_table_block *)malloc(sizeof(fat_table_block));
    vsfs_assert(sizeof(*block) == BLOCKSIZE);
    for (int i = 0; i < perblockentry; i++)
    {
        block->entries[i] = FAT_LIST_NULL;
    }

    for (int i = 1; i <= 32; i++)
    {
        int status = write_block((void *)block, i);
        if (status == -1)
            return -1;
    }
    free(block);
    return 0;
}

int format_superblock(int count)
{
    vsfs_assert(sizeof(super_block) == BLOCKSIZE);
    super_block *superblocktemp = (super_block *)malloc(sizeof(super_block));
    superblocktemp->blockcount = count;
    superblocktemp->blocksize = BLOCKSIZE;
    for (int i = 0; i < 1528; i++)
    {
        superblocktemp->padding[i] = 0;
    }
    for (int i = 0; i < FREEBLOCK_BITVECTOR_SIZE; i++)
    {
        superblocktemp->freeblock_bitvector[i] = UINT16_MAX;
    }
    // set all the first 41 blocks to used, bit 0 to bit 40
    // used by the file system
    // superblock->freeblock_bitvector[0] = 0x0000; // 16 bits
    // superblock->freeblock_bitvector[1] = 0x0000; // + 16 bits
    // superblock->freeblock_bitvector[2] = 0x1100; // + 8 bits
    // 40 bits
    int status = write_block((void *)superblocktemp, 0);
    if (status == -1)
        return -1;
    free(superblocktemp);
    return 0;
}

int format_rootdir()
{
    // take the blocks indexed from 33 to 40 and initialize all directory entries to null
    vsfs_assert(sizeof(root_dir_block) == BLOCKSIZE);
    vsfs_info("directory entry size is: %ld\n", sizeof(directory_entry));
    vsfs_assert(sizeof(directory_entry) == 128);

    directory_entry entry;
    entry.filesize = 0;
    strncpy(entry.filename, "\0", sizeof(entry.filename));
    entry.isoccupied = false;
    entry.startblock = NO_START_BLOCK;
    vsfs_assert(sizeof(entry) <= 128);
    vsfs_info("formatted entry: isoccupied: %d, start block: %d, filesize: %ld, filename: %s\n",
              entry.isoccupied, entry.startblock, entry.filesize, entry.filename);
    root_dir_block *block = (root_dir_block *)malloc(sizeof(root_dir_block));
    vsfs_assert(sizeof(*block) == BLOCKSIZE);
    for (int i = 0; i < 16; i++)
    {
        memcpy(block->entries + i, &entry, sizeof(directory_entry));
    }

    for (int i = 33; i <= 40; i++)
    {
        int status = write_block(block, i);
        if (status == -1)
            return -1;
    }
    free(block);
    return 0;
}

// this function is partially implemented.
int vsformat(char *vdiskname, unsigned int m)
{
    // validate m, max disksize is 2^23 bytes and min disksize is 2^18 bytes
    if (m < 18 || m > 23)
    {
        vsfs_err("m value must be between 18 and 23 only\n");
        return -1;
    }
    int size;
    int num = 1;
    int count;
    size = num << m;
    count = size / BLOCKSIZE;
    vsfs_info("%d %d\n", m, size);

    // memory map disk
    vs_fd = open(vdiskname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ftruncate(vs_fd, size) != 0)
    {
        vsfs_err("failed to truncate vdisk\n");
        return -1;
    }

    struct stat sb;
    if (fstat(vs_fd, &sb))
    {
        vsfs_err("failed to get vdisk size info\n");
        return -1;
    }
    vsfs_info("vsdisk size: %ld\n", sb.st_size);
    vsfs_assert(sb.st_size == size);
    int status;
    status = format_superblock(count);
    if (status == -1)
        return -1;

    vsfs_info("here 1\n");
    status = format_rootdir();
    if (status == -1)
        return -1;
    status = format_fattable(count);
    if (status == -1)
        return -1;

    status = format_datablocks(count);
    if (status == -1)
        return -1;
    close(vs_fd);
    return (0);
}

// this function is partially implemented.
int vsmount(char *vdiskname)
{
    for (int i = 0; i < 128; i++)
    {
        openfiletable[i].free = true;
    }
    // open the Linux file vdiskname and in this
    // way make it ready to be used for other operations.
    // vs_fd is global; hence other function can use it.
    vs_fd = open(vdiskname, O_RDWR);
    // load (chache) the superblock info from disk (Linux file) into memory
    // load the FAT table from disk into memory
    // load root directory from disk into memory

    int status = read_block((void *)(&superblock), 0);
    if (status == -1)
        return -1;
    vsfs_info("on mount, superblock block count: %d\n", superblock.blockcount);
    vsfs_info("on mount, superblock block size: %d\n", superblock.blocksize);

    for (int i = 33; i <= 40; i++)
    {
        status = read_block(rootdir + (i - 33), i);
        if (status == -1)
        {
            return -1;
        }
    }
    // print_dir(print_rootdir);

    for (int i = 1; i <= 32; i++)
    {
        status = read_block(fattable + (i - 1), i);
        if (status == -1)
            return -1;
    }
    print_table(print_fattable);

    return (0);
}

// this function is partially implemented.
int vsumount()
{
    // write superblock to virtual disk file
    int status = write_block((void *)(&superblock), 0);
    if (status == -1)
        return -1;
    // write FAT to virtual disk file
    for (int i = 1; i <= 32; i++)
    {
        status = write_block(fattable + (i - 1), i);
        if (status == -1)
            return -1;
    }

    // write root directory to virtual disk file
    for (int i = 33; i <= 40; i++)
    {
        status = write_block(rootdir + (i - 33), i);
        if (status == -1)
            return -1;
    }

    fsync(vs_fd); // synchronize kernel file cache with the disk
    close(vs_fd);
    return (0);
}

int vscreate(char *filename)
{
    int length = strlen(filename);
    if (length >= 30)
    {
        return -1;
    }

    vsfs_info("creating file with name %s\n", filename);
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 16; j++)
        {
            directory_entry *entry = &rootdir[i].entries[j];
            if (entry->isoccupied)
            {
                if (strcmp(entry->filename, filename) == 0)
                {
                    return -1;
                }
            }
        }
    }

    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 16; j++)
        {
            directory_entry *entry = &rootdir[i].entries[j];
            if (!entry->isoccupied)
            {
                entry->isoccupied = true;
                strncpy(entry->filename, filename, sizeof(entry->filename) - 1);
                entry->filename[sizeof(entry->filename) - 1] = '\0';
                vsfs_info("vscreate: file-> isoccupied: %d, start block: %d, filesize: %ld, filename: %s\n",
                          entry->isoccupied, entry->startblock, entry->filesize, entry->filename);
                return 0;
            }
        }
    }
    return -1;
}

int vsopen(char *file, int mode)
{
    // if have already allocated all the files
    int tableoffset = 0;
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 16; j++)
        {
            directory_entry *entry = &rootdir[i].entries[j];
            if (strcmp(entry->filename, file) == 0)
            {
                // if already opened, check for mode
                if (!openfiletable[tableoffset].free &&
                    openfiletable[tableoffset].mode != mode)
                {
                    return -1;
                }
                openfiletable[tableoffset].entry = entry;
                openfiletable[tableoffset].mode = mode;
                openfiletable[tableoffset].free = false;

                directory_entry *openentry = openfiletable[tableoffset].entry;
                vsfs_info("vsopen: file-> isoccupied: %d, start block: %d, filesize: %ld, filename: %s\n",
                          openentry->isoccupied, openentry->startblock, openentry->filesize, openentry->filename);
                return tableoffset;
            }
            tableoffset++;
        }
    }
    return -1;
}

int vsclose(int fd)
{
    if (fd < 0 && fd > 128)
        return -1;
    if (openfiletable[fd].free)
        return -1;
    openfiletable[fd].free = true;
    return (0);
}

int vssize(int fd)
{
    if (fd < 0 && fd > 128)
        return -1;
    if (openfiletable[fd].free == true)
        return -1;
    return openfiletable[fd].entry->filesize;
}

int vsread(int fd, void *buf, int n)
{
    if (fd < 0 || fd > 128)
        return -1;
    if (openfiletable[fd].free == true)
        return -1;
    if (openfiletable[fd].mode != MODE_READ)
        return -1;

    if (openfiletable[fd].entry->filesize == 0)
    {
        vsfs_assert(openfiletable[fd].entry->startblock == NO_START_BLOCK);
        return 0;
    }

    vsfs_info("reading file %s\n", openfiletable[fd].entry->filename);
    // map buffer to underyling bytestream
    uint8_t *bytestream = (uint8_t *)buf;

    data_block *datablock = new_datablock();
    int datablockindex = 0;
    uint32_t currblocknumber = openfiletable[fd].entry->startblock;
    int readstatus = read_block((void *)datablock, currblocknumber);
    if (readstatus == -1)
        return -1;
    for (int i = 0; i < n; i++)
    {
        datablockindex = i % BLOCKSIZE;
        bytestream[i] = datablock->data[datablockindex];
        vsfs_info("read value %x\n", bytestream[i]);
        if (datablockindex == BLOCKSIZE - 1)
        {
            currblocknumber = fattable[FAT_BLOCK(currblocknumber)].entries[FAT_OFFSET(currblocknumber)];
            if (currblocknumber == FAT_LIST_NULL)
            {
                break;
            }
            int readstatus = read_block((void *)datablock, currblocknumber);
            if (readstatus == -1)
                return -1;
        }
    }
    return 0;
}

void itervative_append(
    uint32_t currblock,
    uint32_t prevblock,
    int endingstreamidx,
    uint8_t *bytestream,
    int startingstreamidx)
{
    data_block *datablock = new_datablock();
    int datablockindex = 0;
    uint32_t currblocknumber = currblock;
    uint32_t prevblocknumber = prevblock;
    fattable[FAT_BLOCK(prevblocknumber)].entries[FAT_OFFSET(prevblocknumber)] = currblocknumber;
    fattable[FAT_BLOCK(currblocknumber)].entries[FAT_OFFSET(currblocknumber)] = FAT_LIST_NULL;
    bool flagnewblock = false;
    for (int i = 0; i < endingstreamidx; i++)
    {
        if (flagnewblock)
        {
            prevblocknumber = currblocknumber;
            currblocknumber = get_nextfreeblock();
            fattable[FAT_BLOCK(prevblocknumber)].entries[FAT_OFFSET(prevblocknumber)] = currblocknumber;
            fattable[FAT_BLOCK(currblocknumber)].entries[FAT_OFFSET(currblocknumber)] = FAT_LIST_NULL;
            flagnewblock = false;
        }
        datablockindex = i % BLOCKSIZE;
        if (startingstreamidx != 0)
        {
            datablock->data[datablockindex] = bytestream[startingstreamidx + i + 1];
        }
        else
        {
            datablock->data[datablockindex] = bytestream[i];
        }
        if (datablockindex == BLOCKSIZE - 1)
        {
            // if was last element to write, flush current data block to disk
            write_block((void *)datablock, currblocknumber);
            flagnewblock = true;
        }
    }
    if (flagnewblock == false)
    {
        // the current written block was not flushed
        fattable[FAT_BLOCK(prevblocknumber)].entries[FAT_OFFSET(prevblocknumber)] = currblocknumber;
        write_block((void *)datablock, currblocknumber);
    }
    free(datablock);
}

int vsappend(int fd, void *buf, int n)
{
    if (fd < 0 || fd > 128)
        return -1;
    if (openfiletable[fd].free == true)
        return -1;
    if (openfiletable[fd].mode != MODE_APPEND)
        return -1;

    // map the underlying buffer to a byte stream
    uint8_t *bytestream = (uint8_t *)malloc(sizeof(uint8_t) * n);
    memcpy(bytestream, buf, sizeof(uint8_t) * n);

    uint32_t startblock = openfiletable[fd].entry->startblock;
    uint32_t lastallocatedblock = get_lastallocatedblock(startblock);
    if (lastallocatedblock == NO_START_BLOCK)
    {
        vsfs_assert(openfiletable[fd].entry->filesize == 0);
        openfiletable[fd].entry->startblock = get_nextfreeblock();

        itervative_append(
            openfiletable[fd].entry->startblock,
            0,
            n,
            bytestream,
            0);
        openfiletable[fd].entry->filesize = n;
        free(bytestream);
        return 0;
    }
    else
    {
        // check if the lastallocatedblock still has available space
        uintmax_t currsize = openfiletable[fd].entry->filesize;
        bool haslastblockspace = currsize % BLOCKSIZE != 0;
        if (haslastblockspace)
        {
            fflush(stdout);
            // load the old block
            data_block *datablock = new_datablock();
            int readstatus = read_block((void *)datablock, lastallocatedblock);
            uintmax_t availablelastblocksize = BLOCKSIZE - (currsize % BLOCKSIZE);
            off_t lastblockoffset = (currsize % BLOCKSIZE);
            fflush(stdout);
            vsfs_assert(availablelastblocksize < BLOCKSIZE && availablelastblocksize > 0);
            if (availablelastblocksize >= n)
            {
                for (int i = 0; i < n; i++)
                {
                    datablock->data[lastblockoffset] = bytestream[i];
                    lastblockoffset++;
                }
                write_block((void *)datablock, lastallocatedblock);
                free(datablock);
                free(bytestream);
                openfiletable[fd]
                    .entry->filesize = currsize + n;
                return 0;
            }
            else
            {
                int bytestreamidx = 0;
                for (; bytestreamidx < availablelastblocksize; bytestreamidx++)
                {
                    datablock->data[lastblockoffset] = bytestream[bytestreamidx];
                    lastblockoffset++;
                }
                // block is completely full
                write_block((void *)datablock, lastallocatedblock);
                // start allocating blocks
                free(datablock);
                itervative_append(
                    get_nextfreeblock(),
                    lastallocatedblock,
                    (n - bytestreamidx - 1),
                    bytestream,
                    bytestreamidx);
                openfiletable[fd].entry->filesize = currsize + n;
                free(datablock);
                free(bytestream);
                return 0;
            }
        }
        else
        {
            fflush(stdout);
            // begin protocol with new block allocation
            itervative_append(
                get_nextfreeblock(),
                lastallocatedblock,
                n,
                bytestream,
                0);
            openfiletable[fd].entry->filesize = currsize + n;
            free(bytestream);
            return 0;
        }
        // outside has last block space if
    }
}

int vsdelete(char *filename)
{
    bool exists = false;
    int blockidx = 0, offsetidx = 0;
    for (int i = 0; i < 8; i++)
    {
        for (int j = 0; j < 16; j++)
        {
            directory_entry *entry = &rootdir[i].entries[j];
            if (strcmp(entry->filename, filename) == 0)
            {
                blockidx = i;
                offsetidx = j;
                exists = true;
                break;
            }
        }
    }
    if (!exists)
        return -1;

    uint16_t startblock = rootdir[blockidx].entries[offsetidx].startblock;
    // delete file entry from rootdir
    rootdir[blockidx].entries[offsetidx].filesize = 0;
    for (int i = 0; i < 30; i++)
    {
        rootdir[blockidx].entries[offsetidx].filename[i] = '\0';
    }
    rootdir[blockidx].entries[offsetidx].isoccupied = false;
    rootdir[blockidx].entries[offsetidx].startblock = NO_START_BLOCK;

    vsfs_info("vsdelete: file entry -> isoccupied: %d, filesize: %ld, startblock: %u, filename: %s\n",
              rootdir[blockidx].entries[offsetidx].isoccupied,
              rootdir[blockidx].entries[offsetidx].filesize,
              rootdir[blockidx].entries[offsetidx].startblock,
              rootdir[blockidx].entries[offsetidx].filename);

    data_block *emptyblock = new_datablock();
    uint16_t currblock = startblock;
    while (currblock != FAT_LIST_NULL)
    {
        int status = write_block((void *)emptyblock, currblock);
        if (status != 0)
        {
            vsfs_err("failed to write empty block");
            return -1;
        }
        currblock = fattable[FAT_BLOCK(currblock)].entries[FAT_OFFSET(currblock)];
        fattable[FAT_BLOCK(currblock)].entries[FAT_OFFSET(currblock)] = FAT_LIST_NULL;
    }

    vsfs_info("file deleted %s", filename);
    print_fattable();
    free(emptyblock);
    return 0;
}