# Contributing

Thanks for considering a contribution to simple-gateway. This document keeps things short — just the practical commands you need.

## Clone

```bash
git clone https://github.com/softwaredc/simple-gateway.git
cd simple-gateway
```

## Build — Go

Requires Go 1.21+.

```bash
cd go

# build all packages (gateway + echo-server)
go build ./...

# run tests (11 tests)
go test ./tests/ -v

# run gateway with default config (listen :8080, catch-all → :9000)
go run cmd/gateway/main.go

# run with a config file
go run cmd/gateway/main.go -c config.json
```

## Build — C++

Requires GCC 9+ or Clang 10+ with C++17 support. Uses POSIX sockets — build inside WSL or any Linux/macOS environment.

```bash
cd cpp
cmake -B build
cmake --build build -j$(nproc)

# run echo-server (upstream) on :9000
./build/echo-server &

# run gateway on :8081 (forwards to echo-server on :9000)
./build/gateway 8081

# smoke test
curl http://localhost:8081/echo
# → {"message":"echo","path":"/echo"}
```

## Code style

- Go: standard `gofmt`. Run `gofmt -w .` before committing.
- C++: `.clang-format` provided. Run `clang-format -i <file>` on changed files.
- Keep comments concise. The Chinese comments in source files are intentional — this is a teaching project. If you touch a file, keep the annotation style consistent.

## PR flow

1. Fork the repo, create a feature branch from `main`.
2. Make changes. **Add tests if you change Go behavior.** C++ has no test harness — use the curl end-to-end smoke test to verify.
3. Run `go test ./tests/ -v` and the C++ build before pushing.
4. Open a PR. Reference the architecture doc (`docs/ARCHITECTURE.md`) if your change affects a cross-layer concern.
5. CI must pass before merge (Go test + C++ build).

## What to work on

- Add a Trie router (issue #10). The linear scan in both `go/pkg/router/router.go` and `cpp/src/router.cpp` is intentionally naive. A radix tree would be a great first contribution.
- HTTP pipelining for the C++ parser. `http_parser.cpp` currently handles one request per connection.
- Streaming proxy for the C++ version (`cpp/src/proxy.cpp`). Currently buffers the full upstream response in memory.
- Add keep-alive support so a single connection can serve multiple requests.
