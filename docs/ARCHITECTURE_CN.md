# simple-gateway 架构文档（中文版）

本文档逐层拆解 simple-gateway 的六个核心模块，并对照 Go 和 C++ 两套实现，看同一个设计意图在两种语言里分别怎么落地。两套实现的职责划分完全一致，差异只在语言特性的表达上。

请求从上到下走一遍，响应原路返回。你可以挑任意一层单独读，不需要依赖其他层的内部实现。

```
client ──► [第6层: accept 主循环]
           [第1层: HTTP 解析]
           [第3层: 中间件链]
           [第2层: 路由匹配]
           [第4层: 反向代理转发]
           [第1层: 序列化响应] ──► client
```

---

## 第1层 — HTTP 解析

**职责。** 把 socket 上来的原始字节转成强类型的 Request / Response 结构体，让上层代码能直接用 `req.path`、`req.headers["content-length"]` 这样的字段。这是一层"没人注意但写起来最痛"的基础工作。

**Go（`net/http`）。** 你根本不会碰到解析器。`http.Server` 直接把填好的 `*http.Request` 交给你——method、path、headers、body 全部由标准库解析完毕。`httputil.ReverseProxy` 还帮你把上游响应序列化回客户端。标准库甚至处理了 chunked transfer encoding 和 header 规范化。这是 Go "电池齐全"设计最让人觉得"不公平"的地方之一。

**C++（`include/gateway/http_parser.hpp` + `src/http_parser.cpp`）。** 自己写了一个有状态的 `HttpRequestParser`。解析器是"有状态"的——因为一个 TCP read 可能只送来半条请求，我们得把字节缓存在内部 buffer 里，等看到 `\r\n\r\n`（header 结束标记）再解析 header，然后根据 `Content-Length` 等 body。支持 GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS、query string、大小写不敏感的 header 名。明确不支持 HTTP pipelining、keep-alive 复用、chunked 编码。

### Go 侧的 HTTP 层

```go
// Go 版本里你完全不用写解析代码。
// http.Server 内部：net.Listen → Accept → serveConn → http.ReadRequest → handler
// 你拿到的 handler 已经是 parse 之后的结果：
http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
    // r.Method, r.URL.Path, r.Header, r.Body —— 全部已就绪
})
```

### C++ 侧的 HTTP 层

```cpp
// C++ 版本必须自己喂字节、等完整请求
HttpRequestParser parser;
while (true) {
    size_t n = read_some(client_fd, buf.data(), buf.size());
    if (n == 0) break;

    auto result_opt = parser.feed(buf.data(), n);
    if (!result_opt.has_value()) continue;  // 还没收齐

    auto& [req, err] = *result_opt;
    // req.method, req.path, req.headers, req.body —— 全部已就绪
    break;
}
```

### 为什么这么设计？

Go 依赖标准库帮你做了一切，这是"最小主义"的另一种形式——不是我们不想写，是标准库已经把这件事做透了。C++ 之所以自实现解析器，有两个原因：一是项目坚持"零外部依赖"，不想引入 Boost.Beast 或 cpp-httplib；二是**自己写一遍 HTTP 解析你才知道 `net/http` 到底帮你省了多少事**——header 规范化、chunked 解码、pipelining 都是 Go 开发者平时完全感知不到的复杂度。教学上这一层反而是最有价值的。

---

## 第2层 — 路由匹配

**职责。** 给定请求路径（比如 `/api/users/123`），找到负责处理它的路由规则。这就是网关的路由表，对应 nginx 里 `server { location }` 块的概念。

**Go（`go/pkg/router/router.go`）。** `Router.Match(path)` 对 `cfg.Routes` 做**两轮线性扫描**。第一轮：只看精确匹配（模式没有尾部 `/*`）。第二轮：只看通配前缀模式（`/api/*` 匹配任何以 `/api/` 开头的路径）。没匹配到 → 返回 `nil`。优先级：精确匹配 > 最长通配前缀 > 兜底 `/*`。

为什么线性？源码注释里写得很直白："生产级网关用 radix tree / Trie。这是教学项目。" N 条路由 O(N) 扫描足够，直到你有几千条路由才会成为瓶颈——那不是教学场景。

**C++（`include/gateway/router.hpp` + `src/router.cpp`）。** 算法一模一样。`Router::match(const std::string& path)` —— 精确扫描一遍，通配扫描一遍，返回 `std::optional<MatchResult>`。`MatchResult` 也有同样三个字段：匹配到的 `Route`、匹配的 pattern 字符串、`remainder`（通配符后面的剩余路径段）。

两边还各有一个自由函数 `strip_prefix()`，路由匹配和路径改写故意分开。

### Go 侧路由匹配

```go
// 第一轮：精确匹配
for _, rt := range r.routes {
    if strings.HasSuffix(rt.Path, "/*") { continue }
    if rt.Path == path { return &Result{Route: rt} }
}
// 第二轮：通配前缀
for _, rt := range r.routes {
    if !strings.HasSuffix(rt.Path, "/*") { continue }
    prefix := strings.TrimSuffix(rt.Path, "*")  // "/api/*" → "/api/"
    if strings.HasPrefix(path, prefix) {
        remainder := strings.TrimPrefix(path, prefix)
        return &Result{Route: rt, Remainder: remainder}
    }
}
return nil
```

### C++ 侧路由匹配

```cpp
// 第一轮：精确匹配（跳过带 /* 的 pattern）
for (const auto& rt : routes_) {
    if (is_wildcard(rt.path)) continue;
    if (rt.path == path) return MatchResult{rt, rt.path, ""};
}
// 第二轮：通配前缀
for (const auto& rt : routes_) {
    if (!is_wildcard(rt.path)) continue;
    std::string prefix = rt.path.substr(0, rt.path.size() - 1); // 去掉 '*'
    if (path.compare(0, prefix.size(), prefix) == 0) {
        std::string remainder = path.substr(prefix.size());
        return MatchResult{rt, rt.path, remainder};
    }
}
return std::nullopt;
```

### 为什么这么设计？

线性扫描不是"性能妥协"——Trie 做的本质上也是这两轮扫描，只是把 `O(N)` 的数组查找变成了 `O(L)` 的树遍历（L 是路径长度）。理解了线性扫描怎么工作，再去看 radix tree 就会觉得"哦，原来是给这个循环加了个聪明的索引"——这正是我们想让读者顿悟的时刻。

精确优先于通配的优先级跟 nginx location 规则一致，直觉上也好理解：`/api/users` 应该匹配专门配置的路由，而不是被 `/api/*` 抢走。

---

## 第3层 — 中间件链

**职责。** 把 CORS、请求日志、限流、鉴权这些"横切关注点"从核心路由逻辑里剥离出来。simple-gateway 附带了四个中间件：CORS、RequestLog、RateLimit、AuthHeader。

**Go（`go/pkg/middleware/middleware.go`）。** Go 社区约定俗成的中间件签名：

```go
type Middleware func(http.Handler) http.Handler
```

每个中间件包下一个 handler，返回一个新 handler。`Chain(h, m1, m2, m3)` 从右向左包裹，所以第一个列出的中间件站在洋葱的最外层。请求流：`m1 → m2 → m3 → h`，响应流相反。

**C++（`include/gateway/middleware.hpp` + `src/middleware.cpp`）。** 定义了两个类型别名：

```cpp
using Handler    = std::function<HttpResponse(const HttpRequest&)>;
using Middleware = std::function<Handler(Handler)>;
```

签名和 Go 略有不同——Handler 是同步的，直接返回 `HttpResponse` 值，而不是通过 ResponseWriter + Request 两个参数来"间接返回"。这让每个中间件和核心 handler 都是纯函数，单测极其简单。代价是不支持流式响应，但教学完全够用。

`chain(h, {mw1, mw2, mw3})` 和 Go 版本一样，从右向左包裹。`RateLimit` 用 `std::map<std::string, rateBucket>` + `std::mutex` 保护——算法和 Go 版完全相同，只是类型换了。

### Go 侧中间件链

```go
// Chain 把多个中间件串成洋葱
func Chain(h http.Handler, mws ...func(http.Handler) http.Handler) http.Handler {
    for i := len(mws) - 1; i >= 0; i-- {
        h = mws[i](h)  // 反向包：Chain(A,B,C) = A(B(C(h)))
    }
    return h
}

// 实际装配（gateway/main.go）
h := middleware.Chain(coreHandler,
    middleware.CORS(),
    middleware.RequestLog(),
    middleware.RateLimit(qps),
    middleware.AuthHeader("X-Token"),
)
```

### C++ 侧中间件链

```cpp
// chain 同样反向包裹
Handler chain(Handler h, const std::vector<Middleware>& mws) {
    for (auto it = mws.end(); it != mws.begin(); ) {
        --it;
        h = (*it)(std::move(h));
    }
    return h;
}

// 实际装配（gateway_main.cpp）
auto app = chain(core_handler, {
    cors(),
    request_log(),
    rate_limit(qps),
    auth_header("X-Token"),
});
```

### 为什么这么设计？

洋葱模型最妙的地方是**正交性**——中间件之间完全独立。加一个新中间件只要写一个 `func(Handler) Handler`，随便插 Chain 的哪个位置都不影响其他中间件。第一次理解"Chain = reverse wrap"时很容易懵：为什么不是从左向右包？画个洋葱截面就懂了——先写的中间件站在最外层，要最后被包进去。

---

## 第4层 — 反向代理转发

**职责。** 把匹配到的请求改写后发给上游（scheme、host、path、X-Forwarded-\* 头），拿到响应再带回客户端。这是网关存在的核心意义。

**Go（`go/pkg/proxy/proxy.go`）。** 用 `net/http/httputil.ReverseProxy`。标准库已经处理了请求体拷贝、响应体拷贝、Transport 连接池、WebSocket upgrade。我们的工作只有两件事：

1. 提供 `Director` 回调——改写 `req.URL.Scheme`、`req.URL.Host`、应用 StripPrefix、设置 `X-Forwarded-For` / `X-Forwarded-Host` / `X-Forwarded-Proto`。
2. 提供 `ErrorHandler`——后端挂了时返回干净的 JSON `{"error":"upstream unavailable"}`。

注意：Go 版**没有**按 upstream 缓存 `ReverseProxy` 实例——每个请求都 `proxy.New()` 一个。源码注释明确说了这是生产级 shortcut，因为每个 `ReverseProxy` 带自己的 `http.Transport` 和连接池。真实网关应该在启动时按 upstream 各建一个 proxy 复用。

**C++（`include/gateway/proxy.hpp` + `src/proxy.cpp`）。** 全部手写：

1. `socket()` + `connect()` 连 upstream（`host:port`）。
2. 构造改写后的 `HttpRequest`（改 path、设 Host、加 X-Forwarded-\*）。
3. `serialize()` 转成字节 → `write()` 到上游 fd。
4. 循环 `read()` 上游 fd，用自己的 `HttpResponseParser` 解析出完整响应（没错，我们复用了同一套 parser——HTTP 请求和响应的 wire format 大部分规则相同）。
5. 关上游 socket。

故意把整个上游响应**全量缓存**在内存里再返回。生产网关会做双向管道（一边读 upstream 一边写 client）省内存，但教学版简化优先级更高。

### Go 侧反向代理

```go
rp := &httputil.ReverseProxy{
    Director: func(req *http.Request) {
        req.URL.Scheme = upstream.Scheme
        req.URL.Host   = upstream.Host
        req.Host       = upstream.Host
        req.URL.Path   = stripPrefix(req.URL.Path, route.StripPrefix)
        req.Header.Set("X-Forwarded-For", clientIP)
        req.Header.Set("X-Forwarded-Proto", "http")
    },
    Transport: &http.Transport{ /* 连接池配置 */ },
    ErrorHandler: func(w http.ResponseWriter, r *http.Request, e error) {
        w.WriteHeader(502)
        w.Write([]byte(`{"error":"upstream unavailable"}`))
    },
}
rp.ServeHTTP(w, r)  // 标准库内部处理了请求/响应拷贝
```

### C++ 侧反向代理

```cpp
HttpResponse Proxy::forward(const HttpRequest& req,
                            const MatchResult& result,
                            const std::string& client_ip) {
    int fd = connect_to(host, port);       // socket + connect
    auto up_bytes = build_upstream_request(req, result, upstream, client_ip);
    write_all(fd, up_bytes.data(), up_bytes.size());  // 发请求

    HttpResponseParser rsp;
    std::vector<uint8_t> tmp(4096);
    while (true) {
        size_t n = read_some(fd, tmp.data(), tmp.size());
        auto r = rsp.feed(tmp.data(), n);
        if (r.has_value()) return r->first;  // 拿到完整响应
    }
}
```

### 为什么这么设计？

ReverseProxy 的本质其实很朴素——改 URL、拷贝请求、拷贝响应。Go 版几乎只是套了个 Director 钩子；C++ 版把这个朴素的流程完整展开，每一个 socket 调用、每一次 buffer 读写都摆在你面前。读 C++ 版的 Proxy 实现你会突然明白："哦，原来 nginx 的 `proxy_pass` 底层就是在干这事。"

---

## 第5层 — Server 主循环

**职责。** 网关的外壳：绑定 TCP 端口、accept 连接、分发给请求处理链路。并发模型就在这一层决定。

**Go。** 你也不写这层。`http.ListenAndServe(":8080", h)` 本质就是：

```go
ln, _ := net.Listen("tcp", ":8080")
for {
    conn, _ := ln.Accept()
    go serveConn(conn, h)  // 每个连接一个 goroutine
}
```

每个 `serveConn` goroutine 读请求、调 handler（也就是我们的第2–4层组合）、写响应。Go goroutine 成本极低（初始栈 ~8 KB），所以"一连接一 goroutine"是一个经过验证的、扩展性出乎意料好的设计。

**C++（`include/gateway/server.hpp` + `src/server.cpp`）。** 自己写：

```cpp
listen_fd = socket(); bind(listen_fd, ...); listen(listen_fd, 128);
while (running_.load()) {
    int client_fd = accept(listen_fd, ...);
    std::thread t(&Server::handle_connection, this, client_fd, client_ip);
    t.detach();
}
```

每个连接一个 `std::thread`，线程栈上分配一个 `HttpRequestParser`。线程里循环读字节、喂 parser、请求完整后调 app_handler（middleware chain + router + proxy）。写回响应后线程 close fd 退出。

明确不做 `epoll`、`kqueue` 或 `IOCP`。源码注释说得直接："生产级网关处理 C10K 问题另有办法，但我们要教的是'网关做了什么'，不是'如何把吞吐量榨到极限'。"

### Go 侧主循环

```go
// 你实际写的只有这几行
srv := &http.Server{
    Addr:    ":8080",
    Handler: middleware.Chain(coreHandler, CORS(), RequestLog()),
}
srv.ListenAndServe()
// 内部：net.Listen → for { Accept → go serveConn }
```

### C++ 侧主循环

```cpp
// 主循环
while (running_.load()) {
    int client_fd = accept(listen_fd, ...);
    std::thread([this, client_fd, client_ip]() {
        this->handle_connection(client_fd, client_ip);
    }).detach();
}

// handle_connection 内部
HttpRequestParser parser;
while (true) {
    auto result = parser.feed(buf.data(), n);
    if (!result.has_value()) continue;
    HttpResponse resp = app_handler(result->request);
    auto bytes = serialize(resp);
    write_all(client_fd, bytes.data(), bytes.size());
    break;
}
```

### 为什么这么设计？

"一连接一线程"（或 goroutine）模型的妙处是**可推导性**——一条连接 → 一个 parser → 一次 handler 调用，代码路径清清楚楚。如果上 `epoll` 事件循环，每一层都得变成状态机感知的异步写法，你就再也看不到那条干净的请求生命周期了。

先理解一连接一线程模型，再去看 `epoll`（或 Go runtime 内部的 netpoll），你会清楚地知道"事件循环在优化掉什么"——这是正确的学习顺序。

---

## 关键设计决策回顾

### 为什么不用 Trie 路由？

线性扫描 O(N)，Trie 能降到 O(log N) 或 O(path长度)。但我们跳过 Trie 的原因和跳过 epoll 一样：它是**性能优化**，不是**正确性逻辑**。线性扫描就是 Trie 要实现的算法，只是没那个花哨的数据结构。读 Go/C++ 两版 `Router.Match` 并排看一遍，你自然会想"原来 radix tree 就是给这个循环加个聪明的索引"——这正是我们想让读者产生的顿悟。

### 为什么 C++ 自实现 HTTP 解析器？

两个理由：一是零外部依赖，不想引入 Boost.Beast 或 cpp-httplib；二是**自己写一遍你才知道 HTTP 在线路上到底长什么样**。Go 的 `net/http` 帮你做了 header 规范化、chunked 解码、pipelining 处理，大部分 Go 开发者从没见过这些字节。C++ 版解析器的状态机（累计 header → 等 Content-Length body → 完整请求 ready）是整个项目里三个教学价值最高的模块之一。

### 为什么不做流式响应（C++ proxy）？

全量缓存 upstream 响应再写回客户端，大响应会爆内存。但生产网关的双向管道（一边读 upstream 一边写 client）对教学是噪声——它优化的是内存占用，不改变"改 URL、发请求、带响应"的核心逻辑。全量缓存让 proxy.cpp 的代码从 330 行变成 330 行但逻辑上线性可推导，代价只是大响应场景下不够健壮。教完核心逻辑，读者可以随手把 `Proxy::forward` 改成流式版本当练习。
