# Architecture — simple-gateway

This document walks through the six layers of simple-gateway, comparing the Go
and C++ implementations side-by-side. The two versions share exactly the same
responsibility split — they only differ in how each language expresses it.

Each request flows through the layers top to bottom, then back up on the
response. You can read any layer in isolation; none depends on internals of
the others.

```
client ──► [Layer 6: accept loop]
           [Layer 1: HTTP parse]
           [Layer 3: middleware chain]
           [Layer 2: router match]
           [Layer 4: proxy forward]
           [Layer 1: serialize response] ──► client
```

---

## Layer 1 — HTTP Parse

**Purpose.** Convert raw bytes off the socket into a typed request / response
structure the rest of the code can work with. This is the layer most people
forget exists until they have to write it themselves.

**Go (`net/http`).** You never call a parser. `http.Server` hands you a fully
populated `*http.Request` — method, path, headers, body — all parsed by the
stdlib. `httputil.ReverseProxy` serializes the upstream response back to the
client for you too. The stdlib even handles chunked transfer encoding and
header canonicalization. This is one of the places where Go's batteries-included
design feels almost unfair.

**C++ (`http_parser.hpp`, `http_parser.cpp`).** We wrote our own
`HttpRequestParser` over POSIX sockets. The parser is *stateful* — one TCP
read might deliver only half a request, so we buffer bytes, track whether we
have seen the `\r\n\r\n` header terminator, then read `Content-Length` bytes
for the body. We support GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS, query strings,
and case-insensitive header names. We explicitly *do not* support HTTP
pipelining, keep-alive multiplexing, or chunked transfer.

The C++ parser also owns serialization of responses (`serialize()`), status
text lookup (`status_text()`), and a handful of convenience constructors like
`make_json_response()`. These are tiny helpers but they eliminate a lot of
boilerplate from the server loop.

---

## Layer 2 — Router Match

**Purpose.** Given an incoming path (e.g. `/api/users/123`), find the route
that should handle it. This is the gateway's routing table, analogous to
`server { location }` blocks in nginx.

**Go (`pkg/router/router.go`).** `Router.Match(path)` does a **linear scan**
over `cfg.Routes` in two passes. First pass: exact matches only (pattern with
no trailing `/*`). Second pass: wildcard prefix patterns (`/api/*` matches
anything starting with `/api/`). No match → returns `nil`.

Priority is: exact > longest wildcard prefix > catch-all (`/*`). This mirrors
nginx's location priority fairly closely, though we skip regex entirely for
simplicity.

Why linear? The comments in the source are blunt: "Production gateways use a
radix tree or Trie. This is a teaching project." With N routes, O(N) scanning
is fine until you have thousands of entries — which is not the teaching use
case.

**C++ (`include/gateway/router.hpp`, `src/router.cpp`).** The algorithm is
literally the same. `Router::match(const std::string& path) const` — exact
pass first, wildcard pass second, returns `std::optional<MatchResult>`.
`MatchResult` has the same three fields: the matched `Route`, the matched
pattern string, and `remainder` (the path segment after the wildcard).

A free function `strip_prefix()` exists next to `Router` in both versions —
router matching and path rewriting are intentionally kept separate.

---

## Layer 3 — Middleware Chain

**Purpose.** Cross-cutting concerns that should apply to every request but
should not live inside the core routing logic. CORS, logging, rate limiting,
and auth header checking are the four middlewares shipped with simple-gateway.

**Go (`pkg/middleware/middleware.go`).** Go's idiomatic middleware type is:

```go
type Middleware func(http.Handler) http.Handler
```

Each middleware wraps the next handler. `Chain(h, m1, m2, m3)` wraps right-to-left so the first listed middleware sits on the outside of the onion. Request flow: `m1 → m2 → m3 → h`, response flow reverses.

Four implementations ship:
- `CORS()` — sets cross-origin headers, short-circuits OPTIONS preflight with 204.
- `RequestLog()` — wraps `http.ResponseWriter` to capture status code, prints one line per request.
- `RateLimit(qps)` — per-IP fixed-window counter, returns 429 when over limit.
- `AuthHeader(name)` — checks a custom header exists, returns 401 otherwise.

**C++ (`include/gateway/middleware.hpp`, `src/middleware.cpp`).** We define two type aliases:

```cpp
using Handler    = std::function<HttpResponse(const HttpRequest&)>;
using Middleware = std::function<Handler(Handler)>;
```

The signatures differ slightly from Go — `Handler` is synchronous and returns an `HttpResponse` value rather than mutating a `ResponseWriter&Request` pair. This makes every middleware and the core handler a pure function that is trivial to unit-test. The trade-off: no streaming responses. Fine for teaching.

`chain(h, {mw1, mw2, mw3})` composes with the same right-to-left semantics as Go. Same four middlewares. `RateLimit` uses `std::map<std::string, rateBucket>` protected by `std::mutex` — identical algorithm to Go's version, just typed differently.

---

## Layer 4 — Proxy Forward

**Purpose.** Take the matched request, rewrite it for the upstream (scheme, host, path, X-Forwarded headers), send it, and bring the response back. This is the gateway's raison d'être.

**Go (`pkg/proxy/proxy.go`).** We use `net/http/httputil.ReverseProxy`. The standard library already handles request body copying, response body copying, Transport-based connection pooling, and WebSocket upgrade. Our job is twofold:

1. Provide a `Director` callback that rewrites `req.URL.Scheme`, `req.URL.Host`, `req.Host`, applies `StripPrefix`, and sets `X-Forwarded-For`, `X-Forwarded-Host`, `X-Forwarded-Proto`.
2. Provide an `ErrorHandler` that returns a clean JSON `{"error":"upstream unavailable"}` when the backend is down.

One thing the Go version *does not* do: cache `ReverseProxy` instances per upstream. We construct a new one per request in `gateway/main.go`. The comments flag this as a production shortcut — each `ReverseProxy` carries an `http.Transport` with its own connection pool. A real gateway would build one proxy per upstream at startup.

**C++ (`include/gateway/proxy.hpp`, `src/proxy.cpp`).** We do everything by hand:

1. `socket()` + `connect()` to upstream (`host:port`).
2. Build the rewritten `HttpRequest` (change path, set `Host`, set `X-Forwarded-*`).
3. `serialize()` it into bytes and `write()` to the upstream fd.
4. Loop `read()` on the upstream fd until we have the full response — parse it with our own `HttpRequestParser` (yes, we reuse the same parser to read upstream responses, since HTTP requests and responses share most wire format rules).
5. Close the upstream socket.

We intentionally buffer the *entire* upstream response in memory before returning. A production gateway would stream upstream bytes directly back to the client (bidirectional pipe) to bound memory on large responses. Teaching simplicity wins here.

---

## Layer 5 — Config

**Purpose.** Describe the gateway: listen address, route table, and middleware toggles. Config should be a dumb data container — no logic lives here.

**Go (`pkg/config/config.go`).** `Config` is three structs with `json` tags:

```go
type Config struct { Listen string; Routes []Route; MW MWConfig }
type Route  struct { Path, Upstream, StripPrefix string }
type MWConfig struct { CORS, RequestLog bool; RateLimit int; AuthHeader string }
```

Two entry points:
- `Load(path)` — reads a JSON file with `encoding/json`, returns a parsed `*Config`. Zero external deps.
- `DefaultConfig()` — returns an in-memory default (listen `:8080`, one catch-all route, CORS + RequestLog on). Lets you run `go run cmd/gateway/main.go` with zero arguments.

**C++ (`include/gateway/config.hpp`).** Same three structs, same fields. No JSON loader — we *intentionally* left it out because any JSON library would be a third-party dependency and the project is committed to zero deps. `DefaultConfig()` does the same job: factory function that fills in sensible defaults. Users who want runtime configuration can extend this themselves.

If you ever add a JSON loader, the `Config` struct is already a perfect deserialization target — just add a `LoadJSON(path)` function, nothing else changes.

---

## Layer 6 — Server Accept Loop

**Purpose.** The gateway's outer shell: bind to a TCP port, accept connections, and dispatch each one for request processing. This is where concurrency happens.

**Go.** You don't write this layer either. `http.ListenAndServe(":8080", h)` is essentially:

```go
ln, _ := net.Listen("tcp", ":8080")
for {
    conn, _ := ln.Accept()
    go serveConn(conn, h)  // one goroutine per connection
}
```

Each `serveConn` goroutine reads and parses requests (Layer 1) and invokes
your handler (which layers 2–4 compose). Go's goroutines are cheap (~8 KB
initial stack), so "one goroutine per connection" is a deliberate design
choice that scales surprisingly well.

**C++ (`include/gateway/server.hpp`, `src/server.cpp`).** We write the loop
ourselves:

```cpp
listen_fd = socket(); bind(listen_fd, ...); listen(listen_fd, SOMAXCONN);
while (running_) {
    int client_fd = accept(listen_fd, ...);
    std::thread t(&Server::handle_connection, this, client_fd, client_ip);
    t.detach();
}
```

Each connection gets its own `std::thread` with a `HttpRequestParser` on the
stack. The thread reads bytes in a loop, feeds them to the parser, and when
a complete request is assembled, calls the app handler (which includes the
middleware chain + router + proxy). After writing the response back, the
thread closes the client fd and exits.

We don't use `epoll`, `kqueue`, or `IOCP`. "One thread per connection" is a
talking point in the header comments — "Production gateways handle the C10K
problem differently, but we are teaching what a gateway *does*, not how to
maximize throughput."

---

## Key design decisions

### Why no epoll (C++ accept loop)?

Because the teaching goal is to show the gateway's control flow, not to reach
100k concurrent connections. `std::thread` per connection makes the code path
trivially readable: one connection → one parser → one handler invocation.
Adding an event loop would force every layer to become state-machine aware,
obscuring the request lifecycle we actually want to teach.

If you understand the one-thread-per-connection model, you understand what
`epoll` would optimize *away*. That is the right order to learn it.

### Why self-built HTTP parser in C++?

First: zero external dependencies. A project called "minimal gateway" that
requires Boost.Beast or cpp-httplib stops feeling minimal.

Second: writing the parser teaches you what HTTP actually looks like on the
wire. `net/http` in Go hides so much — header canonicalization, chunked
decoding, pipelining — that most Go developers have never seen the bytes.
The C++ parser's state machine (accumulate header → read Content-Length body
→ full request ready) is one of the three most pedagogically valuable layers
in the whole project.

### Why no Trie router?

Linear scan over N routes is O(N). Production gateways (nginx, APISIX, Envoy)
use radix trees to reduce this to O(log N) or even O(length of path).

We skip it for the same reason we skip epoll: it is a *performance* concern,
not a *correctness* concern. The linear scan is the same algorithm a Trie
would implement, just without the fancy data structure. Reading the Go and C++
`Router.Match` side-by-side should make you think "oh, so a radix tree would
just be a clever index into this same loop" — which is exactly the realization
we want.
