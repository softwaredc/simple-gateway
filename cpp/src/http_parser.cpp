// ============================================================================
// http_parser.cpp —— HTTP/1.1 解析器实现（干净版）
// ----------------------------------------------------------------------------
// 流程：
//   feed() 追加新字节到内部 buffer，再调用 try_parse() 尝试产出一个
//   完整的 HttpRequest。
//
//   try_parse() 分两步：
//     Step 1. 在 buffer 里搜索 \r\n\r\n（header 结束标记）
//             没找到 → 等更多数据，返回 nullopt
//             找到了 → 解析请求行 + header，读 Content-Length
//     Step 2. 如果有 Content-Length，看 buffer 里 body 字节够不够
//             不够 → 等更多数据
//             够   → 拼出完整 HttpRequest 返回
//
//   用 buffer_.erase() 前移已消费字节，保证每次 try_parse 都是
//   "从剩余数据重新走一遍"——简单可靠，教学版不追求零拷贝。
// ============================================================================

#include "gateway/http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace gateway {

// ---------------------------------------------------------------------------
// 匿名命名空间里放内部辅助函数（不对外暴露）
// ---------------------------------------------------------------------------
namespace {

// 把字符串按 \r\n 切成多行
std::vector<std::string> split_crlf(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find("\r\n", start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 2;
    }
    return out;
}

// 把字符串转成小写（ASCII 够用）
std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 解析 HTTP header（已经去掉末尾 CRLFCRLF 的完整 blob）
//   成功：req.method/path/query/protocol/headers 被填充，返回 true
//   失败：返回 false
bool parse_header_blob(const std::string& blob, HttpRequest& req) {
    auto lines = split_crlf(blob);
    if (lines.empty()) return false;

    // ---- 请求行："GET /path?q=x HTTP/1.1" ----
    const std::string& reqline = lines[0];
    size_t sp1 = reqline.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = reqline.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    req.method   = reqline.substr(0, sp1);
    std::string full_path = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    req.protocol = reqline.substr(sp2 + 1);
    if (req.method.empty() || full_path.empty() || req.protocol.empty())
        return false;

    // path / query 拆分
    size_t qpos = full_path.find('?');
    if (qpos == std::string::npos) {
        req.path = full_path;
    } else {
        req.path  = full_path.substr(0, qpos);
        req.query = full_path.substr(qpos + 1);
    }

    // ---- 后续：header ----
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;

        size_t colon = line.find(':');
        if (colon == std::string::npos) return false;

        std::string key   = to_lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);

        // value 去前导空格（HTTP 允许 ": " 空格）
        size_t vs = value.find_first_not_of(' ');
        value = (vs == std::string::npos) ? std::string() : value.substr(vs);

        req.headers[key] = value;
    }
    return true;
}

// 从 header 里读 Content-Length；没读到或非法返回 0
size_t read_content_length(const HttpRequest& req) {
    auto it = req.headers.find("content-length");
    if (it == req.headers.end()) return 0;
    try { return static_cast<size_t>(std::stoul(it->second)); }
    catch (...) { return 0; }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HttpRequestParser
// ---------------------------------------------------------------------------

HttpRequestParser::HttpRequestParser()
    : header_done_(false), content_length_(0), have_content_length_(false) {}

void HttpRequestParser::reset() {
    buffer_.clear();
    header_done_ = false;
    content_length_ = 0;
    have_content_length_ = false;
}

std::optional<ParserResult> HttpRequestParser::feed(const uint8_t* data, size_t n) {
    buffer_.insert(buffer_.end(), data, data + n);
    return try_parse();
}

void HttpRequestParser::consume(size_t n) {
    if (n >= buffer_.size()) {
        buffer_.clear();
    } else {
        buffer_.erase(buffer_.begin(), buffer_.begin() + n);
    }
}

std::optional<ParserResult> HttpRequestParser::try_parse() {
    ParserResult result;

    // ---- Step 1：找 header 结束标记 ----
    constexpr const char* kEnd = "\r\n\r\n";
    constexpr size_t kEndLen = 4;
    const uint8_t* data = buffer_.data();
    const size_t   size = buffer_.size();

    size_t header_end = std::string::npos;
    for (size_t i = 0; i + kEndLen <= size; ++i) {
        if (std::memcmp(data + i, kEnd, kEndLen) == 0) {
            header_end = i;
            break;
        }
    }

    if (header_end == std::string::npos) {
        // header 还没收齐，等更多数据
        return std::nullopt;
    }

    // header_end 之前就是 header blob
    std::string header_blob(reinterpret_cast<const char*>(data), header_end);
    if (!parse_header_blob(header_blob, result.request)) {
        result.error = "malformed HTTP header";
        return result;   // 解析失败，返回带 error 的 result
    }

    // 从 header 里读 Content-Length
    content_length_      = read_content_length(result.request);
    have_content_length_ = content_length_ > 0;

    // 把 header + CRLFCRLF 全部从 buffer 里扔掉
    size_t header_bytes = header_end + kEndLen;
    consume(header_bytes);

    // ---- Step 2：等 body ----
    if (!have_content_length_) {
        // 没有 Content-Length（如 GET 请求），直接返回——请求完整了
        return result;
    }

    // 有 body：看 buffer 里够不够 content_length_ 字节
    if (buffer_.size() < content_length_) {
        // 不够，等更多数据
        return std::nullopt;
    }

    // body 收齐了：拷出来
    result.request.body.assign(buffer_.begin(), buffer_.begin() + content_length_);
    consume(content_length_);

    return result;
}

// ---------------------------------------------------------------------------
// 序列化 / 状态码辅助
// ---------------------------------------------------------------------------

std::string status_text(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

std::vector<uint8_t> serialize(const HttpResponse& resp) {
    std::vector<uint8_t> out;

    // ---- 状态行："HTTP/1.1 200 OK\r\n" ----
    auto append = [&](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
    };
    std::ostringstream line;
    line << "HTTP/1.1 " << resp.status_code << " "
         << (resp.status_text.empty() ? status_text(resp.status_code) : resp.status_text)
         << "\r\n";
    append(line.str());

    // ---- 响应头 ----
    bool has_content_length = false;
    for (const auto& [k, v] : resp.headers) {
        if (to_lower(k) == "content-length") has_content_length = true;
        append(k);
        append(": ");
        append(v);
        append("\r\n");
    }

    // 如果没有显式 Content-Length 且有 body，自动补一个
    if (!has_content_length && !resp.body.empty()) {
        append("Content-Length: ");
        append(std::to_string(resp.body.size()));
        append("\r\n");
        has_content_length = true;   // 标记已补过，下面的"没有 body 也补"不会再进
    }
    // 如果没有任何 Content-Length，补一个 0（让 curl/keep-alive 更可靠）
    if (!has_content_length) {
        append("Content-Length: 0\r\n");
    }

    // 结尾空行
    append("\r\n");

    // ---- body ----
    out.insert(out.end(), resp.body.begin(), resp.body.end());

    return out;
}

HttpResponse make_json_response(int code, const std::string& json) {
    HttpResponse r;
    r.status_code = code;
    r.status_text = status_text(code);
    r.headers["Content-Type"] = "application/json; charset=utf-8";
    r.body.assign(json.begin(), json.end());
    return r;
}

HttpResponse make_text_response(int code, const std::string& text) {
    HttpResponse r;
    r.status_code = code;
    r.status_text = status_text(code);
    r.headers["Content-Type"] = "text/plain; charset=utf-8";
    r.body.assign(text.begin(), text.end());
    return r;
}

} // namespace gateway
