/*
 * 9.20
 *
 * mofaph@gmail.com
 *
 * 编写你自己的 malloc 和 free 版本，将它的运行时间和空间利用率与标准 C 库提供的
 * malloc 版本进行比较。
 *
 * 对于 32 位系统，最小块是 8 字节；对于 64 位系统，最小块是 16 字节。都是 8 字节
 * 对齐。
 *
 * WARNING: This program is buggy.
 */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include <unistd.h>

#define CHUNK (1<<20)           /* 默认向系统请求 1MB 的堆空间 */
#define BLOCK (1<<12)           /* 记录空闲块的数据结构 */

#define ALIGN (1<<3)            /* 8 字节对齐 */

struct free_block {
        long size;              /* 空闲块的大小 */
        char *addr;             /* 空闲块的地址 */
};

#define header_size (offsetof(struct free_block, addr))
#define free_block_size(n) ((n) * sizeof(struct free_block))

#define MAX(m, n) ((m) < (n) ? (n) : (m))

static struct free_block *start, *end;

struct free_block *find_fit(size_t size);
void place(struct free_block *bp, long asize);
void coalesce(void);

static int min_block(void)
{
        int header = header_size;

        if (header == ALIGN)
                return ALIGN << 1;
        if (header < ALIGN)
                return ALIGN;
        return (header + 7) & ~7;
}

static void swap(struct free_block *b1, struct free_block *b2)
{
        struct free_block tmp;

        tmp = *b1;
        *b1 = *b2;
        *b2 = tmp;
}

#define SORT_BY_ADDRESS 001
#define SORT_BY_SIZE    002

static void block_quick_sort(struct free_block *start, struct free_block *end, int type)
{
        if (start >= end)
                return;

        int k = (end - start) >> 1;
        swap(start, start+k);

        struct free_block *last = start;
        int i;
        for (i = 1; start+i < end; i++) {
                struct free_block *block = start+i;
                if ((type == SORT_BY_ADDRESS && block->addr < start->addr) ||
                    (type == SORT_BY_SIZE && block->size < start->size)) {
                        last += 1;
                        swap(last, block);
                }
        }
        swap(start, last);

        block_quick_sort(start, last, type);
        block_quick_sort(last+1, end, type);
}

static long block_binary_search(long size)
{
        long first = 0;
        long last = end - start;
        long mid = 0;

        while (last > first) {
                mid = (last + first) >> 1;
                struct free_block *ptr = start + mid;
                if (ptr->size < size) {
                        first = mid + 1;
                        mid += 1;
                } else if (ptr->size > size) {
                        last = mid;
                } else /* ptr->size == size */ {
                        return mid;
                }
        }

        return -mid;
}

/*
 * 在这里，空闲块集合中是有序的，按块大小的升序排列（从小到大）
 */
static void insert_free_block(struct free_block *block)
{
        long index = block_binary_search(block->size);
        if (index < 0)
                index = -index;

        end += 1;
        int nblocks = end - start - index;
        size_t size = free_block_size(nblocks);
        memmove(start+index+1, start+index, size);

        *(start+index) = *block;
}

static void remove_free_block(struct free_block *block)
{
        long index = block_binary_search(block->size);
        if (index < 0)
                return;

        int nblocks = end - start - index;
        size_t size = free_block_size(nblocks);
        memmove(start+index, start+index+1, size);
        end -= 1;
}

/*
 * 这个函数用于分割块之后，把余下的空闲块放到合适的位置
 */
static void adjust_free_block(struct free_block *block)
{
        long index = block_binary_search(block->size);
        if (index < 0)
                index = -index;

        struct free_block tmp = *block;
        int nblocks = block - start - index;
        size_t size = free_block_size(nblocks);
        memmove(start+index+1, start+index, size);
        *(start+index) = tmp;
}

int mo_init(void)
{
        void *addr = sbrk(CHUNK+BLOCK);
        if (addr == (void *)-1) {
                /* 当失败时，sbrk 已经设置了 errno */
                return -1;
        }

        struct free_block padding = { 0, NULL };
        start = (struct free_block *)((char *)addr + CHUNK);
        *start = padding;
        end = start + 2;
        *end = padding;

        struct free_block *ptr = start + 1;
        ptr->addr = addr;
        ptr->size = CHUNK;

        return 0;
}

void *mo_malloc(size_t size)
{
        size_t asize = size + offsetof(struct free_block, addr);
        asize = (asize+ALIGN-1) / ALIGN * ALIGN; /* 字节对齐 */

        struct free_block *fit = find_fit(asize);
        if (fit == NULL) {
                coalesce();
                fit = find_fit(asize);
                if (fit == NULL) {
                        size_t alloc_size = MAX(asize, CHUNK);
                        void *old_heap = sbrk(alloc_size);
                        if (old_heap == (void *)-1) {
                                /* errno 已经在 sbrk() 中设置了 */
                                return NULL;
                        }
                        fit = start;

                        int nblocks = end - start + 1;
                        long block_size = free_block_size(nblocks);
                        memmove(start+alloc_size, start, block_size);
                        start += alloc_size;
                        end += alloc_size;

                        struct free_block block;
                        block.addr = (char *)fit;
                        block.size = alloc_size;
                        insert_free_block(&block);
                }
        }

        /*
         * 找到一个空闲块，在空闲块头部填入大小，可能会分割空闲块。因此，就有可能
         * 改变 fit 所指结构的内容，所以在调用 place() 之前，需要保存空闲块的有效
         * 地址
         */
        char *addr = fit->addr + sizeof(fit->size);
        place(fit, asize);
        return addr;
}

void mo_free(void *ptr)
{
        int offset = offsetof(struct free_block, addr);
        char *addr = (char *)ptr - offset;
        long size = *((long *)addr);

        struct free_block block;
        block.size = size;
        block.addr = addr;

        insert_free_block(&block);
}

void coalesce(void)
{
        block_quick_sort(start, end, SORT_BY_ADDRESS);
        struct free_block *curr, *next;
        curr = start + 1;
        next = curr + 1;
        while (next != end) {
                if (curr->addr + curr->size == next->addr) {
                        curr->size += next->size;
                        int nblocks = end - next;
                        long size = free_block_size(nblocks);
                        memmove(next, next+1, size);
                        end -= 1;
                } else {
                        curr += 1;
                        next = curr + 1;
                }
        }
        block_quick_sort(start, end, SORT_BY_SIZE);
}

struct free_block *find_fit(size_t size)
{
        long index = block_binary_search(size);

        if (index == 0)
                return NULL;

        if (index < 0) {
                index = -index;
                if (end <= start + index)
                        return NULL;
        }

        return start+index;
}

void place(struct free_block *bp, long asize)
{
        long *header = (long *)bp->addr;
        long reserve = bp->size - asize;
        long min = min_block();

        if (reserve >= min) {
                *header = asize;
                bp->addr += asize;
                bp->size -= asize;
                adjust_free_block(bp);
        } else {
                *header = bp->size;
        }
}
