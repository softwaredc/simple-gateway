// ============================================================================
// router.hpp —— HTTP 路径到 Route 的匹配
// ----------------------------------------------------------------------------
// 匹配优先级（和 Go 版本 / nginx location 规则一致）：
//   1. 精确匹配：path 完全相等    "/api/users" → "/api/users"
//   2. 通配前缀：模式以 "/*" 结尾 "/api/users/*" → path 以 "/api/users/" 开头
//   3. 兜底     ：模式 "/*"       → 匹配所有
//
// 实现策略：线性扫描所有 Route（O(N)）。
//   教学版不做 Trie/Radix-Tree——几十条路由时线性扫描完全够用。
//   工业级网关（APISIX、Envoy）用 Trie 把路由匹配降到 O(log N)，
//   但那不是本项目要教的重点。
// ============================================================================

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "gateway/config.hpp"

namespace gateway {

// ---------------------------------------------------------------------------
// MatchResult —— 一次匹配的结果
//   route       : 命中的路由规则（含 upstream）
//   matched_path: 命中的 pattern（如 "/api/users/*"）
//   remainder   : 通配符匹配到的剩余部分，path 去掉 pattern 前缀的那一段
// ---------------------------------------------------------------------------
struct MatchResult {
    Route       route;
    std::string matched_path;
    std::string remainder;
};

// ---------------------------------------------------------------------------
// Router —— 持有路由表，提供 Match() 方法
// ---------------------------------------------------------------------------
class Router {
public:
    Router() = default;

    // 直接从 Config 的 routes 构造
    explicit Router(const Config& cfg);

    // 手动添加一条路由（方便程序式配置）
    void add(const Route& r);

    // 把请求 path 匹配到路由
    //   path 必须是 URL 的 path 部分（已经去掉了 query string）
    std::optional<MatchResult> match(const std::string& path) const;

private:
    std::vector<Route> routes_;
};

// ---------------------------------------------------------------------------
// StripPrefix —— 根据 Route 配置改写转发路径
//   path = "/api/users/123", strip_prefix = "/api/users" → "/123"
//   没配置 strip_prefix 时原样返回。
//   为什么独立函数？Go 版本也是独立的——Router 和路径改写是两个关注点。
// ---------------------------------------------------------------------------
std::string strip_prefix(const std::string& path, const Route& route);

} // namespace gateway
