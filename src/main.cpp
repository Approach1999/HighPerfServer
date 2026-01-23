#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536           // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 一次监听的最大事件数量

// 信号处理函数：添加信号捕捉
// 比如：忽略 SIGPIPE 信号，防止客户端异常断开导致服务器崩溃
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 错误处理辅助函数
void show_error(int connfd, const char *info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
    // 默认端口 8080，也可以通过命令行参数传递
    int port = 8080;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // 1. 忽略 SIGPIPE 信号
    // 如果客户端关了，服务器还往里写数据，会触发 SIGPIPE，默认行为是终止进程
    // 我们要忽略它，保证服务器不挂
    addsig(SIGPIPE, SIG_IGN);

    // 2. 创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch (...) {
        return 1;
    }

    // 3. 预先为每个可能的 client 连接分配一个 http_conn 对象
    // 就像去餐厅吃饭，桌子早就摆好了，客人来了直接入座
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 4. 创建监听 Socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    
    // 设置端口复用 (SO_LINGER 也是一种优化，这里先用基础的 REUSEADDR)
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 5. 创建 Epoll 对象
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    // 把监听 socket 加进 Epoll
    // 注意：监听 socket 不需要 ONESHOT，因为我们希望任何时候有新连接都能立刻触发
    http_conn::addfd(epollfd, listenfd, false);
    
    // 把 epollfd 赋给 http_conn 的静态成员，让所有对象共享
    http_conn::m_epollfd = epollfd;

    printf("服务器启动成功！监听端口: %d\n", port);

    // ==========================================
    // 6. 主循环 (Reactor 模式)
    // ==========================================
    while (true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        // 遍历就绪的事件
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 情况一：有新连接进来 (Listen Socket 可读)
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                
                // ET 模式下，可能同一时刻来了多个连接，所以要循环 accept
                // 但为了代码简单，这里先只 accept 一次。
                // 严格的 ET 模式这里需要 while 循环 accept 直到返回 -1
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                
                if (http_conn::m_user_count >= MAX_FD) {
                    show_error(connfd, "Internal server busy");
                    printf("连接数已满\n");
                    continue;
                }
                
                // 初始化这个新连接 (分配桌子)
                users[connfd].init(connfd, client_address);
            }
            // 情况二：对方异常断开 / 错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockfd].close_conn();
            }
            // 情况三：有数据发来了 (读事件)
            else if (events[i].events & EPOLLIN) {
                // 主线程负责一次性把数据读完
                if (users[sockfd].read()) {
                    // 读成功了，扔给线程池去解析逻辑
                    pool->append(users + sockfd);
                } else {
                    // 读失败了 (比如对方关闭)
                    users[sockfd].close_conn();
                }
            }
            // 情况四：可以发送数据了 (写事件)
            else if (events[i].events & EPOLLOUT) {
                // 主线程负责把响应写回去
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}