#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdlib.h>
#include <string.h>
#include <iostream>

// 内存块节点 (链表结构，如果不够用了就挂一个新的块上去)
struct MemoryBlock
{
    size_t size;       // 本块的总大小
    size_t used;       // 本块已经分配出去的大小
    MemoryBlock *next; // 指向下一个内存块
    char *data;        // 真正的内存数据区
};

class MemoryPool
{
public:
    // 默认开辟 4KB 的大块，和 Linux 操作系统的内存页大小一致，效率最高
    MemoryPool(size_t block_size = 4096);
    ~MemoryPool();

    // 分配内存的核心函数 (替代 new / malloc)
    void *allocate(size_t size);

    // 连接断开/请求结束时调用，清空数据，但不还给操作系统！
    void reset();

private:
    // 如果当前块不够了，申请新块
    void *allocate_new_block(size_t size);

private:
    size_t m_block_size;
    MemoryBlock *m_head;    // 链表头指针
    MemoryBlock *m_current; // 当前正在使用的块指针
};

#endif