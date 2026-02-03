#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <unordered_map>

class Config {
public:
    Config();
    ~Config(){};

    // 解析配置文件
    void parse_file(const std::string& filename);

    // 获取配置项
    int get_int(const std::string& key);
    std::string get_string(const std::string& key);

private:
    // 去除字符串前后的空格
    void trim(std::string& str);

public:
    // 监听端口
    int PORT;
    
    // 数据库配置
    std::string DB_HOST;
    int DB_PORT;
    std::string DB_USER;
    std::string DB_PASS;
    std::string DB_NAME;

    // 资源目录
    std::string DOC_ROOT;

    // 线程池数量
    int THREAD_NUM;

private:
    // 存储原始的 key-value 数据
    std::unordered_map<std::string, std::string> m_config_data;
};

#endif