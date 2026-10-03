# simple-gateway

<div align="center">

[![English](https://img.shields.io/badge/EN-English-blue)](README.md)
[![中文](https://img.shields.io/badge/CN-中文-red)](README_CN.md)

[![CI](https://github.com/softwaredc/simple-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/softwaredc/simple-gateway/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

</div>

> A teaching-grade minimal API gateway — Go + C++, annotated, zero deps, under 1000 LOC each.

**simple-gateway** is a deliberately small, fully working API gateway written twice:
once in Go, once in C++. The sole purpose is to show, line by line, what a real
API gateway actually does under the hood — without giving up on a complete,
runnable product.

### What it does

- **Path-based routing** with three priority levels: exact match → wildcard prefix (`/*`) → catch-all
- **CORS middleware** that sets cross-origin headers and short-circuits OPTIONS preflight
- **Rate limiting** per client IP with a sliding one-second window
- **Reverse proxy** that rewrites URLs and sets `X-Forwarded-*` headers before forwarding to upstreams
- **Path stripping** so that `/api/users/*` can cleanly map to `/123` on the backend

### 6-layer architecture

| Layer | Responsibility | Go implementation | C++ implementation |
|-------|---------------|-------------------|--------------------|
| 1. HTTP Parse | Turn raw bytes into request structs | `net/http` does it for us | Self-implemented `HttpRequestParser` over POSIX sockets |
| 2. Router Match | Decide which upstream handles a path | `pkg/router/` — linear scan, exact > wildcard > catch-all | `include/gateway/router.hpp` — same linear scan |
| 3. Middleware Chain | Cross-cutting concerns (CORS, log, rate-limit, auth) | `func(http.Handler) http.Handler` | `std::function<Handler(Handler)>` |
| 4. Proxy Forward | Send the request upstream, bring response back | `httputil.ReverseProxy` with a custom Director | Self-built TCP connect + manual HTTP serialize |
| 5. Config | Load routes and middleware options | `encoding/json` + `DefaultConfig()` | `DefaultConfig()` factory (no JSON loader, zero deps) |
| 6. Server Accept Loop | Listen on a port, handle one connection per goroutine / thread | `net.Listen` + `http.Server` + goroutines | `socket/bind/listen` + `std::thread` per connection |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for a deep dive.

### Quick Start — Go

```bash
# 1. Start the echo backend (simulates your real API)
cd go && go run cmd/echo-server/main.go &

# 2. Start the gateway (listens on :8080, forwards /echo → :9000)
go run cmd/gateway/main.go &

# 3. Hit the gateway
curl http://localhost:8080/echo
# → {"message":"echo","path":"/echo"}
```

Run tests with:

```bash
go test ./tests/ -v        # 11 tests, all pass
go build ./...             # verify it compiles
```

### Quick Start — C++

```bash
# 1. Build
cd cpp && cmake -B build && cmake --build build -j$(nproc)

# 2. Start echo server on :9000
./build/echo-server &

# 3. Start gateway on :8081, forwarding to echo server
./build/gateway 8081 &

# 4. Hit the gateway
curl http://localhost:8081/echo
# → same JSON as Go version
```

### Project structure

```
simple-gateway/
├── go/
│   ├── cmd/
│   │   ├── gateway/main.go        # gateway entry
│   │   └── echo-server/main.go    # example upstream
│   ├── pkg/
│   │   ├── config/                # Config struct + DefaultConfig + JSON Load
│   │   ├── router/                # path → Route matching
│   │   ├── middleware/             # CORS / Log / RateLimit / AuthHeader
│   │   └── proxy/                 # httputil.ReverseProxy wrapper
│   └── tests/gateway_test.go      # 11 integration + middleware tests
├── cpp/
│   ├── include/gateway/           # all headers (http_parser, router, ...)
│   ├── src/                       # library .cpp files (gwlib)
│   ├── examples/                  # gateway_main.cpp / server_main.cpp
│   └── CMakeLists.txt
├── docs/
│   ├── ARCHITECTURE.md
│   └── articles/wechat-article.md
├── LICENSE                        # Apache 2.0
└── CONTRIBUTING.md
```

### Why a teaching gateway

- **Every layer is independent.** You can read the router without reading the
  proxy. You can swap the parser (and the C++ version *does* — it is the layer
  that differs the most from Go).
- **Go vs C++ side-by-side.** The same six layers exist in both trees with the
  same responsibilities — the difference is only how each language solves them.
  This makes the Go version feel like a cheat sheet for the C++ version (or
  vice versa).
- **No epoll, no Trie router, no JSON config loader in C++.** Those are fine
  decisions for production but they are *not* what a gateway is really about.
  We strip them out so the control flow is visible.
- **Heavily annotated.** Source files carry long block comments in Chinese
  explaining every non-obvious choice. The English docs mirror that structure.

### License

Apache License 2.0. See [`LICENSE`](LICENSE).
