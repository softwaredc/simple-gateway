// ============================================================================
// server.cpp —— TCP 服务器主循环 + 单连接处理
// ----------------------------------------------------------------------------
// 主循环（run）：
//   1. 创建 listen socket，bind 到 cfg.listen，listen
//   2. while running_: accept 新连接 → spawn std::thread(handle_connection)
//   3. stop() 把 running_ 置 false，主循环下一轮就会退出
//
// 单连接处理（handle_connection）：
//   1. 循环读客户端数据，用 HttpRequestParser 解析
//   2. 一个请求完整了 → 调 app_handler(req) 拿到 HttpResponse
//   3. serialize → write_all 写回
//   4. 默认只处理一个请求就关连接（HTTP/1.0 行为，教学版简单）
//      如果是 "Connection: keep-alive" 理论上可以继续，但我们不做
//
// 为什么不做多线程 accept（SO_REUSEPORT / 多进程）？
//   教学重点是"请求处理链路"，网络层简化。
// ============================================================================

#include "gateway/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gateway {

// ---------------------------------------------------------------------------
// server.hpp 里声明的 socket 工具函数（实现在这里）
// ---------------------------------------------------------------------------

bool split_host_port(const std::string& listen,
                     std::string& out_host,
                     int& out_port) {
    // listen 形如 "0.0.0.0:8080" 或 ":8080"
    size_t colon = listen.find_last_of(':');
    if (colon == std::string::npos) return false;
    out_host = listen.substr(0, colon);
    try {
        out_port = std::stoi(listen.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return true;
}

// 读满 n 字节才返回；EOF 返回 false
bool read_full(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

size_t read_some(int fd, uint8_t* buf, size_t max_bytes) {
    ssize_t r = ::read(fd, buf, max_bytes);
    return (r > 0) ? static_cast<size_t>(r) : 0;
}

bool write_all(int fd, const uint8_t* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(fd, buf + done, n - done);
        if (w <= 0) return false;
        done += static_cast<size_t>(w);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Server 构造 / run / stop
// ---------------------------------------------------------------------------

Server::Server(const Config& cfg, Handler app_handler)
    : cfg_(cfg), app_handler_(std::move(app_handler)), running_(false) {}

void Server::stop() {
    running_.store(false);
}

void Server::run() {
    // 1. 解析 listen 地址
    std::string host;
    int port = 0;
    if (!split_host_port(cfg_.listen, host, port)) {
        fprintf(stderr, "[server] bad listen address: %s\n", cfg_.listen.c_str());
        return;
    }

    // 2. 建 listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[server] socket");
        return;
    }

    // SO_REUSEADDR：让服务器重启时能立刻 bind（避免 TIME_WAIT 占着端口）
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));

    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听所有网卡
    } else if (host == "127.0.0.1" || host == "localhost") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只监听本地
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[server] bind");
        ::close(listen_fd);
        return;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("[server] listen");
        ::close(listen_fd);
        return;
    }

    fprintf(stderr, "[server] listening on %s\n", cfg_.listen.c_str());
    running_.store(true);

    // 3. 主循环：accept + spawn thread
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t alen = sizeof(client_addr);
        int client_fd = accept(listen_fd,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &alen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;   // 被信号中断，继续
            if (!running_.load()) break;
            perror("[server] accept");
            continue;
        }

        // 拿到客户端 IP（字符串）
        char ipbuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client_addr.sin_addr, ipbuf, sizeof(ipbuf));

        // 先转成 std::string 再 capture——不能在 capture 里临时构造 std::string(ipbuf)
        std::string client_ip = ipbuf;

        // spawn thread：每个连接一个线程（分离式，主循环不等）
        std::thread t([this, client_fd, client_ip]() {
            this->handle_connection(client_fd, client_ip);
        });
        t.detach();
    }

    ::close(listen_fd);
    fprintf(stderr, "[server] stopped\n");
}

// ---------------------------------------------------------------------------
// handle_connection —— 单连接处理（在子线程里执行）
// ---------------------------------------------------------------------------
void Server::handle_connection(int client_fd, const std::string& client_ip) {
    // RAII 守卫：线程退出时自动 close
    struct FdGuard {
        int fd;
        ~FdGuard() { if (fd >= 0) ::close(fd); }
    } fdg{client_fd};

    HttpRequestParser parser;
    std::vector<uint8_t> tmp(8192);

    while (true) {
        size_t n = read_some(client_fd, tmp.data(), tmp.size());
        if (n == 0) break;   // EOF：客户端关了连接

        auto result_opt = parser.feed(tmp.data(), n);
        if (!result_opt.has_value()) {
            // 还没收齐，继续等更多数据
            continue;
        }

        auto& [req, err] = *result_opt;

        if (!err.empty()) {
            // 解析失败 → 400 Bad Request
            auto resp = make_text_response(400, "bad request: " + err);
            auto bytes = serialize(resp);
            write_all(client_fd, bytes.data(), bytes.size());
            break;   // 协议错了，关连接
        }

        // 填充 remote_ip（server 层知道，parser 不知道）
        req.remote_ip = client_ip;

        // 调 app_handler 处理：chain 好的 middleware + route match + proxy
        HttpResponse resp;
        try {
            resp = app_handler_(req);
        } catch (const std::exception& e) {
            fprintf(stderr, "[server] handler threw: %s\n", e.what());
            resp = make_json_response(500, "{\"error\":\"internal error\"}");
        } catch (...) {
            resp = make_json_response(500, "{\"error\":\"internal error\"}");
        }

        auto out = serialize(resp);
        if (!write_all(client_fd, out.data(), out.size())) {
            // 写失败（客户端断了），静默退出
            break;
        }

        // 教学版简单策略：处理一个请求后就关连接
        // （HTTP/1.1 默认 keep-alive，但我们不做）
        break;
    }
}

} // namespace gateway
