#pragma once

// ==========================================
// 1. 第三方库包含 (建议通过构建系统配置 -I 路径)
// ==========================================
#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#include <crow.h>
#include <cpptoml.h>

// ==========================================
// 2. C++17 标准库
// ==========================================
#include <string>
#include <string_view>  // C++17: 用于零拷贝字符串传递
#include <filesystem>   // C++17: 文件系统操作
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>

// ==========================================
// 3. 平台特定头文件与兼容处理
// ==========================================
#ifdef __linux__
    #include <csignal>
    #include <fcntl.h>
    #include <unistd.h>
    #include <ctime>
    
    // 处理 musl libc (如 Alpine Linux) 没有 execinfo.h 的问题
    #ifdef __GLIBC__
        #include <execinfo.h>
    #else
        // musl fallback
        inline int backtrace(void**, int) { return 0; }
        inline char** backtrace_symbols(void**, int) { return nullptr; }
    #endif
#endif

// ==========================================
// 4. 日志与调试宏 (C++17 没有 source_location)
// ==========================================
void logError(const std::string& func, const std::string& file, int line);

// 使用 do-while(0) 确保宏在 if-else 语句中的安全性
#define LOG_ERROR() do { logError(__func__, __FILE__, __LINE__); } while(0)

// ==========================================
// 5. 跨平台工具函数
// ==========================================
namespace blog_utils {
    // 线程安全的 backtrace 包装
    inline int safe_backtrace(void** buffer, int size) {
    #ifdef __linux__
        return backtrace(buffer, size);
    #else
        return 0; // Windows/macOS 暂不实现或返回 0
    #endif
    }

    inline char** safe_backtrace_symbols(void** buffer, int size) {
    #ifdef __linux__
        return backtrace_symbols(buffer, size);
    #else
        return nullptr;
    #endif
    }
    
    // C++17 辅助：检查字符串前缀 (替代 C++20 的 starts_with)
    inline bool starts_with(std::string_view str, std::string_view prefix) {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }

    // C++17 辅助：检查字符串后缀 (替代 C++20 的 ends_with)
    inline bool ends_with(std::string_view str, std::string_view suffix) {
        return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
}
