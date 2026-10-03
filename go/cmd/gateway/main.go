/*
simple-gateway 主程序包。

职责：把 config + router + middleware + proxy 四个包组合起来，
启动一个 net/http Server，处理进来的请求。

请求处理链路：
  client → [middleware chain] → [router.Match] → [proxy.ReverseProxy] → upstream
  (路由没匹配到 → 404 JSON)

这就是网关的全部核心逻辑——从 0 到 Go Serve() 不到 50 行核心代码。
*/
package main

import (
	"fmt"
	"net/http"
	"os"
	"time"

	"github.com/softwaredc/simple-gateway/pkg/config"
	"github.com/softwaredc/simple-gateway/pkg/middleware"
	"github.com/softwaredc/simple-gateway/pkg/proxy"
	"github.com/softwaredc/simple-gateway/pkg/router"
)

/*
Gateway 是网关的运行时对象。

启动流程：
  1. Load / DefaultConfig → 拿到路由表
  2. NewRouter(cfg) → 构造路由器
  3. 注册一个统一的 HTTP handler
  4. middleware.Chain 中间件链包上去
  5. http.ListenAndServe 启动

为什么 handler 是统一的一个？因为 router 是动态的，
每个请求进来临时 Match，然后转给对应 upstream。
这和 nginx 每个 location 一条 handler 本质一样，
但我们用一个 handler 内部 switch，更简单。
*/
type Gateway struct {
	cfg    *config.Config
	router *router.Router
}

// New 构造 Gateway
func New(cfg *config.Config) *Gateway {
	return &Gateway{
		cfg:    cfg,
		router: router.New(cfg),
	}
}

/*
Serve 启动 HTTP 服务，阻塞直到出错。

核心：一个 http.HandlerFunc 处理所有请求：
  step 1: router.Match 找路由
  step 2: proxy.New 构造 ReverseProxy
  step 3: rp.ServeHTTP 转发（Go 标准库内部处理请求/响应拷贝）
  step 4: 路由没匹配到 → 404
*/
func (g *Gateway) Serve() error {
	// 注册所有 routes 的反向代理 handler
	// 注意：我们把所有请求都打到同一个 handler，内部 Match 匹配
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// 1. 路由匹配
		result := g.router.Match(r.URL.Path)
		if result == nil {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusNotFound)
			w.Write([]byte(`{"error":"route not found","path":"` + r.URL.Path + `"}`))
			return
		}

		// 2. 构造反向代理
		//    注意：生产级网关不应该每次请求都 New 一个 proxy——
		//    应该按 upstream 缓存 proxy 实例（每个 upstream 一个连接池）。
		//    但教学版为了逻辑清晰，这里直接创建。
		rp, err := proxy.New(result)
		if err != nil {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusBadGateway)
			w.Write([]byte(`{"error":"proxy create failed"}`))
			return
		}

		// 3. 转发到 upstream
		rp.ServeHTTP(w, r)
	})

	// 中间件链：先 CORS → 再 Log → 再 RateLimit → 最后才进到 handler
	// Chain 从右向左包，所以最终顺序是 CORS → Log → RateLimit → handler
	var h http.Handler = handler
	mws := []func(http.Handler) http.Handler{}
	if g.cfg.MW.CORS {
		mws = append(mws, middleware.CORS())
	}
	if g.cfg.MW.RequestLog {
		mws = append(mws, middleware.RequestLog())
	}
	if g.cfg.MW.RateLimit > 0 {
		mws = append(mws, middleware.RateLimit(g.cfg.MW.RateLimit))
	}
	if g.cfg.MW.AuthHeader != "" {
		mws = append(mws, middleware.AuthHeader(g.cfg.MW.AuthHeader))
	}
	h = middleware.Chain(h, mws...)

	// 启动
	fmt.Printf("[simple-gateway] listening on %s with %d routes\n",
		g.cfg.Listen, len(g.cfg.Routes))
	for _, rt := range g.cfg.Routes {
		fmt.Printf("  %s → %s (strip=%q)\n", rt.Path, rt.Upstream, rt.StripPrefix)
	}

	srv := &http.Server{
		Addr:         g.cfg.Listen,
		Handler:      h,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 30 * time.Second,
	}
	return srv.ListenAndServe()
}

// main 入口
func main() {
	var cfg *config.Config
	var err error

	// 支持两种启动方式：
	//   simple-gateway                    → 用 DefaultConfig
	//   simple-gateway -c config.json     → 读配置文件
	if len(os.Args) >= 3 && os.Args[1] == "-c" {
		cfg, err = config.Load(os.Args[2])
		if err != nil {
			fmt.Fprintf(os.Stderr, "config error: %v\n", err)
			os.Exit(1)
		}
	} else {
		cfg = config.DefaultConfig()
	}

	gw := New(cfg)
	if err := gw.Serve(); err != nil {
		fmt.Fprintf(os.Stderr, "gateway error: %v\n", err)
		os.Exit(1)
	}
}
