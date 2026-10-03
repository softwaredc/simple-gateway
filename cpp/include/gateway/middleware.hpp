// ============================================================================
// middleware.hpp —— HTTP 中间件接口与 Chain 组合器
// ----------------------------------------------------------------------------
// 什么是中间件？
//   一个函数：拿到"下一个 handler"，返回"包装过的新 handler"。
//   这就是 Go net/http 社区说的"洋葱模型"。
//
// 为什么用 std::function 而不是虚基类？
//   Go 是函数式 + interface{}，天然适合链式。
//   C++ 里 std::function<Resp(Req)> 就是类似的可调用对象封装。
//   我们的 Handler 类型：
//     using Handler = std::function<HttpResponse(const HttpRequest&)>;
//   （注意：Handler 是"同步"的——输入请求，返回响应。
//    这和 Go 的 http.Handler 签名略有不同，但教学代码够用。
//    真正异步的 Handler 要涉及协程或回调，复杂度陡增。）
//
// 中间件类型：
//     using Middleware = std::function<Handler(Handler)>;
//   输入：next handler；输出：包装好的 handler。
//
// Chain 顺序：Chain(h, mw1, mw2, mw3) →
//   请求先过 mw1，再过 mw2，再过 mw3，最后到 h。
//   洋葱模型从外到内：mw1 → mw2 → mw3 → h。
//
// 本模块实现 4 个中间件：
//   CORS         —— 加跨域头 + 直接处理预检 OPTIONS
//   RequestLog   —— 打印 method/path/status/latency
//   RateLimit    —— 按 IP 时间窗口限流（0 = 不限）
//   AuthHeader   —— 检查某个请求头是否存在
// ============================================================================

#pragma once

#include <chrono>       // std::chrono 用来测耗时
#include <functional>   // std::function
#include <initializer_list>
#include <map>          // RateLimit 的 IP → 桶
#include <memory>
#include <mutex>        // RateLimit 多线程保护
#include <string>
#include <vector>

#include "gateway/http_parser.hpp"   // HttpRequest / HttpResponse

namespace gateway {

// ---------------------------------------------------------------------------
// Handler & Middleware 类型别名
// ---------------------------------------------------------------------------
// 设计选择：Handler 是"纯函数式"的——输入请求，返回响应。
//   这意味着：handler 不直接操作 socket，只构造 HttpResponse。
//   server 层拿到 HttpResponse 后负责 serialize 写回。
//   好处：每个中间件 / handler 都是可测试的纯函数。
//   代价：不支持流式响应（但教学版不需要）。
// ---------------------------------------------------------------------------
using Handler    = std::function<HttpResponse(const HttpRequest&)>;
using Middleware = std::function<Handler(Handler)>;

// ---------------------------------------------------------------------------
// Chain —— 把 N 个中间件从外到内串起来
// ---------------------------------------------------------------------------
//   Chain(h, mw1, mw2, mw3)  等价于  mw1(mw2(mw3(h)))
//   最后传进来的 handler h 放在最内层。
// 两个重载：initializer_list 和 vector，都能用。
Handler chain(Handler h, std::initializer_list<Middleware> mws);
Handler chain(Handler h, const std::vector<Middleware>& mws);

// ---------------------------------------------------------------------------
// 各个中间件工厂函数
//   每个函数返回一个 Middleware（即一个 std::function<Handler(Handler)>）
// ---------------------------------------------------------------------------

// CORS 中间件：
//   所有域 "*"、所有方法、预检 OPTIONS 直接返回 204
Middleware cors();

// RequestLog 中间件：
//   打印 [时间戳] METHOD /path → status (耗时) [ip]
Middleware request_log();

// RateLimit 中间件：
//   qps = 0 时是 no-op；qps > 0 按 IP 每秒限流
//   算法：极简时间窗口计数（一个 IP 一个桶，1 秒内请求数 > qps 返回 429）
Middleware rate_limit(int qps);

// AuthHeader 中间件：
//   header_name 为空时是 no-op；否则检查请求头里必须有这个字段
Middleware auth_header(const std::string& header_name);

} // namespace gateway
