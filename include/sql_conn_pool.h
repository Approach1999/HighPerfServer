#ifndef SQL_CONN_POOL_H
#define SQL_CONN_POOL_H

#include <stdio.h>
#include <list>
#include <libpq-fe.h> // PostgreSQL 头文件
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h"

class connection_pool {
public:
    // 获取单例实例
    static connection_pool *GetInstance();

    // 初始化池子
    void init(std::string url, std::string User, std::string PassWord, std::string DBName, int Port, int MaxConn);

    // 获取一个连接
    PGconn *GetConnection();

    // 归还一个连接
    bool ReleaseConnection(PGconn *conn);

    // 获取当前空闲连接数
    int GetFreeConn();

    // 销毁池子
    void DestroyPool();

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;  // 最大连接数
    int m_CurConn;  // 当前已创建的连接数
    int m_FreeConn; // 当前空闲的连接数

    locker m_lock;  // 互斥锁（保护 list）
    std::list<PGconn *> connList; // 连接池容器
    sem reserve;    // 信号量（表示有没有空闲连接）

public:
    std::string m_url;  // 主机地址
    std::string m_Port; // 数据库端口
    std::string m_User; // 数据库用户名
    std::string m_PassWord; // 数据库密码
    std::string m_DatabaseName; // 数据库名
};

// ==========================================
// RAII 机制封装：自动获取，自动归还
// ==========================================
class connectionRAII {
public:
    // 构造函数：借连接
    connectionRAII(PGconn **con, connection_pool *connPool);
    // 析构函数：还连接
    ~connectionRAII();

private:
    PGconn *conRAII;
    connection_pool *poolRAII;
};

#endif