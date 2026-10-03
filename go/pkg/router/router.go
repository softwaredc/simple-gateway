/*
Package router 负责把 HTTP 请求的 path 匹配到配置的 Route。

匹配规则（按优先级从高到低）：
  1. 精确匹配：请求 "/api/users" 匹配路由 "/api/users"
  2. 前缀匹配 + 通配：请求 "/api/users/123" 匹配路由 "/api/users/*"
  3. 最后匹配：请求任何路径匹配路由 "/*"

为什么按这个优先级？和 nginx location 匹配规则类似：
  - exact match 最优先
  - prefix match 次之
  - catch-all 兜底

实现策略：为了教学简单，不做 radix tree 或 Trie 优化，
直接 linear scan 所有 Routes（N 条路由 O(N) 匹配）。
生产级网关（nginx/apisix/envoy）都会用 Trie 优化，但教学版够用。

示例：
  注册路由 ["/api/users", "/api/users/*", "/api/*", "/*"]
  请求 "/api/users"    → 匹配 "/api/users"
  请求 "/api/users/1"  → 匹配 "/api/users/*"
  请求 "/api/orders"   → 匹配 "/api/*"
  请求 "/admin"        → 匹配 "/*"
*/
package router

import (
	"strings"

	"github.com/softwaredc/simple-gateway/pkg/config"
)

// Result 是一次匹配的结果
type Result struct {
	Route       config.Route // 匹配到的路由（含 upstream）
	MatchedPath string       // 匹配到的路由 path pattern
	Remainder   string       // 通配符 * 后面剩余的路径部分
}

// Router 持有路由表，提供 Match 方法
type Router struct {
	routes []config.Route
}

// New 从配置构造 Router
func New(cfg *config.Config) *Router {
	return &Router{routes: cfg.Routes}
}

// Add 手动加一条路由（方便程序式配置）
func (r *Router) Add(route config.Route) {
	r.routes = append(r.routes, route)
}

// Match 把请求 path 匹配到路由，返回 Result 或 nil。
//
// 参数 path 是 HTTP 请求的 URL path（不含 scheme/host，含前导 /）。
// 比如 "GET http://example.com/api/users/123?q=x" 的 path 是 "/api/users/123"。
func (r *Router) Match(path string) *Result {
	// 优先级 1：精确匹配（去掉尾部 * 比）
	for _, rt := range r.routes {
		pattern := rt.Path
		if strings.HasSuffix(pattern, "/*") {
			continue // 通配路由稍后处理
		}
		if pattern == path {
			return &Result{Route: rt, MatchedPath: pattern}
		}
	}

	// 优先级 2：通配前缀匹配（模式 "/api/*" 匹配 path "/api/xxx/yyy"）
	for _, rt := range r.routes {
		pattern := rt.Path
		if !strings.HasSuffix(pattern, "/*") {
			continue
		}
		prefix := strings.TrimSuffix(pattern, "*") // "/api/*" → "/api/"
		if strings.HasPrefix(path, prefix) || path == strings.TrimSuffix(prefix, "/") {
			remainder := strings.TrimPrefix(path, prefix)
			return &Result{Route: rt, MatchedPath: pattern, Remainder: remainder}
		}
	}

	// 没匹配到
	return nil
}

/*
StripPrefix 根据 Route 的 StripPrefix 配置改写转发路径。

例：
  请求 path: /api/users/123
  Route.Path: /api/users/*
  Route.StripPrefix: /api/users
  输出: /123

如果没配置 StripPrefix，返回原 path。
*/
func StripPrefix(path string, route config.Route) string {
	if route.StripPrefix == "" {
		return path
	}
	return strings.TrimPrefix(path, route.StripPrefix)
}
