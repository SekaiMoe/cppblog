#include <filesystem>
#include <string>
#include <mutex>
#include <chrono>
#include <thread>
#include <memory>
#include <regex>
#include <vector>
#include <atomic>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <unordered_set>
#include <unordered_map>

#include "blog.h"

void logError(const std::string& func, const std::string& file, int line) {
    const std::string RED = "\033[31m";
    const std::string RESET = "\033[0m";
    std::cerr << RED << "In " << func << "() in " << file << " line " << line << ":" << RESET << std::endl;
}

namespace fs = std::filesystem;

struct BlogPost {
    std::string title;
    std::string content;
    std::string html;
    std::string url;
    std::chrono::system_clock::time_point created_time;
    std::string author;
    std::string full_html;
    std::vector<std::string> tags;
};

struct BlogConfig {
    std::string blog_name;
    std::string blog_description;
    std::string blog_author;
    std::string posts_directory;
    int port;
    bool hot_reload;
    int reload_interval;
};

BlogConfig config;
std::unordered_map<std::string, BlogPost> posts_cache;
std::mutex cache_mutex;
std::atomic<bool> should_run{true};
std::unordered_map<std::string, fs::file_time_type> file_mod_times;

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
        img { max-width: 100%; }
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

const char* SEARCH_RESULT_TEMPLATE = R"(
<div class="search-results">
    <h2>搜索结果: %s</h2>
    %s
</div>
)";

const char* SEARCH_RESULT_ITEM_TEMPLATE = R"(
<div class="search-result">
    <h3><a href="%s">%s</a></h3>
    <div class="search-result-excerpt">%s</div>
</div>
)";

std::string html_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            case '"':
                r += "&quot;";
                break;
            case '\'':
                r += "&#39;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string format_time(const std::chrono::system_clock::time_point& time) {
    auto tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::localtime(&tt);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buffer);
}

std::string format_rfc822_date(const std::chrono::system_clock::time_point& time) {
    auto tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt);
    char buffer[128];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buffer);
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
}

std::string extract_title(const std::string& content) {
    std::regex title_regex(R"(^#\s+(.+)$)");
    std::smatch match;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (std::regex_search(line, match, title_regex)) {
            return match[1].str();
        }
    }
    return "Untitled";
}

std::string convert_md_to_html(const std::string& markdown) {
    cmark_gfm_core_extensions_ensure_registered();
    int options = CMARK_OPT_DEFAULT |
                  CMARK_OPT_UNSAFE |
                  CMARK_OPT_VALIDATE_UTF8;
    cmark_parser *parser = cmark_parser_new(options);
    cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("table"));
    cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("strikethrough"));
    cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("tasklist"));
    cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension("autolink"));
    cmark_parser_feed(parser, markdown.c_str(), markdown.length());
    cmark_node *doc = cmark_parser_finish(parser);
    char *html = cmark_render_html(doc, options,
                                 cmark_parser_get_syntax_extensions(parser));
    std::string result(html);
    free(html);
    cmark_node_free(doc);
    cmark_parser_free(parser);
    return result;
}

std::string string_format(const char* format, ...) {
    va_list args;
    va_start(args, format);

    int size = vsnprintf(nullptr, 0, format, args);
    va_end(args);

    if (size <= 0) {
        return std::string();
    }

    std::string result;
    result.resize(size);

    va_start(args, format);
    vsnprintf(result.data(), size + 1, format, args);
    va_end(args);

    return result;
}

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
        content << "<h2><a href='" << post.url << "'>" << post.title << "</a></h2>";
        content << "<div class='post-meta'>作者: " << post.author 
               << " | 发布时间: " << format_time(post.created_time) << "</div>";
        content << "</li>";
    }
    content << "</ul>";
    return string_format(HTML_TEMPLATE,
        config.blog_name.c_str(),
        config.blog_name.c_str(),
        config.blog_name.c_str(),
        config.blog_description.c_str(),
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
        char item[4096];
        snprintf(item, sizeof(item), RSS_ITEM_TEMPLATE,
                post.title.c_str(),
                post.html.c_str(),
                "127.0.0.1", config.port, post.url.c_str(),
                "127.0.0.1", config.port, post.url.c_str(),
                format_rfc822_date(post.created_time).c_str(),
                post.author.c_str());
        items += item;
    }

    char feed[65536];
    snprintf(feed, sizeof(feed), RSS_TEMPLATE,
             config.blog_name.c_str(),
             config.blog_description.c_str(),
             "127.0.0.1", config.port,
             format_rfc822_date(std::chrono::system_clock::now()).c_str(),
             items.c_str());

    return std::string(feed);
}

std::string strip_front_matter(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    bool in_front_matter = false;
    std::string result;

    if (std::getline(stream, line) && line == "---") {
        in_front_matter = true;
        while (std::getline(stream, line)) {
            if (line == "---") {
                in_front_matter = false;
                continue;
            }
            if (!in_front_matter) {
                result += line + "\n";
            }
        }
    } else {
        result = content;
    }

    return result;
}

void update_cache() {
    std::unordered_set<std::string> seen_files;

    for (const auto& entry : fs::recursive_directory_iterator(config.posts_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") {
            continue;
        }

        auto rel_path = fs::relative(entry.path(), config.posts_directory);
        std::string url_path = "/" + rel_path.string();
        url_path = std::regex_replace(url_path, std::regex("\\.md$"), ".html");

        seen_files.insert(url_path);

        auto current_mtime = fs::last_write_time(entry.path());

        bool needs_update = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto time_it = file_mod_times.find(url_path);
            if (time_it == file_mod_times.end()) {
                // 文件是新增的
                needs_update = true;
            } else if (current_mtime > time_it->second) {
                // 文件已被修改
                needs_update = true;
            }
        }

        if (needs_update) {
            BlogPost post;
            post.content = read_file(entry.path());

            std::istringstream stream(post.content);
            std::string line;
            bool in_front_matter = false;

            if (std::getline(stream, line) && line == "---") {
                in_front_matter = true;
                while (std::getline(stream, line) && line != "---") {
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
                            post.created_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
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

            std::string content_without_front_matter = strip_front_matter(post.content);
            post.html = convert_md_to_html(content_without_front_matter);
            post.url = url_path;

            if (post.title.empty()) {
                post.title = extract_title(content_without_front_matter);
            }
            if (post.author.empty()) {
                post.author = config.blog_author;
            }
            if (post.created_time.time_since_epoch().count() == 0) {
                post.created_time = std::chrono::system_clock::now();
            }

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

static void write_log(const char* msg) {
        std::ofstream logfile("./program_crash.log", std::ios::app);
        if (logfile.is_open()) {
            std::time_t t = std::time(nullptr);
            char timestamp[100];
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            logfile << "[" << timestamp << "] " << msg << std::endl;
            logfile.close();
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
    std::signal(SIGINT,  [](int) { should_run = false; }); // Ctrl+C
}

void load_config() {
    try {
        auto config_toml = cpptoml::parse_file("config.toml");
        config.blog_name = config_toml->get_as<std::string>("blog_name").value_or("My Blog");
        config.blog_description = config_toml->get_as<std::string>("blog_description").value_or("SekaiMoe");
        config.blog_author = config_toml->get_as<std::string>("blog_author").value_or("A simple blog");
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
        update_cache();
        std::this_thread::sleep_for(std::chrono::seconds(config.reload_interval));
    }
}

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

    update_cache();

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
        if (path.empty()) {
            return crow::response(400); // Bad Request
        }

        fs::path user_path(path);
        fs::path normalized = user_path.lexically_normal();

        if (path.find("..") != std::string::npos) {
            return crow::response(400);
        }

        if (user_path.extension() != ".html") {
            return crow::response(400);
        }

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


    // 添加搜索路由
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

        std::vector<const BlogPost*> matches;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            for (const auto& [url, post] : posts_cache) {
                if (post.title.find(query) != std::string::npos ||
                    post.content.find(query) != std::string::npos) {
                    matches.push_back(&post);
                }
            }
        }

        std::ostringstream results_html;
        if (matches.empty()) {
            results_html << "<p>没有找到与 \"" << html_escape(query) << "\" 相关的内容。</p>";
        } else {
            for (const auto* post : matches) {
                std::string excerpt = post->content.substr(0, 100);
                if (post->content.length() > 100) excerpt += "...";

                results_html << "<div class='search-result'>";
                results_html << "<h3><a href='" << html_escape(post->url) << "'>" 
                             << html_escape(post->title) << "</a></h3>";
                results_html << "<div class='search-result-excerpt'>" 
                             << html_escape(excerpt) << "</div>";
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

        // 5. 设置响应并结束
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
