/*
Package proxy 负责把匹配到路由的请求反向代理到后端 upstream。

核心用 Go 标准库 net/http/httputil.ReverseProxy——
它已经帮我们做好了：
  - 请求体拷贝
  - 响应体拷贝
  - 连接复用（Transport）
  - 101 Switching Protocols（WebSocket）

我们只需要改两处：
  1. Director：把 Request 的 URL 改写到 upstream
  2. Transport：自定义连接参数（超时、池大小等）

这就是网关做"路径转发"的本质：
  客户端 → Gateway → 改 URL → ReverseProxy → 后端 → 响应回来
*/
package proxy

import (
	"net/http"
	"net/http/httputil"
	"net/url"
	"time"

	"github.com/softwaredc/simple-gateway/pkg/router"
)

/*
New 根据路由匹配结果构造一个 ReverseProxy。

router.Result 里已经包含：
  - Route.Upstream: "http://127.0.0.1:9000"
  - StripPrefix 配置（如果有的话）
  - Remainder: 通配符匹配到的剩余路径

Director 做的事：
  1. 把 req.URL.Host / Scheme 改成 upstream
  2. 改写 req.URL.Path（StripPrefix + Remainder 组合）
  3. 设置 X-Forwarded-* 头（让后端知道原始信息）

Transport 做的事：
  - 自定义连接参数，比如超时、连接池大小
  - 这样后端慢的时候网关不会永久阻塞

为什么每个 Route 建一个 Proxy 实例？
  - Go httputil.ReverseProxy 内部有 Transport，Transport 是连接池
  - 每个 upstream 用独立连接池最合理
  - 教学版不做 Transport 复用；生产版应该全局复用一个 Transport
*/
func New(result *router.Result) (*httputil.ReverseProxy, error) {
	upstreamURL, err := url.Parse(result.Route.Upstream)
	if err != nil {
		return nil, err
	}

	// Director 是 ReverseProxy 的钩子函数，在转发前改写请求
	director := func(req *http.Request) {
		// 1. 改 scheme + host
		req.URL.Scheme = upstreamURL.Scheme
		req.URL.Host = upstreamURL.Host
		req.Host = upstreamURL.Host

		// 2. 改写 path
		//    StripPrefix：把匹配前缀去掉（如 /api/users → 后端收到空或 /）
		//    Remainder：通配符匹配到的剩余路径（如 123）
		newPath := req.URL.Path
		if result.Route.StripPrefix != "" {
			newPath = stripPrefix(newPath, result.Route.StripPrefix)
		}
		if newPath == "" {
			newPath = "/"
		}
		req.URL.Path = newPath

		// 3. 设置 X-Forwarded-*
		if clientIP(req) != "" {
			req.Header.Set("X-Forwarded-For", clientIP(req))
		}
		if req.Host != "" {
			req.Header.Set("X-Forwarded-Host", req.Host)
		}
		if req.TLS != nil {
			req.Header.Set("X-Forwarded-Proto", "https")
		} else {
			req.Header.Set("X-Forwarded-Proto", "http")
		}
	}

	// Transport：自定义连接参数
	// Go 默认 Transport 已经很好，但教学版显式写出每个配置的含义
	transport := &http.Transport{
		MaxIdleConns:        100,           // 全局最大空闲连接数
		MaxIdleConnsPerHost: 10,            // 每个 upstream 最大空闲连接
		IdleConnTimeout:     90 * time.Second, // 空闲连接存活时间
		ResponseHeaderTimeout: 30 * time.Second, // 读响应头超时
	}

	rp := &httputil.ReverseProxy{
		Director:  director,
		Transport: transport,
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, e error) {
			// 后端不可达时给客户端一个友好提示
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusBadGateway)
			w.Write([]byte(`{"error":"upstream unavailable"}`))
		},
	}

	return rp, nil
}

func stripPrefix(path, prefix string) string {
	// path "/api/users/123", prefix "/api/users" → "/123"
	if prefix == "" {
		return path
	}
	// 如果 prefix 最后没 / 但 path 正好等于 prefix（比如 path=/api/users）
	// 去掉 prefix 后就是空，这里保留原样让上层决定是否补 /
	if path == prefix {
		return ""
	}
	// 如果 path 以 prefix + "/" 开头
	if len(path) > len(prefix) && path[:len(prefix)] == prefix && path[len(prefix)] == '/' {
		return path[len(prefix):]
	}
	// 其他情况（path 不以 prefix 开头）：原样返回
	return path
}

// clientIP 从 X-Forwarded-For 或 RemoteAddr 拿 IP
func clientIP(r *http.Request) string {
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		for i := 0; i < len(xff); i++ {
			if xff[i] == ',' {
				return xff[:i]
			}
		}
		return xff
	}
	if r.RemoteAddr == "" {
		return ""
	}
	// 去掉 :port
	for i := len(r.RemoteAddr) - 1; i >= 0; i-- {
		if r.RemoteAddr[i] == ':' {
			return r.RemoteAddr[:i]
		}
	}
	return r.RemoteAddr
}
