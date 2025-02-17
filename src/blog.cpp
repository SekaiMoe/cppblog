#include "blog.h"
#include <cpptoml.h>
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

namespace fs = std::filesystem;

struct BlogConfig {
    std::string blog_name;
    std::string blog_description;
    std::string posts_directory;
    int port;
    bool hot_reload;
    int reload_interval;
};

BlogConfig config;
std::unordered_map<std::string, std::string> page_cache;
std::mutex cache_mutex;
bool should_run = true;

const char* HTML_TEMPLATE = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>%s - %s</title>
    <style>
        body { max-width: 800px; margin: 0 auto; padding: 20px; line-height: 1.6; }
        pre { background: #f4f4f4; padding: 10px; overflow-x: auto; }
        img { max-width: 100%; }
    </style>
</head>
<body>
    <header>
        <h1>%s</h1>
        <p>%s</p>
    </header>
    <main>
        %s
    </main>
</body>
</html>
)";

std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
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

void update_cache() {
    std::unordered_map<std::string, std::string> new_cache;
    for(const auto& entry : fs::recursive_directory_iterator(config.posts_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            auto rel_path = fs::relative(entry.path(), config.posts_directory);
            std::string url_path = "/" + rel_path.string();
            url_path = std::regex_replace(url_path, std::regex("\\.md$"), ".html");
            std::string content = read_file(entry.path());
            std::string html_content = convert_md_to_html(content);
            char* formatted_html = new char[16384];
            snprintf(formatted_html, 16384, HTML_TEMPLATE,
                    rel_path.stem().c_str(), config.blog_name.c_str(),
                    config.blog_name.c_str(), config.blog_description.c_str(),
                    html_content.c_str());
            new_cache[url_path] = formatted_html;
            delete[] formatted_html;
        }
    }

    std::lock_guard<std::mutex> lock(cache_mutex);
    page_cache = std::move(new_cache);
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
    CROW_ROUTE(app, "/<path>")
    ([](const std::string& path) {
        std::string url_path = "/" + path;
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = page_cache.find(url_path);
        if (it != page_cache.end()) {
            return crow::response(it->second);
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
