// ============================================================================
// http_parser.hpp —— 手动实现的极简 HTTP/1.1 解析器
// ----------------------------------------------------------------------------
// 为什么不直接用 libcurl / cpp-httplib / Boost.Beast？
//   项目要求"零外部依赖"，而且"自己解析 HTTP"本身就是很好的教学练习。
//   HTTP/1.1 的报文格式非常规整：
//     请求： Method SP Path SP "HTTP/1.1" CRLF
//            Header: Value CRLF  × N
//            CRLF
//            Body（Content-Length 字节数）
//   自己实现一遍，对"HTTP 到底是什么"这件事会有实感。
//
// 我们只实现教学必须的子集：
//   - 支持 GET / POST / PUT / DELETE / PATCH / HEAD / OPTIONS
//   - 支持 Content-Length 长度的 body（不支持 chunked 编码）
//   - 支持 query string（? 后面的部分）
//   - header 名字大小写不敏感（HTTP 规范规定）
//
// 不做的（够用就行）：
//   - 不处理 pipelining（一个连接一个请求，够教学）
//   - 不处理连接复用（keep-alive 简单返回，不真正实现）
//   - 不支持 chunked 编码（太复杂，用 Content-Length 足够）
// ============================================================================

#pragma once

#include <cstdint>      // uint8_t
#include <map>          // header 存 std::map<std::string, std::string>
#include <optional>     // 函数返回 optional 表示"可能失败"
#include <string>       // std::string
#include <vector>       // body 用 std::vector<uint8_t>

namespace gateway {

// ---------------------------------------------------------------------------
// 通用别名：头字段（HTTP header 名字大小写不敏感，我们统一存小写）
// ---------------------------------------------------------------------------
using Headers = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------
// HttpRequest —— 解析出来的 HTTP 请求
//   method   : "GET" / "POST" / ...
//   path     : URL path 部分，如 "/api/users/123"（不含 query）
//   query    : URL query 部分，如 "q=abc&page=1"（不含 '?'）
//   protocol : "HTTP/1.1"
//   headers  : key→value（key 已被转小写）
//   body     : 原始字节（POST/PUT 的请求体）
//   remote_ip: 客户端 IP（server 层填充，parser 不负责）
// ---------------------------------------------------------------------------
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string protocol;
    Headers     headers;
    std::vector<uint8_t> body;
    std::string remote_ip;
};

// ---------------------------------------------------------------------------
// HttpResponse —— 构造出来要写回客户端的 HTTP 响应
//   status_code : 200 / 404 / 502 ...
//   status_text : "OK" / "Not Found" / "Bad Gateway" ...
//   headers     : 响应头（注意：key 不自动转小写，写什么就是什么）
//   body        : 响应体原始字节
//
// 为什么分 status_code 和 status_text？
//   HTTP/1.1 响应行格式："HTTP/1.1 200 OK"
//   我们提供两个字段，构造时更灵活；parser 里有一个辅助函数
//   StatusText(code) 根据常用 code 给默认值。
// ---------------------------------------------------------------------------
struct HttpResponse {
    int         status_code = 200;
    std::string status_text = "OK";
    Headers     headers;
    std::vector<uint8_t> body;
};

// ---------------------------------------------------------------------------
// ParserResult —— 一次解析的返回
//   成功时：request 被填充，error 为空
//   失败时：error 里是原因（如 "invalid request line"）
// ---------------------------------------------------------------------------
struct ParserResult {
    HttpRequest request;
    std::string error;  // empty = 成功
};

// ---------------------------------------------------------------------------
// HttpRequestParser —— 有状态的请求解析器
// ---------------------------------------------------------------------------
//
// 为什么"有状态"？
//   一个 HTTP 请求可能分多次 socket read 才完整到达（TCP 是流）。
//   解析器需要记住"我已经解析到哪一步了"：
//     阶段 1：等待完整的请求行 + 所有 header（遇到 CRLFCRLF）
//     阶段 2：根据 Content-Length 等待 body 收齐
//   所以设计成类，内部维护 buffer 和 state。
//
// 使用方法：
//   HttpRequestParser parser;
//   while (socket.read(buf, n) > 0) {
//       auto r = parser.feed(buf, n);
//       if (r.has_value()) { /* 完整请求 ready */ break; }
//   }
//
class HttpRequestParser {
public:
    HttpRequestParser();

    // 把 n 字节新数据喂给解析器
    //   返回：有值表示已解析出一个完整请求；无值表示还需要更多数据
    std::optional<ParserResult> feed(const uint8_t* data, size_t n);

    // 重置解析器状态（处理完一个请求后调用，准备下一个）
    void reset();

private:
    std::vector<uint8_t> buffer_;       // 累计收的所有字节
    bool header_done_;                  // header 是否收完（看到 \r\n\r\n）
    size_t content_length_;             // 从 header 里读到的 Content-Length 值
    bool have_content_length_;          // header 里有没有 Content-Length

    // 尝试从当前 buffer 里解析完整请求。成功返回 ParserResult，
    // 不完整返回 nullopt（caller 需要继续喂数据），失败返回带 error 的 result。
    std::optional<ParserResult> try_parse();

    // 把 buffer 前 consume 字节移除（解析完一批就前移）
    void consume(size_t n);
};

// ---------------------------------------------------------------------------
// 辅助函数（命名空间级别）
// ---------------------------------------------------------------------------

// 把给定的 HttpResponse 序列化成字节流，可直接 write 到 socket
std::vector<uint8_t> serialize(const HttpResponse& resp);

// 根据状态码返回常见的 status text（200→"OK"，404→"Not Found"...）
std::string status_text(int code);

// 便捷构造器：构造一个带 JSON body 的 JSON 响应
HttpResponse make_json_response(int code, const std::string& json);

// 便捷构造器：构造一个纯文本响应
HttpResponse make_text_response(int code, const std::string& text);

} // namespace gateway
