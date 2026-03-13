#include "memory_pool.h"

// 构造函数：向操作系统批发第一块大内存
MemoryPool::MemoryPool(size_t block_size) : m_block_size(block_size)
{
    m_head = (MemoryBlock *)malloc(sizeof(MemoryBlock));
    m_head->data = (char *)malloc(m_block_size);
    m_head->size = m_block_size;
    m_head->used = 0;
    m_head->next = nullptr;
    m_current = m_head;
}

// 析构函数：服务器销毁时，才真正把内存还给操作系统
MemoryPool::~MemoryPool()
{
    MemoryBlock *curr = m_head;
    while (curr != nullptr)
    {
        MemoryBlock *next = curr->next;
        free(curr->data); // 释放数据区
        free(curr);       // 释放控制头
        curr = next;
    }
}

// 分配内存
void *MemoryPool::allocate(size_t size)
{
    // 【内核级优化】：内存对齐到 8 字节边界。
    // CPU 读取对齐的内存比不对齐的内存快得多！
    size = (size + 7) & ~7;

    // 1. 如果当前块容量够，直接切一块走
    if (m_current->size - m_current->used >= size)
    {
        char *ptr = m_current->data + m_current->used;
        m_current->used += size; // 移动游标
        return ptr;              // 返回地址
    }

    // 2. 如果当前块不够了，开辟一个新块挂上去
    return allocate_new_block(size);
}

// 申请新块
void *MemoryPool::allocate_new_block(size_t size)
{
    // 新块的大小：如果用户要的很大，就按用户要的给；否则就给默认的 4KB
    size_t alloc_size = (size > m_block_size) ? size : m_block_size;

    MemoryBlock *new_block = (MemoryBlock *)malloc(sizeof(MemoryBlock));
    new_block->data = (char *)malloc(alloc_size);
    new_block->size = alloc_size;
    new_block->used = size; // 刚分配完就拨出去 size 这么大
    new_block->next = nullptr;

    // 挂在链表末尾，更新 m_current 指针
    m_current->next = new_block;
    m_current = new_block;

    return new_block->data;
}

// 这个函数会在 HTTP 请求解析完毕、发完数据给客户端后调用。
void MemoryPool::reset()
{
    // 不 free 内存！只把 used 游标清零！
    // 下次这个连接再发请求来，直接覆盖旧数据
    MemoryBlock *curr = m_head;
    while (curr != nullptr)
    {
        curr->used = 0;
        curr = curr->next;
    }
    m_current = m_head;
}