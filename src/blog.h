#pragma once

#include "../include/cmark/src/cmark-gfm.h"
#include "../include/crow/include/crow.h"
#include "../include/cpptoml/include/cpptoml.h"

#ifdef __linux__

#include <csignal>
#include <fcntl.h> // For open(), O_RDONLY, O_CLOEXEC
#include <unistd.h> // For close() function
#ifdef __GLIBC__
#include <execinfo.h> // For backtrace
#else
#define backtrace(array, size) 0 // for musl
#define backtrace_symbols(array, size) nullptr
#endif
#include <ctime> // For timestamp

#define LOG_ERROR() logError(__func__, __FILE__, __LINE__)

