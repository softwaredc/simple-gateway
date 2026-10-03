# simple-gateway

<div align="center">

[![English](https://img.shields.io/badge/EN-English-blue)](README.md)
[![中文](https://img.shields.io/badge/CN-中文-red)](README_CN.md)

[![CI](https://github.com/softwaredc/simple-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/softwaredc/simple-gateway/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

</div>

> 一个教学级最小 API 网关 —— Go 一份、C++ 一份、源码重度注释、零外部依赖，每版都不到 1000 行。

**simple-gateway** 故意做得很小，但完整可运行。同一份"网关"逻辑用两种语言各写一遍，目的就是让你能一行一行看懂真实网关到底干了哪些事 —— 不是玩具 demo，是真正能转发请求、跑通测试的成品。

### 它能做什么

- **路径路由**，按三条优先级匹配：精确匹配 → 通配前缀匹配（`/*`）→ 兜底路由
- **CORS 中间件**，自动加跨域头、直接处理 OPTIONS 预检
- **按 IP 限流**，固定 1 秒时间窗口，超限返回 429
- **反向代理转发**，改写 URL 并补上 `X-Forwarded-*` 头再交给后端 upstream
- **路径前缀剥离**，让 `/api/users/*` 在后端收到干净的 `/123`

### 6 层架构一览

| 层级 | 职责 | Go 怎么实现 | C++ 怎么实现 |
|------|------|------------|-------------|
| 1. HTTP 解析 | 把原始字节流变成请求结构体 | `net/http` 自动帮我们做了 | 自己写 POSIX socket 上的 `HttpRequestParser` |
| 2. 路由匹配 | 根据 path 找对应的 upstream | `pkg/router/` —— 线性扫描，精确 > 通配 > 兜底 | `include/gateway/router.hpp` —— 同样线性扫描 |
| 3. 中间件链 | 横切关注点（CORS / 日志 / 限流 / 鉴权） | `func(http.Handler) http.Handler` 洋葱模型 | `std::function<Handler(Handler)>` |
| 4. 代理转发 | 把请求送 upstream、把响应拿回来 | `httputil.ReverseProxy` 加一个自定义 Director | 自己 TCP connect + 手动构造 HTTP 请求 |
| 5. 配置加载 | 路由表 + 中间件开关 | `encoding/json` + `DefaultConfig()` | `DefaultConfig()` 工厂函数（零依赖所以没加 JSON loader） |
| 6. 服务器主循环 | 监听端口、每连接一个协程 / 线程 | `net.Listen` + `http.Server` + goroutine | `socket/bind/listen` + 每个连接 spawn 一个 `std::thread` |

深入的设计说明见 [`docs/ARCHITECTURE_CN.md`](docs/ARCHITECTURE_CN.md)。

### 快速上手 —— Go

```bash
# 1. 启动 echo 后端（模拟真实 API）
cd go && go run cmd/echo-server/main.go &

# 2. 启动网关（监听 :8080，把 /echo 转给 :9000）
go run cmd/gateway/main.go &

# 3. 走网关访问
curl http://localhost:8080/echo
# → {"message":"echo","path":"/echo"}
```

跑测试：

```bash
go test ./tests/ -v    # 11 个测试全过
go build ./...         # 验证能编译
```

### 快速上手 —— C++

```bash
# 1. 构建
cd cpp && cmake -B build && cmake --build build -j$(nproc)

# 2. 启动 echo server 在 :9000
./build/echo-server &

# 3. 启动网关在 :8081，转发到 echo server
./build/gateway 8081 &

# 4. 走网关访问
curl http://localhost:8081/echo
# → 跟 Go 版本一模一样的 JSON
```

### 项目结构

```
simple-gateway/
├── go/
│   ├── cmd/
│   │   ├── gateway/main.go        # 网关入口
│   │   └── echo-server/main.go    # 示例后端
│   ├── pkg/
│   │   ├── config/                # Config 结构体 + DefaultConfig + JSON Load
│   │   ├── router/                # path → Route 匹配
│   │   ├── middleware/             # CORS / Log / RateLimit / AuthHeader
│   │   └── proxy/                 # httputil.ReverseProxy 封装
│   └── tests/gateway_test.go      # 11 个集成 + 中间件测试
├── cpp/
│   ├── include/gateway/           # 全部头文件（http_parser、router、...）
│   ├── src/                       # 库实现 .cpp（编译成 gwlib）
│   ├── examples/                  # gateway_main.cpp / server_main.cpp
│   └── CMakeLists.txt
├── docs/
│   ├── ARCHITECTURE_CN.md
│   └── articles/wechat-article.md
├── LICENSE                        # Apache 2.0
└── CONTRIBUTING.md
```

### 为什么做一个"教学网关"

- **每一层都是独立的。** 你可以只读 router 而不碰 proxy，也可以只看中间件。换语言写同一层也不影响其他层。
- **Go vs C++ 并排对照。** 同样 6 层，同样的职责，区别只在每种语言怎么解决它。读完 Go 版本，C++ 版本就像一份反向教材（或者反过来）。
- **没加 epoll、没加 Trie 路由器、C++ 没加 JSON 解析器。** 这些都是工业级网关的正确选择，但它们掩盖了"网关到底是什么"这件事。我们把它们全部拿掉，让主流程裸露出来。
- **源码重度注释。** 每个 .go 和 .cpp 文件开头都有大段中文注释解释设计选择，源码里也穿插着教学行内注释。英文架构文档同步覆盖同样的内容。

### 许可证

Apache License 2.0。详见 [`LICENSE`](LICENSE)。
