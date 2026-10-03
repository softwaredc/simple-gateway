/*
Echo-server 是 simple-gateway 的演示后端。
它就做一件事：回显收到的请求。

为什么需要它？因为网关是"转发请求到上游"，
没有上游就没法演示。这个 echo-server 模拟真实微服务。

运行：
  go run cmd/echo-server/main.go   # 默认 9000 端口

然后 gateway 配置里加 route：
  { "path": "/echo", "upstream": "http://127.0.0.1:9000" }

curl 测试：
  curl -X POST http://127.0.0.1:8080/echo \
       -H "Content-Type: application/json" \
       -d '{"hello":"world"}'

echo-server 会把请求原样用 JSON 包一层返回。
*/
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"time"
)

// EchoResponse 是返回 JSON 的结构
type EchoResponse struct {
	Method  string            `json:"method"`
	Path    string            `json:"path"`
	Query   string            `json:"query,omitempty"`
	Headers map[string]string `json:"headers"`
	Body    string            `json:"body"`
	EchoAt  time.Time         `json:"echo_at"`
}

// echoHandler 回显所有请求
func echoHandler(w http.ResponseWriter, r *http.Request) {
	// 读 body（限制大小，防止炸内存）
	body, _ := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	defer r.Body.Close()

	// 收集 headers（Go 把 header 存成 map[string][]string，我们取第一个值）
	headers := make(map[string]string)
	for k, v := range r.Header {
		if len(v) > 0 {
			headers[k] = v[0]
		}
	}

	resp := EchoResponse{
		Method:  r.Method,
		Path:    r.URL.Path,
		Query:   r.URL.RawQuery,
		Headers: headers,
		Body:    string(body),
		EchoAt:  time.Now(),
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)

	fmt.Printf("[echo] %s %s → 200 (body=%d bytes)\n", r.Method, r.URL.Path, len(body))
}

func main() {
	port := "9000"
	if len(os.Args) >= 2 {
		port = os.Args[1]
	}
	addr := ":" + port

	mux := http.NewServeMux()
	mux.HandleFunc("/", echoHandler)

	// 加一个健康检查 endpoint（网关 upstream health check 也能用）
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"ok"}`))
	})

	fmt.Printf("[echo-server] listening on %s\n", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		fmt.Fprintf(os.Stderr, "echo-server error: %v\n", err)
		os.Exit(1)
	}
}
