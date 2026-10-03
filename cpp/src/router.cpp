// ============================================================================
// router.cpp —— 请求路径 → Route 匹配
// ----------------------------------------------------------------------------
// 匹配优先级（和 Go 版本完全一致）：
//   1. 精确匹配 path （pattern 不以 /* 结尾，且 == path）
//   2. 通配前缀       （pattern 以 /* 结尾，path 以 pattern 前缀开头）
//   3. 没匹配到 → 返回 nullopt（server 层会返回 404）
//
// 示例：
//   路由表 = ["/api/users", "/api/users/*", "/api/*", "/"]
//   请求 "/api/users"       → 精确匹配 "/api/users"
//   请求 "/api/users/123"   → 通配 "/api/users/*"
//   请求 "/api/orders/list" → 通配 "/api/*"
//   请求 "/"                → 精确 "/"
//   请求 "/anything/else"   → 精确 "/"
//
// 实现：线性扫描 O(N)。教学够用。
// ============================================================================

#include "gateway/router.hpp"

namespace gateway {

Router::Router(const Config& cfg) : routes_(cfg.routes) {}

void Router::add(const Route& r) {
    routes_.push_back(r);
}

std::optional<MatchResult> Router::match(const std::string& path) const {
    constexpr const char* kWildcard = "/*";

    // ---- 第一轮：精确匹配（跳过所有带 /* 的 pattern）----
    for (const auto& rt : routes_) {
        const std::string& pattern = rt.path;
        if (pattern.size() >= 2 &&
            pattern.compare(pattern.size() - 2, 2, kWildcard) == 0) {
            continue;   // 通配路由，下一轮处理
        }
        if (pattern == path) {
            return MatchResult{rt, pattern, ""};
        }
    }

    // ---- 第二轮：通配前缀匹配 ----
    for (const auto& rt : routes_) {
        const std::string& pattern = rt.path;
        if (pattern.size() < 2 ||
            pattern.compare(pattern.size() - 2, 2, kWildcard) != 0) {
            continue;   // 不是通配路由
        }
        // "/api/*" → prefix = "/api/"
        std::string prefix = pattern.substr(0, pattern.size() - 1); // 去掉 '*'

        // 两种情况：
        //   path 以 prefix 开头（"/api/users/123" vs "/api/"）
        //   path 正好等于 prefix 去掉末尾 '/'（"/api/users" vs "/api/users/"）
        bool matched = false;
        std::string remainder;
        if (!prefix.empty() &&
            path.compare(0, prefix.size(), prefix) == 0) {
            matched = true;
            remainder = path.substr(prefix.size());
        } else if (!prefix.empty() && prefix.back() == '/' &&
                   path == prefix.substr(0, prefix.size() - 1)) {
            matched = true;
            remainder.clear();
        }

        if (matched) {
            return MatchResult{rt, pattern, remainder};
        }
    }

    // ---- 没匹配到 ----
    return std::nullopt;
}

std::string strip_prefix(const std::string& path, const Route& route) {
    if (route.strip_prefix.empty()) return path;
    const std::string& prefix = route.strip_prefix;

    // path 正好等于 prefix → 去掉后是空，交给上层决定补 '/'
    if (path == prefix) return "";

    // path 以 prefix + "/" 开头 → 去掉 prefix
    if (path.size() > prefix.size() &&
        path.compare(0, prefix.size(), prefix) == 0 &&
        path[prefix.size()] == '/') {
        return path.substr(prefix.size());
    }
    return path;
}

} // namespace gateway
