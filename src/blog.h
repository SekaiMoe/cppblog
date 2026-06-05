#ifndef BLOG_H
#define BLOG_H

// [FIX] 使用系统包含路径，依赖构建系统配置 -I 参数
#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#include <crow.h>
#include <cpptoml.h>

// [FIX] 只在真正需要的地方包含标准库头文件
// <cstdlib> <cstdarg> 移到 blog.cpp，除非这里真的用到

// [FIX] 安全的宏定义，用 do-while(0) 包裹
#define LOG_ERROR() do { logError(__func__, __FILE__, __LINE__); } while(0)

// [FIX] 跨平台 backtrace 安全包装
#ifdef __linux__
#include <execinfo.h>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>

inline int safe_backtrace(void** buffer, int size) {
    return backtrace(buffer, size);
}

inline char** safe_backtrace_symbols(void** buffer, int size) {
    return backtrace_symbols(buffer, size);  // 调用方需检查返回值
}

#else
// 非 Linux 平台的空实现，避免调用方崩溃
inline int safe_backtrace(void**, int) { return 0; }
inline char** safe_backtrace_symbols(void**, int) { return nullptr; }
#endif

// 函数声明（确保和 blog.cpp 一致）
void logError(const std::string& func, const std::string& file, int line);

#endif // BLOG_H
