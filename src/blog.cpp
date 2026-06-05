#include "blog.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdarg>
#include <ctime>
#include <csignal>
#include <string_view>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <thread>

#ifdef __linux__
#include <unistd.h> // For write(), _exit()
#endif

// ==========================================
// 1. 全局变量与结构体
// ==========================================

namespace fs = std::filesystem;

struct BlogPost {
    std::string title;
    std::string content;
    std::string html;
    std::string url;
    std::chrono::system_clock::time_point created_time;
    std::string author;
    std::vector<std::string> tags;
};

struct BlogConfig {
    std::string blog_name;
    std::string blog_description;
    std::string blog_author;
    std::string blog_domain; 
    std::string posts_directory;
    int port;
    bool hot_reload;
    int reload_interval;
};

// 搜索结果结构体 (修复 Use-After-Free 悬挂指针问题)
struct SearchResult {
    std::string url;
    std::string title;
    std::string excerpt;
};

BlogConfig config;
std::unordered_map<std::string, BlogPost> posts_cache;
std::mutex cache_mutex;
std::atomic<bool> should_run{true};
std::unordered_map<std::string, fs::file_time_type> file_mod_times;
std::mutex log_mutex; // 日志文件写入锁

// ==========================================
// 2. 模板字符串
// ==========================================

const char* RSS_TEMPLATE = R"(<?xml version="1.0" encoding="UTF-8" ?>
<rss version="2.0">
<channel>
    <title>%s</title>
    <description>%s</description>
    <link>http://%s:%d</link>
    <lastBuildDate>%s</lastBuildDate>
    %s
</channel>
</rss>
)";

const char* RSS_ITEM_TEMPLATE = R"(
    <item>
        <title>%s</title>
        <description><![CDATA[%s]]></description>
        <link>http://%s:%d%s</link>
        <guid>http://%s:%d%s</guid>
        <pubDate>%s</pubDate>
        <author>%s</author>
    </item>
)";

const char* HTML_TEMPLATE = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>%s - %s</title>
    <link rel="alternate" type="application/rss+xml" title="RSS Feed" href="/feed.xml" />
    <style>
        body { max-width: 800px; margin: 0 auto; padding: 20px; line-height: 1.6; }
        pre { background: #f4f4f4; padding: 10px; overflow-x: auto; }
        img { max-width: 100%%; }
        .search-form { margin-bottom: 20px; }
        .search-input { width: 70%%; padding: 8px; }
        .search-button { padding: 8px 16px; }
        .search-results { margin-top: 20px; }
        .search-result { margin-bottom: 20px; padding: 10px; border: 1px solid #ddd; }
        .search-result h3 { margin-top: 0; }
        .search-result-excerpt { color: #666; }
        .post-list { list-style: none; padding: 0; }
        .post-item { margin-bottom: 20px; padding-bottom: 20px; border-bottom: 1px solid #eee; }
        .post-meta { color: #666; font-size: 0.9em; }
        .rss-link { float: right; }
    </style>
</head>
<body>
    <header>
        <h1><a href="/" style="text-decoration: none; color: inherit;">%s</a></h1>
        <p>%s</p>
        <div class="rss-link">
            <a href="/feed.xml">RSS订阅</a>
        </div>
        <form class="search-form" action="/search" method="get">
            <input type="text" name="q" class="search-input" placeholder="搜索博客...">
            <button type="submit" class="search-button">搜索</button>
        </form>
    </header>
    <main>
        %s
    </main>
</body>
</html>
)";

// ==========================================
// 3. 工具函数 (C++17 优化)
// ==========================================

void logError(const std::string& func, const std::string& file, int line) {
    const std::string RED = "\033[31m";
    const std::string RESET = "\033[0m";
    std::cerr << RED << "In " << func << "() in " << file << " line " << line << ":" << RESET << std::endl;
}

// 辅助函数：去除 Windows 换行符 \r
void trim_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

// [C++17 优化] 使用 std::string_view 避免临时 std::string 构造开销
std::string html_escape(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += c;
        }
    }
    return r;
}

// 线程安全的时间转换封装
std::tm safe_localtime(std::time_t t) {
    std::tm tm = {};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::tm safe_gmtime(std::time_t t) {
    std::tm tm = {};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

std::string format_time(const std::chrono::system_clock::time_point& time) {
    auto tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = safe_localtime(tt);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buffer);
}

std::string format_rfc822_date(const std::chrono::system_clock::time_point& time) {
    auto tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = safe_gmtime(tt);
    char buffer[128];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buffer);
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string extract_title(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        trim_cr(line);
        // [C++17] 使用 blog.h 中封装的 starts_with 替代低效的 std::regex
        if (blog_utils::starts_with(line, "# ")) {
            return line.substr(2);
        }
    }
    return "Untitled";
}

std::string convert_md_to_html(const std::string& markdown) {
    cmark_gfm_core_extensions_ensure_registered();
    int options = CMARK_OPT_DEFAULT | CMARK_OPT_UNSAFE | CMARK_OPT_VALIDATE_UTF8;
    cmark_parser *parser = cmark_parser_new(options);
    
    auto attach_ext = [&](const char* name) {
        cmark_syntax_extension* ext = cmark_find_syntax_extension(name);
        if (ext) cmark_parser_attach_syntax_extension(parser, ext);
    };
    attach_ext("table");
    attach_ext("strikethrough");
    attach_ext("tasklist");
    attach_ext("autolink");

    cmark_parser_feed(parser, markdown.c_str(), markdown.length());
    cmark_node *doc = cmark_parser_finish(parser);
    char *html = cmark_render_html(doc, options, cmark_parser_get_syntax_extensions(parser));
    std::string result(html);
    free(html);
    cmark_node_free(doc);
    cmark_parser_free(parser);
    return result;
}

// 修复 va_list 重复使用导致的未定义行为 (UB)
std::string string_format(const char* format, ...) {
    va_list args, args_copy;
    va_start(args, format);
    va_copy(args_copy, args); 

    int size = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (size <= 0) {
        va_end(args);
        return std::string();
    }

    std::string result(size, '\0');
    vsnprintf(result.data(), size + 1, format, args); 
    va_end(args);

    return result;
}

std::string strip_front_matter(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string result;

    if (std::getline(stream, line)) {
        trim_cr(line); 
        if (line == "---") {
            bool found_end = false;
            while (std::getline(stream, line)) {
                trim_cr(line);
                if (line == "---") {
                    found_end = true;
                    break;
                }
            }
            if (found_end) {
                std::string rest((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
                result = rest;
            } else {
                result = content;
            }
        } else {
            result = content;
        }
    }
    return result;
}

// ==========================================
// 4. 核心业务逻辑
// ==========================================

std::string generate_index_page() {
    std::vector<BlogPost> sorted_posts;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto& [_, post] : posts_cache) {
            sorted_posts.push_back(post);
        }
    }

    std::sort(sorted_posts.begin(), sorted_posts.end(),
              [](const BlogPost& a, const BlogPost& b) {
                  return a.created_time > b.created_time;
              });

    std::stringstream content;
    content << "<ul class='post-list'>";
    for (const auto& post : sorted_posts) {
        content << "<li class='post-item'>";
        // [修复] XSS 漏洞，对 URL 和 Title 进行 HTML 转义
        content << "<h2><a href='" << html_escape(post.url) << "'>" 
                << html_escape(post.title) << "</a></h2>";
        content << "<div class='post-meta'>作者: " << html_escape(post.author) 
               << " | 发布时间: " << format_time(post.created_time) << "</div>";
        content << "</li>";
    }
    content << "</ul>";
    
    return string_format(HTML_TEMPLATE,
        html_escape(config.blog_name).c_str(),
        html_escape(config.blog_name).c_str(),
        html_escape(config.blog_name).c_str(),
        html_escape(config.blog_description).c_str(),
        content.str().c_str()
    );
}

std::string generate_rss_feed() {
    std::vector<BlogPost> sorted_posts;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto& [_, post] : posts_cache) {
            sorted_posts.push_back(post);
        }
    }

    std::sort(sorted_posts.begin(), sorted_posts.end(),
              [](const BlogPost& a, const BlogPost& b) {
                  return a.created_time > b.created_time;
              });

    std::string items;
    for (const auto& post : sorted_posts) {
        // [修复] 抛弃固定大小的 char 数组，防止长文章导致 RSS 截断损坏
        items += string_format(RSS_ITEM_TEMPLATE,
                html_escape(post.title).c_str(),
                post.html.c_str(),
                config.blog_domain.c_str(), config.port, post.url.c_str(),
                config.blog_domain.c_str(), config.port, post.url.c_str(),
                format_rfc822_date(post.created_time).c_str(),
                html_escape(post.author).c_str());
    }

    return string_format(RSS_TEMPLATE,
             html_escape(config.blog_name).c_str(),
             html_escape(config.blog_description).c_str(),
             config.blog_domain.c_str(), config.port,
             format_rfc822_date(std::chrono::system_clock::now()).c_str(),
             items.c_str());
}

void update_cache() {
    std::unordered_set<std::string> seen_files;

    // 注意：如果 posts_directory 不存在，这里会抛出 fs::filesystem_error
    for (const auto& entry : fs::recursive_directory_iterator(config.posts_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") {
            continue;
        }

        auto rel_path = fs::relative(entry.path(), config.posts_directory);
        std::string url_path = "/" + rel_path.string();
        
        // [C++17] 使用 blog.h 中封装的 ends_with 替代低效的 std::regex
        if (blog_utils::ends_with(url_path, ".md")) {
            url_path.replace(url_path.length() - 3, 3, ".html");
        }

        seen_files.insert(url_path);
        auto current_mtime = fs::last_write_time(entry.path());

        bool needs_update = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto time_it = file_mod_times.find(url_path);
            if (time_it == file_mod_times.end() || current_mtime > time_it->second) {
                needs_update = true;
            }
        }

        if (needs_update) {
            BlogPost post;
            post.content = read_file(entry.path());

            std::istringstream stream(post.content);
            std::string line;

            if (std::getline(stream, line)) {
                trim_cr(line); 
                if (line == "---") {
                    while (std::getline(stream, line)) {
                        trim_cr(line);
                        if (line == "---") break;
                        
                        size_t pos = line.find(':');
                        if (pos != std::string::npos) {
                            std::string key = line.substr(0, pos);
                            std::string value = line.substr(pos + 1);
                            value.erase(0, value.find_first_not_of(" "));
                            value.erase(value.find_last_not_of(" ") + 1);

                            if (key == "title") {
                                post.title = value;
                            } else if (key == "date") {
                                std::tm tm = {};
                                std::istringstream ss(value);
                                ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                                if (!ss.fail()) { 
                                    post.created_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                                }
                            } else if (key == "author") {
                                post.author = value;
                            } else if (key == "tags") {
                                std::istringstream tags_stream(value);
                                std::string tag;
                                while (std::getline(tags_stream, tag, ',')) {
                                    tag.erase(0, tag.find_first_not_of(" "));
                                    tag.erase(tag.find_last_not_of(" ") + 1);
                                    post.tags.push_back(tag);
                                }
                            }
                        }
                    }
                }
            }

            std::string content_without_front_matter = strip_front_matter(post.content);
            post.html = convert_md_to_html(content_without_front_matter);
            post.url = url_path;

            if (post.title.empty()) post.title = extract_title(content_without_front_matter);
            if (post.author.empty()) post.author = config.blog_author;
            if (post.created_time.time_since_epoch().count() == 0) post.created_time = std::chrono::system_clock::now();

            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                posts_cache[url_path] = std::move(post);
                file_mod_times[url_path] = current_mtime;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto it = posts_cache.begin(); it != posts_cache.end();) {
            if (seen_files.find(it->first) == seen_files.end()) {
                file_mod_times.erase(it->first);
                it = posts_cache.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ==========================================
// 5. 系统级与线程管理
// ==========================================

static void write_log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream logfile("./program_crash.log", std::ios::app);
    if (logfile.is_open()) {
        std::time_t t = std::time(nullptr);
        char timestamp[100];
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        logfile << "[" << timestamp << "] " << msg << std::endl;
    }
}

static void sighandle(int sig) {
    const char msg[] = "Fatal error: signal received. Exiting.\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(127);
}

void register_signal() {
    std::signal(SIGSEGV, sighandle);
    std::signal(SIGABRT, sighandle);
    std::signal(SIGFPE,  sighandle);
    std::signal(SIGILL,  sighandle);

    std::signal(SIGTERM, [](int) { should_run = false; });
    std::signal(SIGINT,  [](int) { should_run = false; }); 
}

void load_config() {
    try {
        auto config_toml = cpptoml::parse_file("config.toml");
        config.blog_name = config_toml->get_as<std::string>("blog_name").value_or("My Blog");
        config.blog_description = config_toml->get_as<std::string>("blog_description").value_or("SekaiMoe");
        config.blog_author = config_toml->get_as<std::string>("blog_author").value_or("A simple blog");
        config.blog_domain = config_toml->get_as<std::string>("blog_domain").value_or("127.0.0.1"); 
        config.posts_directory = config_toml->get_as<std::string>("posts_directory").value_or("posts");
        config.port = config_toml->get_as<int>("port").value_or(5444);
        config.hot_reload = config_toml->get_as<bool>("hot_reload").value_or(true);
        config.reload_interval = config_toml->get_as<int>("reload_interval").value_or(5);
    } catch (const std::exception& e) {
        std::cerr << "配置文件加载失败: " << e.what() << std::endl;
        exit(1);
    }
}

void hot_reload_thread() {
    while (should_run) {
        // [修复] 捕获文件系统异常，防止热重载线程崩溃导致整个进程退出
        try {
            update_cache();
        } catch (const std::exception& e) {
            write_log("Hot reload error: " + std::string(e.what()));
        } catch (...) {
            write_log("Hot reload error: Unknown exception");
        }
        std::this_thread::sleep_for(std::chrono::seconds(config.reload_interval));
    }
}

// ==========================================
// 6. 主函数与路由
// ==========================================

int main() {
    #ifdef __linux__
    register_signal();
    #endif
    cmark_gfm_core_extensions_ensure_registered();
    load_config();

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        posts_cache.clear();
        file_mod_times.clear();
    }

    try {
        update_cache();
    } catch (const std::exception& e) {
        std::cerr << "Initial cache update failed: " << e.what() << std::endl;
    }

    std::thread reload_thread;
    if (config.hot_reload) {
        reload_thread = std::thread(hot_reload_thread);
    }

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([]() {
        return crow::response(generate_index_page());
    });

    CROW_ROUTE(app, "/feed.xml")
    ([]() {
        crow::response res(generate_rss_feed());
        res.set_header("Content-Type", "application/xml");
        return res;
    });

    CROW_ROUTE(app, "/<path>")
    ([](const std::string& path) {
        if (path.empty()) return crow::response(400); 

        fs::path user_path(path);
        fs::path normalized = user_path.lexically_normal();

        if (path.find("..") != std::string::npos) return crow::response(400);
        if (user_path.extension() != ".html") return crow::response(400);

        std::string url_path = "/" + path;
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = posts_cache.find(url_path);
        if (it != posts_cache.end()) {
            std::string full_html = string_format(HTML_TEMPLATE,
                html_escape(it->second.title).c_str(),
                html_escape(config.blog_name).c_str(),
                html_escape(config.blog_name).c_str(),
                html_escape(config.blog_description).c_str(),
                it->second.html.c_str()
            );
            return crow::response(full_html);
        }
        return crow::response(404);
    });

    CROW_ROUTE(app, "/search")
    ([](const crow::request& req, crow::response& res) {
        auto q_param = req.url_params.get("q");
        if (!q_param) {
            res.set_header("Location", "/");
            res.code = 302;
            res.end();
            return;
        }
        std::string query = std::string(q_param);

        // [修复] 使用值类型存储结果，避免锁释放后指针失效 (Use-After-Free)
        std::vector<SearchResult> matches;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            for (const auto& [url, post] : posts_cache) {
                if (post.title.find(query) != std::string::npos ||
                    post.content.find(query) != std::string::npos) {
                    SearchResult r;
                    r.url = post.url;
                    r.title = post.title;
                    r.excerpt = post.content.substr(0, 100);
                    if (post.content.length() > 100) r.excerpt += "...";
                    matches.push_back(std::move(r));
                }
            }
        }

        std::ostringstream results_html;
        if (matches.empty()) {
            results_html << "<p>没有找到与 \"" << html_escape(query) << "\" 相关的内容。</p>";
        } else {
            for (const auto& match : matches) {
                results_html << "<div class='search-result'>";
                results_html << "<h3><a href='" << html_escape(match.url) << "'>" 
                             << html_escape(match.title) << "</a></h3>";
                results_html << "<div class='search-result-excerpt'>" 
                             << html_escape(match.excerpt) << "</div>";
                results_html << "</div>";
            }
        }

        std::ostringstream full_page;
        full_page << "<!DOCTYPE html>\n<html>\n<head>\n"
                  << "    <meta charset=\"UTF-8\">\n"
                  << "    <title>搜索 \"" << html_escape(query) << "\" - " << html_escape(config.blog_name) << "</title>\n"
                  << "    <link rel=\"alternate\" type=\"application/rss+xml\" title=\"RSS Feed\" href=\"/feed.xml\" />\n"
                  << R"(<style>
        body { max-width: 800px; margin: 0 auto; padding: 20px; line-height: 1.6; }
        pre { background: #f4f4f4; padding: 10px; overflow-x: auto; }
        img { max-width: 100%; }
        .search-form { margin-bottom: 20px; }
        .search-input { width: 70%; padding: 8px; }
        .search-button { padding: 8px 16px; }
        .search-results { margin-top: 20px; }
        .search-result { margin-bottom: 20px; padding: 10px; border: 1px solid #ddd; }
        .search-result h3 { margin-top: 0; }
        .search-result-excerpt { color: #666; }
        .post-list { list-style: none; padding: 0; }
        .post-item { margin-bottom: 20px; padding-bottom: 20px; border-bottom: 1px solid #eee; }
        .post-meta { color: #666; font-size: 0.9em; }
        .rss-link { float: right; }
    </style>
</head>
<body>
    <header>
        <h1><a href="/" style="text-decoration: none; color: inherit;">)" 
                  << html_escape(config.blog_name) << R"(</a></h1>
        <p>)" << html_escape(config.blog_description) << R"(</p>
        <div class="rss-link">
            <a href="/feed.xml">RSS订阅</a>
        </div>
        <form class="search-form" action="/search" method="get">
            <input type="text" name="q" class="search-input" value=")" 
                  << html_escape(query) << R"(" placeholder="搜索博客...">
            <button type="submit" class="search-button">搜索</button>
        </form>
    </header>
    <main>
        <h2>搜索结果: ")" << html_escape(query) << R"("</h2>
)" << results_html.str() << R"(
    </main>
</body>
</html>)";

        res.set_header("Content-Type", "text/html; charset=utf-8");
        res.write(full_page.str());
        res.end();
    });

    app.port(config.port).run();

    should_run = false;
    if (config.hot_reload && reload_thread.joinable()) {
        reload_thread.join();
    }

    return 0;
}
