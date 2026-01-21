#include "http_conn.h"

// 定义 HTTP 响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录
const char *doc_root = "/home/jasper/HighPerfServer/resources";

// 设置非阻塞
int http_conn::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向 Epoll 添加需要监听的文件描述符
void http_conn::addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    
    // EPOLLIN: 数据可读
    // EPOLLET: 边缘触发 (Edge Triggered)
    // EPOLLRDHUP: TCP连接被对方关闭
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    
    // EPOLLONESHOT: 
    // 防止一个线程正在处理 socket 时，又有新数据来了，触发了另一个线程也来处理同一个 socket
    // 加上这个，同一时间只能有一个线程处理这个 socket
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd); // 记得设为非阻塞
}

// 从 Epoll 中移除
void http_conn::removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置 ONESHOT
void http_conn::modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    // 重新加上 ONESHOT，保证下次还能被触发
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化静态成员变量
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 用户数减一
    }
}

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;
    
    // 调试用
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 添加到 Epoll 对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    
    init(); // 调用私有的 init 初始化其他变量
}

// 初始化新接受的连接（重置各种变量）
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态：正在解析请求行
    m_linger = false; // 默认不保持连接 (Connection: close)
    
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    
    memset(m_read_buf, 0, READ_BUFFER_SIZE);
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}