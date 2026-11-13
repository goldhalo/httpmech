// =============================================================================
//  httpmech 示例：JSON 配置驱动的静态 WEB 服务器
// =============================================================================
//  编译：cmake --build build --target static_json_server
//  运行：./build/bin/static_json_server <config.json>
//        ./build/bin/static_json_server example/static_server.json
//
//  配置拆成两段：`server` 管监听与连接，`static` 管静态文件行为。
//  缺字段一律用代码里的默认值；类型写错会打印 config error 并退出。
//  端口 0 表示由系统分配，起来后打印实际端口。
// =============================================================================
#include <httpmech/httpmech.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using nlohmann::json;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return s;
}

/// 取一个必须是 object 的段；不存在时返回空 object。
json section(const json& root, const char* name)
{
    if (!root.contains(name)) { return json::object(); }
    const auto& s = root.at(name);
    if (!s.is_object()) {
        throw std::runtime_error(std::string{name} + " must be a JSON object");
    }
    return s;
}

/// 强类型读取：字段类型不符直接抛，由 main 统一转成一条 config error。
template <typename T>
T get_or(const json& j, const char* key, T def)
{
    if (!j.contains(key)) { return def; }
    return j.at(key).get<T>();
}

void fill_string_map(const json& j, const char* key,
                     const std::function<void(std::string, std::string)>& put)
{
    if (!j.contains(key)) { return; }
    const auto& obj = j.at(key);
    if (!obj.is_object()) {
        throw std::runtime_error(std::string{key} + " must be a JSON object");
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        put(it.key(), it.value().get<std::string>());
    }
}

int run(const char* config_path)
{
    std::ifstream in{config_path};
    if (!in) {
        throw std::runtime_error(std::string{"cannot open config: "} + config_path);
    }
    json cfg;
    in >> cfg;   // JSON 语法错误抛 json::parse_error
    if (!cfg.is_object()) {
        throw std::runtime_error("config root must be a JSON object");
    }

    const json server = section(cfg, "server");
    const json stat   = section(cfg, "static");

    // --- 静态文件配置 ---
    httpmech::StaticFileConfig sf;
    sf.root                  = get_or<std::string>(stat, "root", "./www");
    sf.mount_point           = get_or<std::string>(stat, "mount_point", "/");
    sf.index_file            = get_or<std::string>(stat, "index_file", "index.html");
    sf.default_content_type  = get_or<std::string>(stat, "default_content_type",
                                                   "application/octet-stream");
    sf.max_file_bytes        = get_or<std::size_t>(stat, "max_file_bytes", 0);
    sf.enable_range          = get_or<bool>(stat, "enable_range", true);
    sf.enable_etag           = get_or<bool>(stat, "enable_etag", true);
    sf.redirect_directory    = get_or<bool>(stat, "redirect_directory", true);

    fill_string_map(stat, "mime_types",
        [&sf](std::string k, std::string v) {
            k = lower(std::move(k));
            if (!k.empty() && k.front() == '.') { k.erase(0, 1); }
            if (!k.empty()) { sf.mime_types[std::move(k)] = std::move(v); }
        });
    fill_string_map(stat, "headers",
        [&sf](std::string k, std::string v) {
            sf.extra_headers.set(k, v);
        });

    // --- 监听配置 ---
    httpmech::ServerConfig sc;
    const auto host = get_or<std::string>(server, "host", "0.0.0.0");
    const auto port = get_or<std::uint16_t>(server, "port", 8080);
    sc.local                 = httpmech::Endpoint{host, port};
    sc.server_name           = get_or<std::string>(server, "server_name", "httpmech-static");
    sc.emit_date             = get_or<bool>(server, "emit_date", true);
    sc.max_send_queue_bytes  = get_or<std::size_t>(server, "max_send_queue_bytes",
                                                   2 * 1024 * 1024);

    auto files = httpmech::mount_static(nullptr, std::move(sf));
    if (!files->valid()) {
        throw std::runtime_error("static root invalid: " + files->error_message());
    }

    auto srv = httpmech::HttpServer::create(sc, files);
    if (!srv) {
        throw std::runtime_error("HttpServer::create failed");
    }
    if (auto ec = srv->start()) {
        throw std::runtime_error("listen failed: " + ec.message());
    }

    const auto actual = srv->local();
    std::printf("config    : %s\n", config_path);
    std::printf("listening : %s:%u\n", actual.host().c_str(),
                static_cast<unsigned>(actual.port()));
    std::printf("root      : %s\n", files->resolved_root().c_str());
    std::printf("mount     : %s\n", files->config().mount_point.c_str());
    std::printf("max_file  : %zu B (0 = unlimited)\n",
                files->config().max_file_bytes);
    std::printf("Ctrl-C to stop\n");
    std::fflush(stdout);

    while (srv->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    srv->stop();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string{argv[1]} == "--help") {
        std::printf("Usage: %s <config.json>\n\n"
                    "Example config (example/static_server.json):\n"
                    "  {\"server\":{\"host\":\"0.0.0.0\",\"port\":8080},\n"
                    "   \"static\":{\"root\":\"./www\",\"mount_point\":\"/\"}}\n",
                    argv[0]);
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <config.json>   (see --help)\n", argv[0]);
        return 1;
    }
    try {
        return run(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }
}
