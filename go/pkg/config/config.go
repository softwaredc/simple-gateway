/*
Package config 负责读取 simple-gateway 的路由配置。

配置格式极简，用 YAML（标准库没有 YAML，我们用 JSON 代替？
不——YAML 确实需要第三方包。但教学项目强调"零外部依赖"，
所以我们用 Go 标准库 encoding/json + 简单的结构。

等等，既然项目要零依赖，那 YAML/TOML 都不合适。
我们直接用 Go 原生结构——用户在代码里注册路由，或者提供一个 JSON 文件。
但 config 包还是要有：负责从 JSON 反序列化到内存结构。
*/
package config

import (
	"encoding/json"
	"fmt"
	"os"
)

/*
Config 是网关启动时读入的总配置。

示例 JSON（对应 Go struct 的 JSON tag）：

{
  "listen": ":8080",
  "routes": [
    {
      "path": "/api/users/*",
      "upstream": "http://127.0.0.1:9001",
      "strip_prefix": "/api/users"
    },
    {
      "path": "/api/orders",
      "upstream": "http://127.0.0.1:9002"
    }
  ],
  "global_middleware": {
    "cors": true,
    "request_log": true,
    "rate_limit_qps": 100
  }
}

设计说明：
  - Routes 是一条一条的路径 → 后端地址映射
  - StripPrefix 让网关去掉前缀再转发，后端收到的路径更干净
  - GlobalMiddleware 是对所有路由生效的中间件开关
  - 我们不用完整的中间件配置嵌套结构——因为中间件的逻辑写死在代码里，
    配置只决定"开/关"和参数值（如 rate_limit_qps）。
    这比工业级网关简单多了，教学够用。
*/

// Config 网关总配置
type Config struct {
	Listen  string     `json:"listen"`  // 监听地址，如 ":8080"
	Routes  []Route    `json:"routes"`  // 路由表
	MW      MWConfig   `json:"middleware"` // 全局中间件配置
}

// Route 单条路由规则
type Route struct {
	Path        string `json:"path"`                   // 匹配路径，如 "/api/users" 或 "/api/users/*"
	Upstream    string `json:"upstream"`               // 后端地址，如 "http://127.0.0.1:9001"
	StripPrefix string `json:"strip_prefix,omitempty"` // 去掉的前缀（可选）
}

// MWConfig 中间件配置
type MWConfig struct {
	CORS        bool `json:"cors"`          // 开/关 CORS
	RequestLog  bool `json:"request_log"`   // 开/关请求日志
	RateLimit   int  `json:"rate_limit_qps"` // 每秒最大请求数（0 = 不限）
	AuthHeader  string `json:"auth_header"`   // 自定义鉴权 header 名（空 = 不检查）
}

/*
Load 从 JSON 文件读取配置。

为什么用 JSON 而不是 YAML？
  - encoding/json 是 Go 标准库，零外部依赖
  - YAML 需要 gopkg.in/yaml.v3 或 github.com/ghodss/yaml
  - 教学项目优先零依赖
  - 生产项目可以换 YAML，不影响 Go struct 定义
*/
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("config.load: %w", err)
	}

	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("config.parse: %w", err)
	}

	// 校验：listen 不能为空
	if cfg.Listen == "" {
		cfg.Listen = ":8080" // 默认值
	}

	return &cfg, nil
}

/*
DefaultConfig 返回一份内置的默认配置（不读文件也能启动网关）。
这让 simple-gateway 可以 "零配置" 跑起来——Go 的 net/http 也是这个风格。
*/
func DefaultConfig() *Config {
	return &Config{
		Listen: ":8080",
		Routes: []Route{
			{Path: "/echo", Upstream: "http://127.0.0.1:9000"},
		},
		MW: MWConfig{
			CORS:       true,
			RequestLog: true,
			RateLimit:  0,
		},
	}
}
