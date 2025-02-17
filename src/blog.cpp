#include "blog.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <memory>
#include <regex>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <ctime>

namespace fs = std::filesystem;

struct BlogPost {
    std::string title;
    std::string content;
    std::string html;
    std::string url;
    std::chrono::system_clock::time_point created_time;
    std::string author;
};

struct BlogConfig {
    std::string blog_name;
    std::string blog_description;
    std::string posts_directory;
    int port;
    bool hot_reload;
    int reload_interval;
};

BlogConfig config;
std::unordered_map<std::string, BlogPost> posts_cache;
std::mutex cache_mutex;
bool should_run = true;

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
    </header>
    <main>
        %s
    </main>
</body>
</html>
)";

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
    cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
    cmark_parser_feed(parser, markdown.c_str(), markdown.length());
    cmark_node *doc = cmark_parser_finish(parser);
    char *html = cmark_render_html(doc, CMARK_OPT_DEFAULT, nullptr);
    std::string result(html);
    free(html);
    cmark_node_free(doc);
    cmark_parser_free(parser);
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

    char page[16384];
    snprintf(page, sizeof(page), HTML_TEMPLATE,
            config.blog_name.c_str(), config.blog_name.c_str(),
            config.blog_name.c_str(), config.blog_description.c_str(),
            content.str().c_str());

    return std::string(page);
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

void update_cache() {
    std::unordered_map<std::string, BlogPost> new_cache;

    for(const auto& entry : fs::recursive_directory_iterator(config.posts_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            auto rel_path = fs::relative(entry.path(), config.posts_directory);
            std::string url_path = "/" + rel_path.string();
            url_path = std::regex_replace(url_path, std::regex("\\.md$"), ".html");

            BlogPost post;
            post.content = read_file(entry.path());
            post.title = extract_title(post.content);
            post.html = convert_md_to_html(post.content);
            post.url = url_path;
            post.created_time = std::chrono::system_clock::now();
            post.author = "SekaiMoe";

            char page[16384];
            snprintf(page, sizeof(page), HTML_TEMPLATE,
                    post.title.c_str(), config.blog_name.c_str(),
                    config.blog_name.c_str(), config.blog_description.c_str(),
                    post.html.c_str());

            new_cache[url_path] = std::move(post);
        }
    }

    std::lock_guard<std::mutex> lock(cache_mutex);
    posts_cache = std::move(new_cache);
}

void load_config() {
    try {
        auto config_toml = cpptoml::parse_file("config.toml");
        config.blog_name = config_toml->get_as<std::string>("blog_name").value_or("My Blog");
        config.blog_description = config_toml->get_as<std::string>("blog_description").value_or("A simple blog");
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
    load_config();
    update_cache();

    std::thread reload_thread;
    if (config.hot_reload) {
        reload_thread = std::thread(hot_reload_thread);
    }

    crow::SimpleApp app;

    // 添加首页路由
    CROW_ROUTE(app, "/")
    ([]() {
        return crow::response(generate_index_page());
    });

    // 添加RSS feed路由
    CROW_ROUTE(app, "/feed.xml")
    ([]() {
        crow::response res(generate_rss_feed());
        res.set_header("Content-Type", "application/xml");
        return res;
    });

    CROW_ROUTE(app, "/<path>")
    ([](const std::string& path) {
        std::string url_path = "/" + path;
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = posts_cache.find(url_path);
        if (it != posts_cache.end()) {
            char page[16384];
            snprintf(page, sizeof(page), HTML_TEMPLATE,
                    it->second.title.c_str(), config.blog_name.c_str(),
                    config.blog_name.c_str(), config.blog_description.c_str(),
                    it->second.html.c_str());
            return crow::response(page);
        }
        return crow::response(404);
    });

    app.port(config.port).run();

    should_run = false;
    if (config.hot_reload && reload_thread.joinable()) {
        reload_thread.join();
    }

    return 0;
}
