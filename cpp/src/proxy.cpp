// ============================================================================
// proxy.cpp —— 反向代理：把请求转给 upstream 并把响应带回来
// ----------------------------------------------------------------------------
// 流程：
//   1. 把 upstream（"127.0.0.1:9000"）拆成 host + port
//   2. 新建 TCP socket → connect
//   3. 构造发往 upstream 的 HTTP/1.1 请求（重写 path + 加 X-Forwarded-*）
//   4. serialize 这个改写后的请求 → write 到 upstream socket
//   5. 循环 read upstream socket，解析出完整 HTTP 响应
//   6. 返回 HttpResponse（server 层会把它写回客户端）
//
// 为什么要重新构造请求，而不是直接把原始 HttpRequest 发过去？
//   因为网关要做"透明改写"：
//     - Host 头改成 upstream（否则后端看到 Host: client-hostname 会晕）
//     - path 根据 strip_prefix + remainder 改写
//     - 加 X-Forwarded-*（让后端知道原始 host / 协议 / IP）
//   这是反向代理的核心——网关不是"转发字节"，而是"解析-改写-重发"。
//
// 不做流式的原因：
//   生产级网关会做双向管道（一边读 upstream 一边写 client），省内存。
//   教学版简化：把 upstream 响应整个读完再一次性写回。
// ============================================================================

#include "gateway/proxy.hpp"

// POSIX socket 头文件（WSL 下就是 Linux 的）
#include <arpa/inet.h>      // htons
#include <netdb.h>          // getaddrinfo
#include <netinet/in.h>     // sockaddr_in
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>           // strerror
#include <cstring>
#include <sstream>
#include <vector>

namespace gateway {

// ---------------------------------------------------------------------------
// 匿名命名空间：socket 辅助 + HTTP 响应解析
// ---------------------------------------------------------------------------
namespace {

// 把 "host:port" 拆成 host 和 port
bool split_host_port(const std::string& s, std::string& host, int& port) {
    size_t colon = s.find_last_of(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    try {
        port = std::stoi(s.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return true;
}

// 同步 connect 到指定 host:port，返回 fd（-1 失败）
int connect_to(const std::string& host, int port) {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;   // 既支持 IPv4 也支持 IPv6
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) return -1;

    int fd = -1;
    for (auto* rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// 循环 write 直到全部写完或出错
bool write_all(int fd, const uint8_t* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(fd, buf + done, n - done);
        if (w <= 0) return false;
        done += static_cast<size_t>(w);
    }
    return true;
}

// 循环 read 直到遇到 EAGAIN/EWOULDBLOCK 或 buffer 满
//   简单阻塞读一次也可以（对教学场景完全够）
size_t read_some(int fd, uint8_t* buf, size_t max) {
    ssize_t r = ::read(fd, buf, max);
    return (r > 0) ? static_cast<size_t>(r) : 0;
}

// ---------------------------------------------------------------------------
// 极简 HTTP 响应解析（只够我们用）
//   我们用它来把 upstream 返回的字节 → HttpResponse
//   流程和 HttpRequestParser 类似：先等 header 结束标记 \r\n\r\n
//   然后根据 Content-Length 收 body
// ---------------------------------------------------------------------------
struct HttpResponseParser {
    std::vector<uint8_t> buffer;

    // 把 n 字节喂进去，成功产出一个完整 HttpResponse 返回；
    // 还没收齐返回 nullopt；格式错误返回带 error 的 pair
    std::optional<std::pair<HttpResponse, std::string>> feed(const uint8_t* data, size_t n) {
        buffer.insert(buffer.end(), data, data + n);
        return try_parse();
    }

    std::optional<std::pair<HttpResponse, std::string>> try_parse() {
        constexpr const char* kEnd = "\r\n\r\n";
        constexpr size_t kEndLen = 4;

        // 找 header 结束标记
        size_t header_end = std::string::npos;
        for (size_t i = 0; i + kEndLen <= buffer.size(); ++i) {
            if (std::memcmp(buffer.data() + i, kEnd, kEndLen) == 0) {
                header_end = i;
                break;
            }
        }
        if (header_end == std::string::npos) return std::nullopt;

        std::string header_blob(reinterpret_cast<const char*>(buffer.data()), header_end);

        // 按 \r\n 切行
        // （这里直接写 inline 逻辑，不抽函数避免重复）
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t pos = header_blob.find("\r\n", start);
            if (pos == std::string::npos) {
                lines.push_back(header_blob.substr(start));
                break;
            }
            lines.push_back(header_blob.substr(start, pos - start));
            start = pos + 2;
        }
        if (lines.empty()) return std::make_pair(HttpResponse{}, "empty header");

        // ---- 状态行："HTTP/1.1 200 OK" ----
        HttpResponse resp;
        const std::string& status_line = lines[0];
        size_t sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) return std::make_pair(resp, "bad status line");
        size_t sp2 = status_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) {
            // 没有 reason phrase（"HTTP/1.1 200"），也接受
            try { resp.status_code = std::stoi(status_line.substr(sp1 + 1)); }
            catch (...) { return std::make_pair(resp, "bad status code"); }
            resp.status_text = "OK";
        } else {
            try { resp.status_code = std::stoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1)); }
            catch (...) { return std::make_pair(resp, "bad status code"); }
            resp.status_text = status_line.substr(sp2 + 1);
        }

        // ---- header ----
        size_t header_bytes = header_end + kEndLen;
        for (size_t i = 1; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (line.empty()) continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key   = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // value 去前导空格
            size_t vs = value.find_first_not_of(' ');
            value = (vs == std::string::npos) ? std::string() : value.substr(vs);
            resp.headers[key] = value;
        }

        // 把 header 字节从 buffer 里扔了
        buffer.erase(buffer.begin(), buffer.begin() + header_bytes);

        // ---- body ----
        // 简单处理：读 Content-Length，收齐 body
        size_t content_len = 0;
        bool has_cl = false;
        for (const auto& [k, v] : resp.headers) {
            std::string kl;
            for (char c : k) kl.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (kl == "content-length") {
                try { content_len = static_cast<size_t>(std::stoul(v)); has_cl = true; }
                catch (...) {}
                break;
            }
        }

        if (has_cl && content_len > 0) {
            if (buffer.size() < content_len) return std::nullopt;  // 等更多
            resp.body.assign(buffer.begin(), buffer.begin() + content_len);
            buffer.erase(buffer.begin(), buffer.begin() + content_len);
        }
        // 没 Content-Length？教学版不做 chunked，假设响应没 body
        // （如果真有 body 就丢掉——echo-server 我们会显式加 Content-Length）

        return std::make_pair(std::move(resp), std::string());
    }
};

// 构造发往 upstream 的 HTTP 请求（改写后的）
std::vector<uint8_t> build_upstream_request(const HttpRequest& orig_req,
                                            const MatchResult& result,
                                            const std::string& upstream_host_port,
                                            const std::string& client_ip) {
    std::vector<uint8_t> out;

    auto append = [&](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
    };

    // ---- 改写 path ----
    //   先 strip_prefix，再拼 remainder（如果有）
    std::string new_path = strip_prefix(orig_req.path, result.route);
    if (new_path.empty()) new_path = "/";

    // 如果有 query string，拼回去
    std::string full_path = new_path;
    if (!orig_req.query.empty()) {
        full_path += "?";
        full_path += orig_req.query;
    }

    // ---- 请求行 ----
    append(orig_req.method + " " + full_path + " " + orig_req.protocol + "\r\n");

    // ---- Host 头 ----
    //   把原始 Host 保存下来当 X-Forwarded-Host，然后把 Host 改成 upstream
    //   upstream_host_port 形如 "127.0.0.1:9000"（config 里就是这么写的）
    append("Host: " + upstream_host_port + "\r\n");

    // ---- 原始请求的其他 header（跳过 Host）----
    for (const auto& [k, v] : orig_req.headers) {
        if (k == "host") continue;    // Host 已经改成 upstream 的了
        if (k == "content-length") {
            // Content-Length 要按我们发出去的 body 来——body 还是原始 body，所以值不变
            append("Content-Length: " + std::to_string(orig_req.body.size()) + "\r\n");
            continue;
        }
        append(k + ": " + v + "\r\n");
    }

    // ---- 加 X-Forwarded-* ----
    append("X-Forwarded-For: " + client_ip + "\r\n");
    append("X-Forwarded-Proto: http\r\n");
    append("X-Forwarded-Host: ");
    // 原始 Host 头可能不存在，用 remote_ip 兜底
    auto it = orig_req.headers.find("host");
    if (it != orig_req.headers.end()) append(it->second);
    else                               append(orig_req.remote_ip);
    append("\r\n");

    // ---- 结尾 ----
    append("\r\n");

    // ---- body ----
    out.insert(out.end(), orig_req.body.begin(), orig_req.body.end());

    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Proxy::Forward —— 真正的转发逻辑
// ---------------------------------------------------------------------------
HttpResponse Proxy::forward(const HttpRequest& req,
                            const MatchResult& result,
                            const std::string& client_ip) {
    const std::string& upstream = result.route.upstream;

    // 拆 host:port
    std::string host;
    int port = 0;
    if (!split_host_port(upstream, host, port)) {
        return make_json_response(502, "{\"error\":\"bad upstream: " + upstream + "\"}");
    }

    // connect
    int fd = connect_to(host, port);
    if (fd < 0) {
        return make_json_response(502, "{\"error\":\"upstream connect failed\"}");
    }

    // 确保函数退出时 fd 被 close（RAII）
    struct FdGuard {
        int fd;
        ~FdGuard() { if (fd >= 0) ::close(fd); }
    } fdg{fd};

    // 构造改写后的请求并写入
    auto upstream_bytes = build_upstream_request(req, result, upstream, client_ip);
    if (!write_all(fd, upstream_bytes.data(), upstream_bytes.size())) {
        return make_json_response(502, "{\"error\":\"upstream write failed\"}");
    }

    // 循环读 upstream 的响应（socket fd 保持打开直到 upstream 关）
    HttpResponseParser rsp;
    std::vector<uint8_t> tmp(4096);
    while (true) {
        size_t n = read_some(fd, tmp.data(), tmp.size());
        if (n == 0) {
            // upstream EOF 了，但我们还没解析出完整响应 → 协议错误
            if (rsp.buffer.empty()) break;   // 实际上不会走到这里——try_parse 可能还没跑
            break;
        }
        auto r = rsp.feed(tmp.data(), n);
        if (r.has_value()) {
            if (r->second.empty()) {
                return r->first;   // 成功拿到完整响应
            } else {
                // 解析失败
                return make_json_response(502, "{\"error\":\"upstream parse failed: " + r->second + "\"}");
            }
        }
    }

    // 走到这里要么是 upstream 直接关了（没给完整 header）
    // 要么是 EOF 后我们还没产出完整 response
    return make_json_response(502, "{\"error\":\"upstream closed prematurely\"}");
}

} // namespace gateway
