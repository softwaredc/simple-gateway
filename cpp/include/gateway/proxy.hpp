// ============================================================================
// proxy.hpp —— 反向代理：把请求转给 upstream
// ----------------------------------------------------------------------------
// 做的事情：
//   1. 新建 TCP socket → connect 到 upstream（host:port）
//   2. 把客户端请求改写成发往 upstream 的 HTTP 请求：
//      - Host 改成 upstream 的 host
//      - 重写 path（strip_prefix + remainder）
//      - 设置 X-Forwarded-* 头（让后端知道原始信息）
//   3. 把改写后的请求 serialize 并写入 upstream socket
//   4. 循环从 upstream 读数据，拼出完整的 HttpResponse
//   5. 返回 HttpResponse，server 层负责写回客户端
//
// 为什么不做"流式管道"（一边读 upstream 一边写 client）？
//   教学版简化：read 全 upstream body → 再 write client。
//   对于小 body（教学场景）完全够用。
//   生产级网关确实会做双向流式，避免大响应占内存。
//
// 错误处理：
//   - connect 失败 → 返回 502 Bad Gateway
//   - upstream 响应超时 / 格式错误 → 返回 502
//   - client 断开（我们在写客户端时发现写失败）→ 直接丢弃，不报错
// ============================================================================

#pragma once

#include <string>

#include "gateway/http_parser.hpp"   // HttpRequest / HttpResponse
#include "gateway/router.hpp"       // MatchResult

namespace gateway {

// ---------------------------------------------------------------------------
// Proxy —— 反向代理
// ---------------------------------------------------------------------------
class Proxy {
public:
    Proxy() = default;

    // 把一个匹配好的请求转发到 upstream
    //   req      : 原始请求（从客户端来的）
    //   result   : Router::Match 的结果（含 route / upstream / remainder）
    //   client_ip: 客户端 IP（用于填 X-Forwarded-For）
    //
    // 返回：upstream 的响应（或 502）
    HttpResponse forward(const HttpRequest& req,
                         const MatchResult& result,
                         const std::string& client_ip);
};

} // namespace gateway
