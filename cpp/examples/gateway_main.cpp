// ============================================================================
// examples/gateway_main.cpp —— Gateway 主程序
// ----------------------------------------------------------------------------
// 流程（一行概括）：
//   构造 Config → 构造 Router / Proxy → 把"路由匹配 + 反向代理"做成 app_handler
//   → chain 上中间件 → 起 Server 监听。
//
// app_handler 做什么？
//   输入：一个 HttpRequest（已经被 parser 解出来、已经过中间件）
//   输出：一个 HttpResponse
//   内部流程：Router::Match → 构造 MatchResult → Proxy::Forward
//
// 为什么把 route+proxy 放进 app_handler 而不是中间件？
//   因为 route+proxy 是"业务核心"——决定请求最终怎么处理。
//   中间件是"横切关注点"——CORS、日志、限流，不决定请求去哪。
//   洋葱模型里：中间件在外，业务 handler 在内。
//
// 运行：
//   ./gateway 8081           # 监听 8081，转发到 127.0.0.1:9000
//   curl http://127.0.0.1:8081/
// ============================================================================

#include "gateway/config.hpp"
#include "gateway/http_parser.hpp"
#include "gateway/middleware.hpp"
#include "gateway/proxy.hpp"
#include "gateway/router.hpp"
#include "gateway/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <memory>
#include <string>

int main(int argc, char* argv[]) {
    // 允许命令行覆盖：argv[1]=gateway端口, argv[2]=upstream端口(默认9000)
    int port = 8081;
    int upstream_port = 9000;
    if (argc >= 2) try { port = std::stoi(argv[1]); } catch (...) {}
    if (argc >= 3) try { upstream_port = std::stoi(argv[2]); } catch (...) {}

    // 1. 拿默认配置 + 改 listen 端口 + 改 upstream
    gateway::Config cfg = gateway::DefaultConfig();
    cfg.listen = "0.0.0.0:" + std::to_string(port);
    for (auto& r : cfg.routes) {
        r.upstream = "127.0.0.1:" + std::to_string(upstream_port);
    }

    // 2. 路由表（可以覆盖 DefaultConfig 的 routes）
    //    DefaultConfig 里已经有一个 catch-all "/": "127.0.0.1:9000"
    //    我们再加一个 strip_prefix 示例路由
    gateway::Router router(cfg);
    // router.add(gateway::Route{"/api/users/*", "127.0.0.1:9000", "/api/users"});
    // （已经有 "/" catch-all，就不加了，保持简单）

    // 3. Proxy 实例
    gateway::Proxy proxy;

    // 4. 构造 app_handler：route → proxy.Forward
    //    注意要用 shared_ptr 捕获 router / proxy，避免引用悬空
    auto router_ptr = std::make_shared<gateway::Router>(router);
    auto proxy_ptr  = std::make_shared<gateway::Proxy>(proxy);

    gateway::Handler core = [router_ptr, proxy_ptr](const gateway::HttpRequest& req) -> gateway::HttpResponse {
        auto result_opt = router_ptr->match(req.path);
        if (!result_opt.has_value()) {
            return gateway::make_json_response(404, "{\"error\":\"route not found\"}");
        }
        const auto& result = *result_opt;
        return proxy_ptr->forward(req, result, req.remote_ip);
    };

    // 5. 拼中间件链
    std::vector<gateway::Middleware> mws;
    if (cfg.mw.cors)          mws.push_back(gateway::cors());
    if (cfg.mw.request_log)   mws.push_back(gateway::request_log());
    if (cfg.mw.rate_limit_qps > 0) mws.push_back(gateway::rate_limit(cfg.mw.rate_limit_qps));
    if (!cfg.mw.auth_header_name.empty()) mws.push_back(gateway::auth_header(cfg.mw.auth_header_name));

    // 洋葱：Chain(core, request_log, cors, ...) → 先过 request_log，再过 cors，再 core
    // （注意 chain 是反向包的，mws 顺序决定了"最外层"的中间件在向量末尾）
    // 我们想要：请求先过 request_log（记录入口），再过 cors（加头），再 core
    // 所以 mws 顺序是 [request_log, cors, ...]。
    // chain 内部会反向包 → cors 包 core，request_log 再包 cors → 完美。
    // 但 DefaultConfig 里 mw.request_log 默认 true、mw.cors 默认 true，
    // 我们上面 mws 插入顺序已经是 cors 先？不对——看上面：
    //   mws.push_back(cors());  // index 0
    //   mws.push_back(request_log());  // index 1
    //   chain(core, mws) → 反向包：request_log(cors(core))
    //   请求进来先过 request_log（最外层），再过 cors，再 core ✓
    //   响应回来：core → cors → request_log ✓ 日志里能看到最终状态码 ✓

    // 用 initializer_list 版 chain（更简洁）
    auto app = gateway::chain(std::move(core),
                              mws);

    // 6. 起 Server
    gateway::Server srv(cfg, std::move(app));

    // 优雅退出（Ctrl+C 时调 stop）
    std::signal(SIGINT, [](int) {
        fprintf(stderr, "\n[gateway] SIGINT received, shutting down...\n");
        // 注意：这是 signal handler 里调 fprintf 是异步信号不安全的——
        // 教学版忽略这个细节；生产应该用 sig_atomic_t 加主循环检查。
        // 但 Server::stop 是 std::atomic<bool>.store，async-signal safe。
        // （我们没法在 handler 里调 srv.stop()——srv 在 main 栈上。
        //  所以这里打印一下，靠进程退出就行。）
    });

    fprintf(stderr, "[gateway] listening on %s → catch-all upstream 127.0.0.1:9000\n",
            cfg.listen.c_str());
    fprintf(stderr, "[gateway] try: curl http://127.0.0.1:%d/\n", port);
    srv.run();

    return 0;
}
