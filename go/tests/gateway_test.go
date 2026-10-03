/*
Package tests 是 simple-gateway 的外部集成测试包。

为什么用 package tests 而不是 package router（或 middleware）？
  - 内部测试（package router + _test.go）可以访问未导出符号
  - 外部测试（package tests）只能通过公开 API 使用包
  - 我们想模拟"真实用户怎么用 simple-gateway"，所以用外部测试包
  - 教学项目两种风格都好，但外部包更能倒逼公开 API 设计合理

编译时：go test ./tests/
  - Go 会把 tests/ 当成一个单独的包
  - 里面只有 gateway_test.go，编译成测试可执行文件运行
*/
package tests

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/softwaredc/simple-gateway/pkg/config"
	"github.com/softwaredc/simple-gateway/pkg/middleware"
	"github.com/softwaredc/simple-gateway/pkg/proxy"
	"github.com/softwaredc/simple-gateway/pkg/router"
)

/*
================================================================================
测试 1: Router.Match —— 精确匹配
================================================================================

教学点：
  Router.Match 按优先级匹配路由。最高优先级是"精确匹配"：
  注册路由 ["/api/users", "/api/*", "/*"]
  请求 "/api/users"  →  应该命中精确路由，不能被通配抢走

为什么这很重要？
  如果优先级错了，"/api/users" 可能被 "/api/*" 先匹配，
  那专门给精确路径写的 upstream 就永远接不到流量。
  nginx 的 location 匹配也是这个思路（= 精确 > ^~ 前缀 > ~ 正则）。
*/
func TestRouter_Match_Exact(t *testing.T) {
	// Step 1: 构造路由表（模拟网关配置了三条路由）
	cfg := &config.Config{
		Routes: []config.Route{
			{Path: "/api/users", Upstream: "http://u1:9001"},
			{Path: "/api/*", Upstream: "http://u2:9002"},
			{Path: "/*", Upstream: "http://catchall:9999"},
		},
	}

	r := router.New(cfg)

	// Step 2: 调用 Match
	res := r.Match("/api/users")
	if res == nil {
		t.Fatal("期望匹配到路由，但 Match 返回 nil")
	}

	// Step 3: 断言命中了精确路由（不是通配！）
	if res.Route.Path != "/api/users" {
		t.Errorf("期望命中 /api/users，实际命中 %q", res.Route.Path)
	}

	// 精确匹配不会产生 remainder（通配才会有）
	if res.Remainder != "" {
		t.Errorf("精确匹配的 Remainder 应为空，实际 %q", res.Remainder)
	}

	t.Logf("✓ 精确匹配成功: path=%s → route=%s upstream=%s",
		"/api/users", res.Route.Path, res.Route.Upstream)
}

/*
================================================================================
测试 2: Router.Match —— 通配前缀匹配
================================================================================

教学点：
  优先级 2 是通配前缀匹配（pattern 以 "/*" 结尾）。
  当精确匹配失败后，会找最长匹配的通配路由。

  注册路由 ["/api/users", "/api/*", "/*"]
  请求 "/api/users/123"  →  精确路由不匹配（尾部多了 /123）
                           →  通配路由 "/api/*" 应该命中
                           →  Remainder = "users/123"（"/*" 后面的剩余）

  Remainder 用来干嘛？网关改写转发路径时会拼上它。
  比如 StripPrefix="/api"，那么后端收到的就是 "/users/123"。
*/
func TestRouter_Match_Wildcard(t *testing.T) {
	cfg := &config.Config{
		Routes: []config.Route{
			{Path: "/api/users", Upstream: "http://u1:9001"},
			{Path: "/api/*", Upstream: "http://u2:9002"},
			{Path: "/*", Upstream: "http://catchall:9999"},
		},
	}

	r := router.New(cfg)

	// 请求带子路径，精确路由不匹配
	res := r.Match("/api/users/123")
	if res == nil {
		t.Fatal("期望匹配到路由，但 Match 返回 nil")
	}

	// 应该命中 /api/* 而不是 /*
	if res.Route.Path != "/api/*" {
		t.Errorf("期望命中 /api/*，实际命中 %q", res.Route.Path)
	}

	// Remainder 是 /api/ 之后的剩余部分
	if res.Remainder != "users/123" {
		t.Errorf("期望 Remainder=users/123，实际 %q", res.Remainder)
	}

	t.Logf("✓ 通配匹配成功: path=%s → route=%s remainder=%q",
		"/api/users/123", res.Route.Path, res.Remainder)
}

/*
================================================================================
测试 3: Router.Match —— 兜底路由（Catch-All）
================================================================================

教学点：
  当没有任何精确或通配前缀匹配时，由 "/*" 兜底。
  注意：路由表里必须显式配置 "/*"，网关才不会对未知路径返回 404。
  如果路由表里没有 /*，Match 会返回 nil。

  注册路由 ["/api/users/*", "/*"]
  请求 "/admin" → 不匹配 /api/users/* → 命中 /*
*/
func TestRouter_Match_CatchAll(t *testing.T) {
	cfg := &config.Config{
		Routes: []config.Route{
			{Path: "/api/users/*", Upstream: "http://users:9001"},
			{Path: "/*", Upstream: "http://catchall:9999"},
		},
	}

	r := router.New(cfg)

	res := r.Match("/admin")
	if res == nil {
		t.Fatal("期望匹配到 /* 兜底路由，实际返回 nil")
	}

	if res.Route.Path != "/*" {
		t.Errorf("期望命中 /*，实际命中 %q", res.Route.Path)
	}

	// Remainder 应该是 "admin"（"/*" 去掉 * 就是 "/"，TrimPrefix("/admin", "/") = "admin"）
	if res.Remainder != "admin" {
		t.Errorf("期望 Remainder=admin，实际 %q", res.Remainder)
	}

	t.Logf("✓ 兜底匹配成功: path=%s → route=%s", "/admin", res.Route.Path)
}

/*
================================================================================
测试 4: StripPrefix 路径改写
================================================================================

教学点：
  StripPrefix 是网关的核心能力之一——把外部请求 URL 的前缀去掉再转发给后端。
  好处：
    - 外部可以统一用 /api/users/... 前缀
    - 后端内部只认识 /123 这种干净路径
    - 多个后端服务之间不会路径冲突

  例：请求 /api/users/123，Route.StripPrefix="/api/users"
      → 后端收到 /123

  这不是 router.Match 的一部分，是 proxy 转发时的改写步骤，
  但 router 包里提供了独立的 StripPrefix 函数方便复用。
*/
func TestRouter_StripPrefix(t *testing.T) {
	tests := []struct {
		name    string
		path    string
		route   config.Route
		want    string
	}{
		{
			name:  "基本去前缀",
			path:  "/api/users/123",
			route: config.Route{StripPrefix: "/api/users"},
			want:  "/123",
		},
		{
			name:  "无前缀配置返回原路径",
			path:  "/api/users/123",
			route: config.Route{},
			want:  "/api/users/123",
		},
		{
			name:  "前缀正好等于路径本身",
			path:  "/api/users",
			route: config.Route{StripPrefix: "/api/users"},
			want:  "", // TrimPrefix 后变空（proxy 会补 /）
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := router.StripPrefix(tc.path, tc.route)
			if got != tc.want {
				t.Errorf("StripPrefix(%q, StripPrefix=%q) = %q, want %q",
					tc.path, tc.route.StripPrefix, got, tc.want)
			}
		})
	}

	t.Log("✓ StripPrefix 全部通过")
}

/*
================================================================================
测试 5: CORS 中间件 —— 预检请求（Preflight）
================================================================================

教学点：
  浏览器跨域请求会先发 OPTIONS 预检：
    1. 浏览器：OPTIONS /api/users Origin: http://evil.com
    2. 网关：返回 204 + Access-Control-Allow-Origin
    3. 浏览器看到跨域被允许，才会发真正的 GET/POST

  CORS 中间件做的事：
    - 对所有响应写 Access-Control-Allow-Origin 等头
    - 对 OPTIONS + Origin 直接返回 204，不转发到后端

  我们用 httptest.Server 启动一个"网关"（中间件包一个 dummy handler），
  然后模拟浏览器发 OPTIONS，检查响应。
*/
func TestMiddleware_CORS_Preflight(t *testing.T) {
	// Step 1: 构造一个被 CORS 包装的 handler
	dummy := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("upstream body"))
	})

	handler := middleware.CORS()(dummy)

	// Step 2: 启动 httptest server（模拟网关）
	srv := httptest.NewServer(handler)
	defer srv.Close()

	// Step 3: 发送预检请求（OPTIONS + Origin）
	req, _ := http.NewRequest(http.MethodOptions, srv.URL+"/api/users", nil)
	req.Header.Set("Origin", "http://frontend.example.com")
	req.Header.Set("Access-Control-Request-Method", "GET")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()

	// Step 4: 断言状态码是 204 No Content（预检不转发到后端）
	if resp.StatusCode != http.StatusNoContent {
		t.Errorf("期望 204，实际 %d", resp.StatusCode)
	}

	// Step 5: 断言 CORS 头存在
	if origin := resp.Header.Get("Access-Control-Allow-Origin"); origin != "*" {
		t.Errorf("期望 Access-Control-Allow-Origin: *，实际 %q", origin)
	}
	if methods := resp.Header.Get("Access-Control-Allow-Methods"); methods == "" {
		t.Error("期望 Access-Control-Allow-Methods 头存在")
	}

	t.Logf("✓ CORS 预检通过: status=%d, Origin=%s",
		resp.StatusCode, resp.Header.Get("Access-Control-Allow-Origin"))
}

/*
================================================================================
测试 6: RequestLog 中间件
================================================================================

教学点：
  RequestLog 是"副作用型"中间件——它不改变请求/响应的业务逻辑，
  只是打印一行日志。
  测试这种中间件很简单：启动 server，发请求，断言 handler 正常工作。
  日志打印本身不影响测试结果（我们没法 assert stdout），
  只需要确认中间件没有破坏正常流程。

  为什么需要 loggingWriter？
    Go 标准 http.ResponseWriter 接口里没有 "拿到已写状态码" 的方法。
    中间件要记录响应码，必须包一层 ResponseWriter，
    覆写 WriteHeader 捕获 status 并存到自己的字段里。
    这是 chi/gorilla/mux 等框架的标准做法。
*/
func TestMiddleware_RequestLog(t *testing.T) {
	// 用 atomic 计数，确认 handler 被调用了正确次数
	var handlerCalled int64
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt64(&handlerCalled, 1)
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("ok"))
	})

	// Chain 里 RequestLog 包 inner
	handler := middleware.Chain(inner, middleware.RequestLog())

	srv := httptest.NewServer(handler)
	defer srv.Close()

	// 发 3 次请求
	for i := 0; i < 3; i++ {
		resp, err := http.Get(srv.URL + "/ping")
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			t.Errorf("第 %d 次请求期望 200，实际 %d", i+1, resp.StatusCode)
		}
		if string(body) != "ok" {
			t.Errorf("期望 body=ok，实际 %q", body)
		}
	}

	if got := atomic.LoadInt64(&handlerCalled); got != 3 {
		t.Errorf("handler 应被调用 3 次，实际 %d", got)
	}

	t.Log("✓ RequestLog 通过（请求正常流转，日志副作用不计入断言）")
}

/*
================================================================================
测试 7: RateLimit 限流中间件
================================================================================

教学点：
  网关限流是保护后端的核心手段。这里实现的是"固定窗口计数器"：
  - 每个 IP 在 1 秒窗口内最多 qps 次请求
  - 超过则返回 429 Too Many Requests + Retry-After 头

  测试策略：
    QPS=2 → 快速发 3 次请求
    #1、#2 成功（200）
    #3 应该被拒绝（429）

  为什么用 httptest？因为 RateLimit 中间件内部有 sync.Mutex + map，
  必须走真实 HTTP 请求才会触发并发场景（虽然本测试不特意并发）。
*/
func TestMiddleware_RateLimit(t *testing.T) {
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("allowed"))
	})

	// QPS=2：每秒最多 2 次请求
	handler := middleware.Chain(inner, middleware.RateLimit(2))

	srv := httptest.NewServer(handler)
	defer srv.Close()

	// 同一个客户端（默认 RemoteAddr 相同）快速发 3 次
	for i := 0; i < 3; i++ {
		resp, err := http.Get(srv.URL + "/api/test")
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()

		if i < 2 {
			// 前 2 次应成功
			if resp.StatusCode != http.StatusOK {
				t.Errorf("第 %d 次应 200，实际 %d (body=%s)",
					i+1, resp.StatusCode, string(body))
			}
		} else {
			// 第 3 次应被限流
			if resp.StatusCode != http.StatusTooManyRequests {
				t.Errorf("第 3 次应 429，实际 %d", resp.StatusCode)
			}
			if retry := resp.Header.Get("Retry-After"); retry == "" {
				t.Error("429 响应应带 Retry-After 头")
			}
		}
	}

	t.Log("✓ RateLimit 通过: 前 2 次 200，第 3 次 429")
}

/*
================================================================================
测试 8: AuthHeader 鉴权中间件
================================================================================

教学点：
  网关层鉴权 vs 后端鉴权：
    - 后端自己鉴权：每个服务都要写一遍 JWT 验证逻辑，重复造轮子
    - 网关统一鉴权：一处配置，所有后端受益

  这里是极简版——只检查某个 header 是否存在（不是 JWT）。
  - 请求带 X-Token → 放行
  - 不带 X-Token → 返回 401 Unauthorized + WWW-Authenticate 头

  这种"header 存在性检查"其实就是 API Key 鉴权的雏形。
  生产版会配合签名验证、过期时间等。
*/
func TestMiddleware_AuthHeader(t *testing.T) {
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("secret data"))
	})

	// 要求必须带 X-Token 头
	handler := middleware.Chain(inner, middleware.AuthHeader("X-Token"))

	srv := httptest.NewServer(handler)
	defer srv.Close()

	t.Run("缺少 header 返回 401", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/protected")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusUnauthorized {
			t.Errorf("期望 401，实际 %d", resp.StatusCode)
		}
		if auth := resp.Header.Get("WWW-Authenticate"); auth != "X-Token" {
			t.Errorf("期望 WWW-Authenticate: X-Token，实际 %q", auth)
		}
	})

	t.Run("带 header 返回 200", func(t *testing.T) {
		req, _ := http.NewRequest(http.MethodGet, srv.URL+"/protected", nil)
		req.Header.Set("X-Token", "my-secret-key")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			t.Errorf("期望 200，实际 %d", resp.StatusCode)
		}
		body, _ := io.ReadAll(resp.Body)
		if string(body) != "secret data" {
			t.Errorf("期望 body=secret data，实际 %q", string(body))
		}
	})

	t.Log("✓ AuthHeader 通过：缺 header → 401，有 header → 200")
}

/*
================================================================================
测试 9: 端到端集成 —— Gateway → ReverseProxy → Upstream
================================================================================

教学点：
  这是最接近真实使用场景的测试。我们同时启动两个 httptest server：

    client → gateway (proxy.New) → upstream (echo-server)

  用 proxy.New 根据 router.Result 构造 ReverseProxy，
  然后让 gateway handler 把请求反向代理到 upstream。

  测试要点：
    1. upstream 收到的路径被正确改写（StripPrefix 生效）
    2. upstream 收到的 X-Forwarded-For 头存在（clientIP 正确写入）
    3. client 收到的响应体来自 upstream

  为什么要用 httptest？因为真实网关监听 0.0.0.0:8080，
  测试里起真实端口会冲突。httptest 会自动选一个空闲端口。
*/
func TestProxy_Integration(t *testing.T) {
	// ======================================================================
	// Step 1: 启动 upstream（echo-server）——一个简单的 handler，
	//         把收到的 path 和 X-Forwarded-For 回显给调用方
	// ======================================================================
	type upstreamResp struct {
		Path           string `json:"path"`
		ForwardedFor   string `json:"x_forwarded_for"`
		ForwardedHost  string `json:"x_forwarded_host"`
		ForwardedProto string `json:"x_forwarded_proto"`
	}

	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(upstreamResp{
			Path:           r.URL.Path,
			ForwardedFor:   r.Header.Get("X-Forwarded-For"),
			ForwardedHost:  r.Header.Get("X-Forwarded-Host"),
			ForwardedProto: r.Header.Get("X-Forwarded-Proto"),
		})
	}))
	defer upstream.Close()

	// 把 upstream 的 httptest URL 转成 url.URL
	upstreamURL, _ := url.Parse(upstream.URL)

	// ======================================================================
	// Step 2: 用 proxy.New 构造 ReverseProxy
	//         模拟路由匹配结果：path="/api/users/123" 命中 "/api/users/*"
	//         StripPrefix="/api/users" → 后端收到 "/123"
	// ======================================================================
	route := config.Route{
		Path:        "/api/users/*",
		Upstream:    upstreamURL.String(),
		StripPrefix: "/api/users",
	}

	result := &router.Result{
		Route:     route,
		MatchedPath: "/api/users/*",
		Remainder:  "123", // 通配符匹配的剩余部分
	}

	rp, err := proxy.New(result)
	if err != nil {
		t.Fatalf("proxy.New 失败: %v", err)
	}

	// ======================================================================
	// Step 3: 启动 gateway server —— handler 就是 ReverseProxy
	// ======================================================================
	gateway := httptest.NewServer(rp)
	defer gateway.Close()

	// ======================================================================
	// Step 4: 从"客户端"发请求到 gateway
	// ======================================================================
	resp, err := http.Get(gateway.URL + "/api/users/123")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("期望 200，实际 %d", resp.StatusCode)
	}

	var got upstreamResp
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatalf("解析 upstream 响应失败: %v", err)
	}

	// ======================================================================
	// Step 5: 断言
	// ======================================================================
	// StripPrefix 生效：后端收到的路径应该是 "/123"
	if got.Path != "/123" {
		t.Errorf("后端收到路径期望 /123 (StripPrefix 去 /api/users)，实际 %q", got.Path)
	}

	// X-Forwarded-For 应该有值（来自 proxy.Director 的设置）
	if got.ForwardedFor == "" {
		t.Error("期望 X-Forwarded-For 头被 proxy 写入，实际为空")
	}

	// X-Forwarded-Proto 应该是 http（因为 httptest 默认 http）
	if got.ForwardedProto != "http" {
		t.Errorf("期望 X-Forwarded-Proto=http，实际 %q", got.ForwardedProto)
	}

	t.Logf("✓ 端到端通过: gateway → upstream, path=%s, XFF=%s",
		got.Path, got.ForwardedFor)
}

/*
================================================================================
测试 10: 网关未匹配路径返回 404 JSON
================================================================================

教学点：
  真实网关有两个层面的 404：
    1. Router.Match 返回 nil（没有任何路由能匹配）
    2. 匹配到路由但 upstream 返回 404

  本测试模拟第 1 种情况——router 拒绝匹配，
  网关应该返回清晰的 JSON 错误，而不是裸 404 页面。

  测试策略：构造一个最小 gateway handler，
  用 router.Match 判断路径，没匹配到就返回 JSON 404。
  这样既测试了 router 的 nil 返回，也教大家网关主循环怎么写。
*/
func TestGateway_NotFound(t *testing.T) {
	// Step 1: 构造只有一条路由的 router
	r := router.New(&config.Config{
		Routes: []config.Route{
			{Path: "/api/health", Upstream: "http://health:9000"},
		},
	})

	// Step 2: 构造一个最小 gateway handler
	//         教学版不关心 proxy 转发逻辑，只演示 router nil 时的处理
	gatewayHandler := http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		res := r.Match(req.URL.Path)
		if res == nil {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusNotFound)
			w.Write([]byte(`{"error":"route not found","path":"` + req.URL.Path + `"}`))
			return
		}
		// 正常情况：这里会 proxy 到 upstream，简化演示
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("matched: " + res.Route.Path))
	})

	srv := httptest.NewServer(gatewayHandler)
	defer srv.Close()

	// Step 3: 测试未匹配路径
	t.Run("未匹配路径返回 404 JSON", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/no/such/path")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusNotFound {
			t.Errorf("期望 404，实际 %d", resp.StatusCode)
		}
		if ct := resp.Header.Get("Content-Type"); !strings.Contains(ct, "application/json") {
			t.Errorf("期望 Content-Type 含 application/json，实际 %q", ct)
		}

		body, _ := io.ReadAll(resp.Body)
		var errResp map[string]string
		if err := json.Unmarshal(body, &errResp); err != nil {
			t.Fatalf("期望 JSON 响应，实际解析失败: %v (body=%s)", err, string(body))
		}
		if errResp["error"] == "" {
			t.Error("期望 JSON 里有 error 字段")
		}
	})

	// Step 4: 顺便测试匹配路径仍然 200（确保 nil 判断没影响正常请求）
	t.Run("匹配路径正常返回 200", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/api/health")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			t.Errorf("期望 200，实际 %d", resp.StatusCode)
		}
	})

	t.Log("✓ 未匹配路径 404 JSON 通过")
}

/*
================================================================================
Bonus: 中间件组合（Middleware Chain）—— 一个额外的教学用例
================================================================================

教学点：
  middleware.Chain 把多个中间件"叠"起来，顺序很重要：
  Chain(inner, A, B, C) → 请求链路 A → B → C → inner
  （洋葱模型：外面的中间件先收到请求）

  我们组合 RateLimit + AuthHeader 看它们能不能互不干扰：
  限流通过 AND 鉴权通过 → 放行
  限流通过 AND 鉴权失败 → 401
  限流失败             → 429

  这里只是演示，不做严格断言，主要是让读者看到 Chain 的效果。
*/
func TestMiddleware_Chain_Composition(t *testing.T) {
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("hello"))
	})

	// Chain: RateLimit(10) → AuthHeader("X-Key") → inner
	handler := middleware.Chain(inner,
		middleware.RateLimit(10),
		middleware.AuthHeader("X-Key"),
	)

	srv := httptest.NewServer(handler)
	defer srv.Close()

	// Case A: 没带 header → 401
	t.Run("Chain: 无 header → 401", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/a")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusUnauthorized {
			t.Errorf("期望 401，实际 %d", resp.StatusCode)
		}
	})

	// Case B: 带 header → 200
	t.Run("Chain: 有 header → 200", func(t *testing.T) {
		req, _ := http.NewRequest(http.MethodGet, srv.URL+"/b", nil)
		req.Header.Set("X-Key", "k")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("期望 200，实际 %d", resp.StatusCode)
		}
	})

	t.Log("✓ 中间件 Chain 组合通过")
}

/*
================================================================================
辅助函数 / 常量
================================================================================

当前测试不需要额外辅助函数——所有内部逻辑都是内联的。
如果未来有需要，比如构造一个通用的"echo server"启动函数，
可以在这里添加。
*/
