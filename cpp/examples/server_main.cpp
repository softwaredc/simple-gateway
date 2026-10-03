// ============================================================================
// examples/server_main.cpp —— Echo Server（后端示例）
// ----------------------------------------------------------------------------
// 功能：收到任何 HTTP 请求，返回 JSON 格式的 echo（把 method / path / headers / body 原样回显）。
// 和 Go 版 cmd/echo-server 行为一致。
//
// 为什么要先有 echo-server？
//   网关的上游（upstream）需要一个能响应 HTTP 的后端——echo-server 就是最简单的后端。
//   网关接请求 → 匹配路由 → 转发给 echo-server → echo-server 返回 echo → 网关回给客户端。
//
// 运行：
//   ./echo-server 9000    # 在 127.0.0.1:9000 起一个后端
// ============================================================================

#include "gateway/config.hpp"
#include "gateway/http_parser.hpp"
#include "gateway/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

// 把 HttpRequest 序列化成 JSON 字符串（echo 效果）
static std::string request_to_json(const gateway::HttpRequest& req) {
    std::ostringstream os;
    os << "{";
    os << "\"method\":\"" << req.method << "\",";
    os << "\"path\":\"" << req.path << "\",";
    os << "\"query\":\"" << req.query << "\",";
    os << "\"remote_ip\":\"" << req.remote_ip << "\",";

    // headers
    os << "\"headers\":{";
    bool first = true;
    for (const auto& [k, v] : req.headers) {
        if (!first) os << ",";
        first = false;
        os << "\"" << k << "\":\"" << v << "\"";
    }
    os << "},";

    // body（转成字符串）
    std::string body_str(req.body.begin(), req.body.end());
    // 简单转义（处理 JSON 里的特殊字符）
    std::string body_escaped;
    for (char c : body_str) {
        switch (c) {
            case '"':  body_escaped += "\\\""; break;
            case '\\': body_escaped += "\\\\"; break;
            case '\n': body_escaped += "\\n";  break;
            case '\r': body_escaped += "\\r";  break;
            case '\t': body_escaped += "\\t";  break;
            default:   body_escaped.push_back(c);
        }
    }
    os << "\"body\":\"" << body_escaped << "\",";
    os << "\"body_len\":" << req.body.size();

    os << "}";
    return os.str();
}

int main(int argc, char* argv[]) {
    // 允许命令行指定端口，默认 9000
    int port = 9000;
    if (argc >= 2) {
        try { port = std::stoi(argv[1]); }
        catch (...) {}
    }

    // 构造一个最简单的 Server：app_handler 直接 echo
    gateway::Config cfg;
    cfg.listen = "0.0.0.0:" + std::to_string(port);

    gateway::Handler echo_handler = [](const gateway::HttpRequest& req) -> gateway::HttpResponse {
        std::string json = request_to_json(req);
        return gateway::make_json_response(200, json);
    };

    gateway::Server srv(cfg, echo_handler);

    fprintf(stderr, "[echo-server] listening on 0.0.0.0:%d\n", port);
    srv.run();

    return 0;
}
