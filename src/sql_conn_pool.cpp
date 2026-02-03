#include "sql_conn_pool.h"

connection_pool::connection_pool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 初始化：一次性创建 MaxConn 个连接
void connection_pool::init(std::string url, std::string User, std::string PassWord, std::string DBName, int Port, int MaxConn) {
    m_url = url;
    m_Port = std::to_string(Port);
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_MaxConn = MaxConn;

    for (int i = 0; i < MaxConn; i++) {
        // 拼接 PG 连接字符串
        std::string conninfo = "host=" + m_url + " port=" + m_Port + 
                               " dbname=" + m_DatabaseName + 
                               " user=" + m_User + 
                               " password=" + m_PassWord;
        
        PGconn *con = PQconnectdb(conninfo.c_str());

        if (PQstatus(con) != CONNECTION_OK) {
            std::cout << "Error: " << PQerrorMessage(con) << std::endl;
            exit(1);
        }
        
        connList.push_back(con);
        ++m_FreeConn;
    }

    // 初始化信号量，初始值为 MaxConn (表示一开始有这么多资源可用)
    // 注意：这里的 sem 是我们在 locker.h 里封装的，需要确保它支持带参数的 init
    // 如果你的 sem 类只支持 init(0, 0)，你需要去 locker.h 修改一下 sem 的构造函数，或者在这里循环 post
    // 为了简单，我们假设 sem 初始是 0，我们手动 post MaxConn 次
    for(int i=0; i<MaxConn; i++) {
        reserve.post();
    }
    
    m_MaxConn = m_FreeConn;
}

// 从池子里拿一个
PGconn *connection_pool::GetConnection() {
    PGconn *con = NULL;
    if (connList.size() == 0) return NULL;

    // P操作：减信号量，如果为0则阻塞等待
    reserve.wait();

    m_lock.lock();

    con = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    m_lock.unlock();
    return con;
}

// 归还连接
bool connection_pool::ReleaseConnection(PGconn *con) {
    if (NULL == con) return false;

    m_lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    m_lock.unlock();

    // V操作：加信号量，唤醒等待的线程
    reserve.post();
    return true;
}

// 销毁池子
void connection_pool::DestroyPool() {
    m_lock.lock();
    if (connList.size() > 0) {
        std::list<PGconn *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            PGconn *con = *it;
            PQfinish(con); // 关闭连接
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    m_lock.unlock();
}

connection_pool::~connection_pool() {
    DestroyPool();
}

// ================= RAII 实现 =================

connectionRAII::connectionRAII(PGconn **SQL, connection_pool *connPool) {
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}