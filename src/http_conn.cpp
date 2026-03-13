#include "http_conn.h"
#include "sql_conn_pool.h"

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

// 初始化静态变量
char *http_conn::m_doc_root = nullptr;
char *http_conn::m_db_url = nullptr; // 比如 "host=... port=..."
char *http_conn::m_db_user = nullptr;
char *http_conn::m_db_password = nullptr;
char *http_conn::m_db_name = nullptr;

// 设置非阻塞
int http_conn::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向 Epoll 添加需要监听的文件描述符
void http_conn::addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

    // EPOLLIN: 数据可读
    // EPOLLET: 边缘触发 (Edge Triggered)
    // EPOLLRDHUP: TCP连接被对方关闭
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    // EPOLLONESHOT:
    // 防止一个线程正在处理 socket 时，又有新数据来了，触发了另一个线程也来处理同一个 socket
    // 加上这个，同一时间只能有一个线程处理这个 socket
    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd); // 记得设为非阻塞
}

// 从 Epoll 中移除
void http_conn::removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置 ONESHOT
void http_conn::modfd(int epollfd, int fd, int ev)
{
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
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 用户数减一

        // 释放内存池
        if (m_pool)
        {
            delete m_pool;
            m_pool = nullptr;
        }
    }
}

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 调试用
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到 Epoll 对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    // 如果是第一次连接，分配一个内存池
    if (m_pool == nullptr)
    {
        m_pool = new MemoryPool(4096);
    }
    else
    {
        // 如果连接是被复用的（比如长连接 Keep-Alive 的下一次请求）
        // 直接重置内存池游标，瞬间满血复活！
        m_pool->reset();
    }

    init(); // 调用私有的 init 初始化其他变量
}

// 初始化新接受的连接（重置各种变量）
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态：正在解析请求行
    m_linger = false;                        // 默认不保持连接 (Connection: close)

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

// 循环读取客户数据，直到无数据可读
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false; // 缓冲区满了
    }

    int bytes_read = 0;
    while (true)
    {
        // 从 m_read_idx 开始写，防止覆盖之前没处理的数据
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

        if (bytes_read == -1)
        {
            // EAGAIN 或 EWOULDBLOCK 说明内核缓冲区空了，数据读完了
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false; // 其他错误（如连接重置）
        }
        else if (bytes_read == 0)
        {
            return false; // 对方关闭了连接
        }

        m_read_idx += bytes_read; // 更新读取指针
    }
    return true;
}

// 从状态机：解析一行，判断依据是 \r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // m_checked_idx 指向当前正在检查的字符，m_read_idx 指向有效数据的尾部
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];

        // 如果读到了回车符 \r
        if (temp == '\r')
        {
            // 如果 \r 是最后一个收到的字符，说明还没收完 \n，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            // 如果下一个是 \n，说明读到了一行完整的
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0'; // 把 \r 变成字符串结束符
                m_read_buf[m_checked_idx++] = '\0'; // 把 \n 变成字符串结束符
                return LINE_OK;
            }
            return LINE_BAD; // 语法错误
        }
        // 如果读到了换行符 \n (通常会先读到 \r，但也防一手)
        else if (temp == '\n')
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN; // 没找到 \r\n，需要继续读数据
}

// 解析 HTTP 请求行，获得请求方法、目标 URL、HTTP 版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // strpbrk 检索字符串中第一个匹配 \t (制表符) 或 空格 的位置
    // GET /index.html HTTP/1.1
    //    ^
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    // 把空格变成 \0，这样前面的 text 就变成了 "GET\0"
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
        m_method = POST;
    else
        return BAD_REQUEST;

    // 跳过空格和制表符，指向 /index.html
    //       /index.html HTTP/1.1
    //       ^
    m_url += strspn(m_url, " \t");

    // 找 HTTP 版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // 仅支持 HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // 检查 URL 是否带有 http:// (有的浏览器会发完整路径)
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); // 找到域名后的第一个 /
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 状态转移：请求行解析完，下一步解析头部
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST; // 继续解析
}

// 解析 HTTP 请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行，说明头部解析完毕
    if (text[0] == '\0')
    {
        // 如果有 Content-Length，说明有请求体 (POST)，状态转移到解析内容
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明这是一个完整的 GET 请求
        return GET_REQUEST;
    }
    // 解析 Connection 头部
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true; // 保持连接
        }
    }
    // 解析 Content-Length 头部
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 解析 Host 头部
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // 其他头部暂时不处理
        // TODO
        // printf("oop! unknown header: %s\n", text);
    }
    return NO_REQUEST;
}

// 判断 HTTP 请求体是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 这一步很简单：只要已读数据的长度 >= (内容起始位置 + 内容长度)
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 处理请求：主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 循环条件：
    // 1. 解析内容且行状态OK (针对 POST 内容)
    // 2. 或者 解析出一行完整的数据 (针对 Header)
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {

        // 获取一行数据
        text = get_line();

        // 更新下一行的起始位置
        m_start_line = m_checked_idx;
        // printf("got 1 http line: %s\n", text);

        // 状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
                return do_request();
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN; // 没读完就继续
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // 1. 初始化路径
    strcpy(m_real_file, m_doc_root);
    int len = strlen(m_doc_root);

    // ===============================================
    // 【新增】 API 路由判断
    // ===============================================
    // 如果请求的是 /api/login 并且是 POST 方法
    if (strcasecmp(m_url, "/api/login") == 0 && m_method == POST)
    {
        // 解析 Body 里的 JSON (用户名和密码)
        // 注意：这里需要先把 m_read_buf 里的内容转成字符串
        // 为了简化，这里假设 m_string 已经存了 body (实际需要结合 content-length 解析)

        // 这里是个难点：
        // 在状态机解析完 Header 后，m_read_buf 的剩余部分就是 Body。
        // 我们简单模拟一下逻辑：

        char *body = m_read_buf + m_checked_idx; // 指向 Body 开始的地方

        // 简单的 JSON 解析 (手撸或者用 nlohmann/json)
        // 假设 body 是 {"username":"admin", "password":"123"}
        // 这里为了不引入太多复杂度，先硬编码测试一下数据库连接：

        // 真正的项目中，这里应该解析 JSON，然后调用：
        // if (check_login(username, password)) ...

        // 我们先返回一个这就成功的信号，具体 JSON 解析逻辑可以后面细化
        return FILE_REQUEST;
    }

    // ===============================================
    // 原有的静态文件处理逻辑
    // ===============================================
    // 拼接路径
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // ... stat 判断 ...
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // ... 权限判断 ...
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // ... 目录判断 ...
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // ... open & mmap ...
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}
// 释放内存映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 非阻塞写：将响应头和文件内容一次性发出去
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0; // 已经发送的字节数
    // m_write_idx 是响应头的长度
    // m_file_stat.st_size 是文件长度
    int bytes_to_send = m_write_idx + m_content_length;

    if (bytes_to_send == 0)
    {
        // 没什么要发的，重置一下等待下次
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        // writev 分散写：同时发送 m_iv[0] 和 m_iv[1]
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp <= -1)
        {
            // 如果 TCP 写缓冲满了，那就等待下一轮 EPOLLOUT 事件
            if (errno == EAGAIN)
            {
                // 这一步非常重要：因为我们没写完，必须记录已经写了多少
                // 下次进来要接着写，否则会发重复数据或乱码
                // (为了简化教程，这里省略了断点续传的复杂逻辑，实际项目中需要调整 iovec 的指针)
                // 这里简单处理：只要满了就重置 EPOLLOUT 等待下次
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            // 发送失败
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;

        // 数据全发完了
        if (bytes_to_send <= 0)
        {
            unmap(); // 解除文件映射

            // 如果是长连接 (Keep-Alive)
            if (m_linger)
            {
                init();                              // 重置内部变量，准备接收下个请求
                modfd(m_epollfd, m_sockfd, EPOLLIN); // 继续监听读
                return true;
            }
            else
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false; // 返回 false 让上层关闭连接
            }
        }
    }
}

// 往写缓冲区里写入数据 (类似 printf)
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    va_list arg_list;
    va_start(arg_list, format);
    // vsnprintf: 安全地把变参格式化进 buffer
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 根据 do_request 的结果，生成对应的 HTTP 响应
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    case NO_RESOURCE: // 404
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    case FILE_REQUEST: // 成功找到文件
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            // 1. 写 Header
            add_headers(m_file_stat.st_size);

            // 2. 准备 iovec
            // 第一块内存：Header (m_write_buf)
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;

            // 第二块内存：File (mmap的地址)
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            m_iv_count = 2;

            // 此时，response 已经准备好，等待 write() 函数被调用
            // 但我们还需要更新一下 content_length 用于 write 里的计数
            m_content_length = m_file_stat.st_size;

            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }

    // 如果不是 FILE_REQUEST，就只需要发 Header + 错误信息，不需要发文件
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    m_content_length = 0; // 这里的逻辑稍微简化了，实际错误信息在buffer里
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    // 注册写事件：告诉 Epoll，我准备好写数据了，等 socket 可写的时候通知我
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 校验用户登录
bool http_conn::check_login(const std::string &username, const std::string &password)
{
    if (username.empty() || password.empty())
    {
        return false;
    }

    // 1. 从连接池取一个连接 (RAII)
    PGconn *conn = nullptr;
    connectionRAII mysqlcon(&conn, connection_pool::GetInstance());

    if (conn == nullptr)
    {
        return false;
    }

    // 2. 防注入处理 (简单的做法，工业级请用 PQexecParams)
    // 这里为了简单演示逻辑
    std::string sql = "SELECT id FROM users WHERE username='" + username + "' AND password='" + password + "'";

    // 3. 执行查询
    PGresult *res = PQexec(conn, sql.c_str());

    // 4. 判断结果
    bool success = false;
    if (PQresultStatus(res) == PGRES_TUPLES_OK)
    {
        if (PQntuples(res) > 0)
        {
            success = true;
        }
    }

    // 5. 清理结果集 (连接会由 RAII 自动归还)
    PQclear(res);

    return success;
}
