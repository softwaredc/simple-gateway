// ============================================================================
// config.cpp —— DefaultConfig 工厂函数
// ----------------------------------------------------------------------------
// 为什么不做 JSON/YAML 文件加载？
//   零外部依赖 + 教学代码：在代码里直接 new 比写 JSON parser 容易。
//   生产网关确实会做文件监听、热加载，但本项目核心是"网关的请求
//   处理链路"，配置解析不是重点。
// ============================================================================

#include "gateway/config.hpp"

namespace gateway {

Config DefaultConfig() {
    Config cfg;
    cfg.listen = "0.0.0.0:8080";
    cfg.routes = {
        // 一条 catch-all 路由：所有请求都转发给 echo-server
        // 这样 curl http://127.0.0.1:8080/ 就能通
        Route{"/", "127.0.0.1:9000", ""},
        // 再加一条精确路由做例子
        Route{"/api/echo", "127.0.0.1:9000", ""},
    };
    cfg.mw.cors           = true;
    cfg.mw.request_log    = true;
    cfg.mw.rate_limit_qps = 0;   // 0 = 不限
    cfg.mw.auth_header_name.clear();
    return cfg;
}

} // namespace gateway
