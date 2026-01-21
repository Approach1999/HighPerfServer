#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 模板类 T 是任务类
template <typename T>
class threadpool {
public:
    // thread_number: 线程池中线程的数量
    // max_requests: 请求队列中允许的最大请求数
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    
    // 往队列里添加任务
    bool append(T *request);

private:
    // 工作线程运行的函数，它会不断从队列里取任务
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t *m_threads;       // 描述线程池的数组，其大小为 m_thread_number
    std::list<T *> m_workqueue; // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 是否有任务需要处理（信号量）
};



template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) 
    : m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL) {
    
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();

    // 创建 thread_number 个线程，并将它们设置为脱离线程
    for (int i = 0; i < thread_number; ++i) {
        // worker 是静态函数，必须把 this 指针传进去，否则 worker 访问不到类的成员变量
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        
        // 设置为 detach，线程结束自动释放资源
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request) {
    // 操作工作队列时一定要加锁，因为它是被所有线程共享的
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    
    // 信号量 +1，提醒有任务了
    m_queuestat.post(); 
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *)arg;
    pool->run(); // 真正干活的函数
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while (true) {
        // 等待信号量（如果没有任务，线程会阻塞在这里睡觉，不占 CPU）
        m_queuestat.wait();
        
        // 被唤醒了，加锁取任务
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        // 取出第一个任务
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request)
            continue;

        // 执行任务 (这里假设任务类 T 有一个 process 方法)
        // 这里的 Reactor 模式是：主线程只管读写，数据读完了交给子线程处理逻辑
        request->process(); 
    }
}

#endif