#include "config.h"
#include <fstream>
#include <iostream>
#include <algorithm>

Config::Config() {
    // 设置默认值（防止配置文件没写）
    PORT = 8080;
    THREAD_NUM = 8;
    DB_PORT = 5432;
    DB_HOST = "127.0.0.1";

    // 默认值，防止空指针崩溃
    DOC_ROOT = "/home/jasper/HighPerfServer/resources"; 
}

void Config::trim(std::string& str) {
    if (str.empty()) return;
    // 去除头部空格
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    // 去除尾部空格
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
}

void Config::parse_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法打开配置文件: " << filename << "，将使用默认值。" << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // 1. 去除两端空格
        trim(line);

        // 2. 忽略空行和注释 (# 开头)
        if (line.empty() || line[0] == '#') continue;

        // 3. 查找 '='
        size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) continue;

        // 4. 拆分 key 和 value
        std::string key = line.substr(0, equal_pos);
        std::string value = line.substr(equal_pos + 1);

        trim(key);
        trim(value);

        m_config_data[key] = value;
    }

    // 5. 将读取到的值赋给成员变量
    if (m_config_data.count("PORT")) PORT = std::stoi(m_config_data["PORT"]);
    if (m_config_data.count("THREAD_NUM")) THREAD_NUM = std::stoi(m_config_data["THREAD_NUM"]);
    if (m_config_data.count("DB_PORT")) DB_PORT = std::stoi(m_config_data["DB_PORT"]);
    
    if (m_config_data.count("DOC_ROOT")) DOC_ROOT = m_config_data["DOC_ROOT"];
    if (m_config_data.count("DB_HOST")) DB_HOST = m_config_data["DB_HOST"];
    if (m_config_data.count("DB_USER")) DB_USER = m_config_data["DB_USER"];
    if (m_config_data.count("DB_PASS")) DB_PASS = m_config_data["DB_PASS"];
    if (m_config_data.count("DB_NAME")) DB_NAME = m_config_data["DB_NAME"];
}

int Config::get_int(const std::string& key) {
    if (m_config_data.count(key)) return std::stoi(m_config_data[key]);
    return 0;
}

std::string Config::get_string(const std::string& key) {
    if (m_config_data.count(key)) return m_config_data[key];
    return "";
}