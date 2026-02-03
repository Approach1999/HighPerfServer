#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <string>   
#include "locker.h"

class http_conn {
public:
    // 设置文件描述符为非阻塞 (ET模式必须)
    static int setnonblocking(int fd);
    
    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    static void addfd(int epollfd, int fd, bool one_shot);
    
    // 从内核时间表删除描述符
    static void removefd(int epollfd, int fd);
    
    // 修改描述符，重置 socket 上的 EPOLLONESHOT 事件，确保下一次可读时，EPOLLIN 再次被触发
    static void modfd(int epollfd, int fd, int ev);

public:
    static int m_user_count; // 统计所有的用户数量
    static int m_epollfd;    // 所有 socket 上的事件都被注册到同一个 epoll 内核事件表中，所以设置成静态的

public:
    // HTTP请求方法，这里只支持 GET 和 POST
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };
    
    // 解析客户端请求时，主状态机所处的状态
    enum CHECK_STATE { 
        CHECK_STATE_REQUESTLINE = 0, // 当前正在分析请求行
        CHECK_STATE_HEADER,          // 当前正在分析头部字段
        CHECK_STATE_CONTENT          // 当前正在分析请求体
    };
    
    // 服务器处理 HTTP 请求的可能结果
    enum HTTP_CODE { 
        NO_REQUEST,        // 请求不完整，需要继续读取客户数据
        GET_REQUEST,       // 获得了一个完整的客户请求
        BAD_REQUEST,       // 客户请求有语法错误
        NO_RESOURCE,       // 也就是 404
        FORBIDDEN_REQUEST, // 也就是 403
        FILE_REQUEST,      // 文件请求（获取文件成功）
        INTERNAL_ERROR,    // 500 错误
        CLOSED_CONNECTION  // 客户端关闭连接
    };
    
    // 从状态机的三种可能状态：行读取状态
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

    // 初始化新接受的连接
    void init(int sockfd, const sockaddr_in &addr);
    
    // 关闭连接
    void close_conn(bool real_close = true);
    
    // 处理客户请求（这是线程池 worker 调用的入口函数）
    void process();
    
    // 非阻塞读操作
    bool read();
    
    // 非阻塞写操作
    bool write();

private:
    // 初始化连接（内部私有）
    void init();
    
    // --- 下面是 HTTP 解析相关的函数 (TODO) ---
    HTTP_CODE process_read();           // 解析 HTTP 请求
    bool process_write(HTTP_CODE ret);  // 填充 HTTP 响应

    // 这一组函数被 process_read 调用以分析 HTTP 请求
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    bool check_login(const std::string& username, const std::string& password);

    // 这一组函数被 process_write 调用以填充 HTTP 响应
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static const int FILENAME_LEN = 200;       // 文件名最大长度
    static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区大小


    static char* m_doc_root;      // 网站根目录
    static char* m_db_url;        // 数据库连接串
    static char* m_db_user;
    static char* m_db_password;
    static char* m_db_name;

private:
    int m_sockfd;           // 该 HTTP 连接的 socket
    sockaddr_in m_address;  // 该 HTTP 连接的对方的 socket 地址
    
    // 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    // 标识读缓冲中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;
    // 当前正在解析的行的起始位置
    int m_start_line;

    // 写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 写缓冲区中待发送的字节数
    int m_write_idx;

    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;

    // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    // 客户请求的目标文件的文件名
    char *m_url;
    // HTTP协议版本号，我们仅支持 HTTP/1.1
    char *m_version;
    // 主机名
    char *m_host;
    // HTTP请求的消息体的长度
    int m_content_length;
    // HTTP请求是否要求保持连接
    bool m_linger;

    // 客户请求的目标文件被 mmap 到内存中的起始位置
    char *m_file_address;
    // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    
    // 我们将采用 writev 来执行写操作，所以定义下面两个成员
    // m_iv[0] 存 header，m_iv[1] 存文件内容 (mmap)
    struct iovec m_iv[2];
    int m_iv_count;
};

#endif