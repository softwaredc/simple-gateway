/*
Package middleware 实现网关的 HTTP 中间件。

Go 标准库 net/http 的中间件模式：
  type Middleware func(http.Handler) http.Handler

每个中间件拿到 next handler，返回一个"包装了 next 的新 handler"。
请求进来时先过这个中间件，再调用 next，最后再过回来（洋葱模型）。

示例：
  原始链路：  client → gateway → upstream → gateway → client
  加 Log 后：client → [Log:请求进来] → next → [Log:响应出去] → client
  加 CORS 后：client → [CORS:设置header] → next → client

洋葱模型的好处：中间件之间完全正交，随便加随便减。
生产级网关（nginx/apisix）的插件链也是这个思路。

本包实现 4 个常用中间件：
  1. CORS         — 设置跨域响应头 + 处理预检 OPTIONS
  2. RequestLog   — 打印 method/path/status/latency
  3. RateLimit    — 按 IP 每秒限流（golang.org/x/time/rate 思路，自实现简化版）
  4. AuthHeader   — 检查自定义请求头是否存在（教学用，不是 JWT）
*/
package middleware

import (
	"net"
	"net/http"
	"strconv"
	"sync"
	"time"
)

/*
Chain 把多个中间件串起来，返回一个最终 handler。

顺序：Chain(A, B, C) → 请求先过 A，再过 B，再过 C，再过 handler。
相当于洋葱从外到里 A→B→C→handler。
*/
func Chain(h http.Handler, mws ...func(http.Handler) http.Handler) http.Handler {
	// 反向包：最后一个中间件最先执行
	for i := len(mws) - 1; i >= 0; i-- {
		h = mws[i](h)
	}
	return h
}

/*
CORS 中间件。

为什么网关需要 CORS？因为浏览器直连后端时，CORS 策略由后端控制，
但请求经过网关后，Origin 变了——浏览器认为跨域。所以网关统一处理 CORS。

教学简化：所有域 "*"、所有方法、预检 OPTIONS 直接 204。
生产网关应该精细化配置，但网关里加 CORS 很常见。
*/
func CORS() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// 设置跨域头
			w.Header().Set("Access-Control-Allow-Origin", "*")
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH")
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-*")
			w.Header().Set("Access-Control-Max-Age", "86400")

			// 预检请求（OPTIONS 且带 Origin）直接 204，不转发到后端
			if r.Method == http.MethodOptions && r.Header.Get("Origin") != "" {
				w.WriteHeader(http.StatusNoContent)
				return
			}

			next.ServeHTTP(w, r)
		})
	}
}

/*
RequestLog 中间件。

打印格式（一行）：
  [2026-10-03 15:30:00] GET /api/users/123 → 200 (12ms) [127.0.0.1]

为了拿到响应码和耗时，需要一个 wrap ResponseWriter 的 trick：
  标准 http.ResponseWriter 不记录写了什么状态码。
  我们封一个 loggingWriter，覆写 WriteHeader 记录。
  这是 Go net/http 社区的标准做法（chi/middleware、gorilla/mux 都这么干）。
*/
func RequestLog() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()

			// 把原始 w 包一层，用来捕获状态码
			lw := &loggingWriter{ResponseWriter: w, status: http.StatusOK}

			next.ServeHTTP(lw, r)

			latency := time.Since(start)
			ip := clientIP(r)

			// 不打印到 stdout 的话，生产环境可以注入 logger
			// 教学版直接 fmt.Printf
			ts := start.Format("2006-01-02 15:04:05")
			str := latency.String()
			println("[" + ts + "] " + r.Method + " " + r.URL.Path +
				" → " + strconv.Itoa(lw.status) + " (" + str + ") [" + ip + "]")
		})
	}
}

// loggingWriter 是一个只额外记录 status 的 ResponseWriter
type loggingWriter struct {
	http.ResponseWriter
	status      int
	wroteHeader bool
}

func (l *loggingWriter) WriteHeader(code int) {
	if !l.wroteHeader {
		l.status = code
		l.wroteHeader = true
		l.ResponseWriter.WriteHeader(code)
	}
}

func (l *loggingWriter) Write(b []byte) (int, error) {
	if !l.wroteHeader {
		l.WriteHeader(http.StatusOK) // 有些 handler 不调用 WriteHeader，默认 200
	}
	return l.ResponseWriter.Write(b)
}

// clientIP 从请求里拿客户端 IP（考虑 X-Forwarded-For）
func clientIP(r *http.Request) string {
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		// X-Forwarded-For 格式：client, proxy1, proxy2
		// 取第一个就是真正客户端
		for i := 0; i < len(xff); i++ {
			if xff[i] == ',' {
				return xff[:i]
			}
		}
		return xff
	}
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	return host
}

/*
RateLimit 中间件——按客户端 IP 限流。

算法：令牌桶（token bucket）。每个 IP 一个桶，每秒产生 qps 个令牌。
请求进来先取一个令牌，没令牌返回 429 Too Many Requests。

我们用一个极简实现：每个 IP 记录上次请求时间 + 时间窗口内请求数。
比真正的令牌桶粗糙，但足够教学看限流是什么意思。
生产级限流：golang.org/x/time/rate 或 Sentinel。
*/
func RateLimit(qps int) func(http.Handler) http.Handler {
	if qps <= 0 {
		// qps=0 表示不限流，这个中间件是 no-op
		return func(next http.Handler) http.Handler {
			return next
		}
	}

	// 全局 map：IP → 时间窗口内请求数
	// 用 sync.Mutex 保护并发
	var mu sync.Mutex
	window := time.Second                   // 1 秒窗口
	tracked := make(map[string]*rateBucket) // key=IP

	// 定期清理过期桶（避免 map 无限增长）
	// 教学版简单处理——在每次限流检查时顺便清理最老的
	_ = tracked

	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ip := clientIP(r)

			mu.Lock()
			bucket, ok := tracked[ip]
			now := time.Now()
			if !ok || now.Sub(bucket.start) > window {
				// 新窗口
				bucket = &rateBucket{start: now, count: 1}
				tracked[ip] = bucket
			} else {
				bucket.count++
			}
			allowed := bucket.count <= qps
			mu.Unlock()

			if !allowed {
				w.Header().Set("Retry-After", "1")
				w.WriteHeader(http.StatusTooManyRequests)
				return
			}

			next.ServeHTTP(w, r)
		})
	}
}

// rateBucket 限流桶数据
type rateBucket struct {
	start time.Time // 当前窗口开始时间
	count int       // 窗口内请求数
}

/*
AuthHeader 中间件——检查请求头里必须有某个字段。

不是 JWT 鉴权，就是简单检查 header 是否存在。
教学版告诉读者：网关层可以拦截请求做鉴权，不需要每个后端自己检查。
*/
func AuthHeader(headerName string) func(http.Handler) http.Handler {
	if headerName == "" {
		// 没配置 header → no-op
		return func(next http.Handler) http.Handler {
			return next
		}
	}

	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Header.Get(headerName) == "" {
				w.Header().Set("WWW-Authenticate", headerName)
				w.WriteHeader(http.StatusUnauthorized)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}
