// ============================================================================
// middleware.cpp —— HTTP 中间件实现（CORS / RequestLog / RateLimit / AuthHeader）
// ----------------------------------------------------------------------------
// 洋葱模型：Chain(h, mw1, mw2, mw3) → mw1(mw2(mw3(h)))
//   请求进来：mw1 → mw2 → mw3 → h
//   响应回去：h → mw3 → mw2 → mw1
//
// 设计要点：
//   - Handler 是"同步函数对象"：HttpResponse(req)
//   - 中间件就是包一层 Handler，在调用 next 前后做拦截
//   - 这种设计天然适合纯函数测试——和 socket 无关
//
// 共享状态的问题：
//   RateLimit 中间件需要一个"全局的 IP→桶 map"在所有请求之间共享。
//   用 std::shared_ptr<std::map<...>> + std::shared_ptr<std::mutex>
//   包装后被 lambda 捕获——所有请求通过同一个 shared_ptr 指向同一份数据。
// ============================================================================

#include "gateway/middleware.hpp"

#include <cstdio>       // fprintf
#include <ctime>        // std::time / std::strftime
#include <memory>       // std::shared_ptr
#include <sstream>

namespace gateway {

// ---------------------------------------------------------------------------
// Chain —— 反向包裹（最右的中间件最内层）
// ---------------------------------------------------------------------------
// vector 版是真正实现，initializer_list 版只是把 initializer_list
// 构造成 vector 再调 vector 版（避免写两份一样的反向循环）。
Handler chain(Handler h, const std::vector<Middleware>& mws) {
    // 反向迭代：Chain(h, mw1, mw2) = mw1(mw2(h))
    for (auto it = mws.end(); it != mws.begin(); ) {
        --it;
        h = (*it)(std::move(h));
    }
    return h;
}

Handler chain(Handler h, std::initializer_list<Middleware> mws) {
    return chain(std::move(h), std::vector<Middleware>(mws));
}

// ---------------------------------------------------------------------------
// CORS 中间件
// ---------------------------------------------------------------------------
Middleware cors() {
    return [](Handler next) -> Handler {
        return [next](const HttpRequest& req) -> HttpResponse {
            // 预检请求：OPTIONS 且带 Origin 头 → 直接返回 204
            bool is_preflight = (req.method == "OPTIONS") &&
                                (req.headers.find("origin") != req.headers.end());
            if (is_preflight) {
                HttpResponse r;
                r.status_code = 204;
                r.status_text = "No Content";
                r.headers["Access-Control-Allow-Origin"]   = "*";
                r.headers["Access-Control-Allow-Methods"]  = "GET, POST, PUT, DELETE, OPTIONS, PATCH";
                r.headers["Access-Control-Allow-Headers"]  = "Content-Type, Authorization, X-*";
                r.headers["Access-Control-Max-Age"]       = "86400";
                return r;
            }

            // 正常请求：先转发给 next，再在响应里加 CORS 头
            auto resp = next(req);
            resp.headers["Access-Control-Allow-Origin"]   = "*";
            resp.headers["Access-Control-Allow-Methods"]  = "GET, POST, PUT, DELETE, OPTIONS, PATCH";
            resp.headers["Access-Control-Allow-Headers"]  = "Content-Type, Authorization, X-*";
            return resp;
        };
    };
}

// ---------------------------------------------------------------------------
// RequestLog 中间件
// ---------------------------------------------------------------------------
Middleware request_log() {
    return [](Handler next) -> Handler {
        return [next](const HttpRequest& req) -> HttpResponse {
            auto start = std::chrono::steady_clock::now();
            auto resp  = next(req);
            auto end   = std::chrono::steady_clock::now();

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            // 时间戳格式化
            auto t = std::time(nullptr);
            std::tm tm_buf{};
            localtime_r(&t, &tm_buf);   // POSIX 安全版本（localtime 非线程安全）
            char ts[32] = {};
            std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

            const char* ip = req.remote_ip.empty() ? "-" : req.remote_ip.c_str();
            fprintf(stderr,
                    "[%s] %s %s → %d (%lldms) [%s]\n",
                    ts, req.method.c_str(), req.path.c_str(),
                    resp.status_code,
                    static_cast<long long>(ms.count()),
                    ip);

            return resp;
        };
    };
}

// ---------------------------------------------------------------------------
// RateLimit 中间件 —— 时间窗口计数
// ---------------------------------------------------------------------------
Middleware rate_limit(int qps) {
    if (qps <= 0) {
        // 不限流 → 直接透传（no-op）
        return [](Handler next) { return next; };
    }

    // 每个 IP 一个桶：窗口起点 + 计数
    struct Bucket {
        std::chrono::steady_clock::time_point start;
        int count = 0;
    };

    // 用 shared_ptr 把 map 和 mutex 包起来，让所有 lambda 实例共享
    // 为什么不用 mutex 直接捕获？——lambda 的 operator() 是 const，
    // 要改外部 map 必须加 mutable，但 std::mutex 禁止拷贝。
    // shared_ptr 既能拷贝，又能让所有实例指向同一份数据。
    auto mu       = std::make_shared<std::mutex>();
    auto buckets  = std::make_shared<std::map<std::string, Bucket>>();
    const int limit = qps;

    return [mu, buckets, limit](Handler next) -> Handler {
        return [mu, buckets, limit, next](const HttpRequest& req) -> HttpResponse {
            // 用 remote_ip 做 key（没 remote_ip 就用 "unknown"，所有匿名请求共用一个桶）
            const std::string& ip = req.remote_ip.empty() ? std::string("unknown") : req.remote_ip;

            auto now = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lk(*mu);

            auto it = buckets->find(ip);
            if (it == buckets->end() ||
                now - it->second.start > std::chrono::seconds(1)) {
                // 新 IP 或旧窗口过期 → 新起一个桶
                (*buckets)[ip] = Bucket{now, 1};
            } else {
                it->second.count++;
            }

            bool allowed = (*buckets)[ip].count <= limit;
            lk.unlock();

            if (!allowed) {
                HttpResponse r;
                r.status_code = 429;
                r.status_text = "Too Many Requests";
                r.headers["Retry-After"] = "1";
                r.headers["Content-Type"] = "application/json";
                r.body = {'{', '"', 'e', 'r', 'r', 'o', 'r', '"', ':', '"',
                          't', 'o', 'o', ' ', 'm', 'a', 'n', 'y', ' ', 'r',
                          'e', 'q', 'u', 'e', 's', 't', 's', '"', '}'};
                return r;
            }
            return next(req);
        };
    };
}

// ---------------------------------------------------------------------------
// AuthHeader 中间件
// ---------------------------------------------------------------------------
Middleware auth_header(const std::string& header_name) {
    if (header_name.empty()) {
        return [](Handler next) { return next; };
    }

    // 注意：HTTP header 名在我们的 parser 里统一成小写了
    // 所以 check 时也要转一下
    std::string key_lower;
    key_lower.reserve(header_name.size());
    for (char c : header_name) {
        key_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return [key_lower, header_name](Handler next) -> Handler {
        return [key_lower, header_name, next](const HttpRequest& req) -> HttpResponse {
            if (req.headers.find(key_lower) == req.headers.end()) {
                HttpResponse r;
                r.status_code = 401;
                r.status_text = "Unauthorized";
                r.headers["WWW-Authenticate"] = header_name;
                r.headers["Content-Type"] = "application/json";
                r.body = {'{', '"', 'e', 'r', 'r', 'o', 'r', '"', ':', '"',
                          'm', 'i', 's', 's', 'i', 'n', 'g', ' ', 'h', 'e',
                          'a', 'd', 'e', 'r', '"', '}'};
                return r;
            }
            return next(req);
        };
    };
}

} // namespace gateway
