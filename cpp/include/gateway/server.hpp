// ============================================================================
// server.hpp —— TCP 服务器（accept + 每连接 spawn 一个 std::thread）
// ----------------------------------------------------------------------------
// 设计选择：线程模型（而不是 epoll/IOCP）
//   - 每个连接一个 std::thread，阻塞 socket read/write
//   - 教学代码直观，"一条连接一个线程"人人能懂
//   - 生产级网关不会这么做（C10K 问题），但我们要教的是
//     "网关 = accept + parse + middleware + route + proxy" 的流程，
//     而不是高性能网络编程。
//
// 流程（主循环）：
//   while true:
//       client_fd = accept(listen_fd)
//       spawn thread:  handle_one_connection(client_fd)
//
// 单连接处理流程：
//   HttpRequestParser parser;
//   while true:
//       从 client_fd 读数据 → parser.feed()
//       如果 parser 说请求完整了 →
//           Router::Match() → 找不到返回 404
//           Proxy::Forward() → 拿到 HttpResponse
//           serialize → 写回 client_fd
//           如果是 HTTP/1.0 或 Connection: close → 退出线程
// ============================================================================

#pragma once

#include <atomic>      // 优雅退出时用 std::atomic<bool> 控制循环
#include <functional>  // std::function
#include <string>

#include "gateway/config.hpp"       // Config
#include "gateway/http_parser.hpp"  // HttpRequest / HttpResponse
#include "gateway/middleware.hpp"    // Handler
#include "gateway/router.hpp"       // Router
#include "gateway/proxy.hpp"        // Proxy

namespace gateway {

// ---------------------------------------------------------------------------
// Server —— 持有 listen socket，提供 run() 和 stop()
// ---------------------------------------------------------------------------
class Server {
public:
    // 构造：接收配置 + 已组装好的"应用 handler"
    //   app_handler 是 chain(middlewares...) 后的最终 Handler，
    //   它负责"拿匹配到的路由 → proxy.Forward → 返回 HttpResponse"。
    //   我们把 router 和 proxy 放这里让 Server 自己创建也行，
    //   但把 Handler 注入更灵活（测试时可以传 mock）。
    Server(const Config& cfg, Handler app_handler);

    // 启动并阻塞直到收到 stop() 信号
    void run();

    // 从另一个线程调用，让 run() 退出
    void stop();

private:
    Config         cfg_;
    Handler        app_handler_;
    std::atomic<bool> running_;

    // 单连接处理：在子线程中执行
    void handle_connection(int client_fd, const std::string& client_ip);
};

// ---------------------------------------------------------------------------
// 一个小工具：把 "0.0.0.0:8080" 拆成 host + port
// ---------------------------------------------------------------------------
bool split_host_port(const std::string& listen,
                     std::string& out_host,
                     int& out_port);

// 从已连接的 socket fd 读指定数量的字节（循环 read 直到够或出错）
//   返回：读满了返回 true，EOF 返回 false，错误返回 false
bool read_full(int fd, uint8_t* buf, size_t n);

// 把 socket 里当前能读到的所有字节都读出来（不阻塞，或阻塞到有数据）
//   简单版：阻塞读一次，最多 max_bytes
size_t read_some(int fd, uint8_t* buf, size_t max_bytes);

// 把 buffer 里所有字节写到 socket（循环 write 直到全部写完或出错）
bool write_all(int fd, const uint8_t* buf, size_t n);

} // namespace gateway
