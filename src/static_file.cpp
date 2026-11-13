// =============================================================================
//  httpmech - 静态文件服务（阶段 1：缓冲模式）
// =============================================================================
//  设计参考 cpp-httplib 0.66.0 的 set_mount_point / handle_file_request /
//  check_if_not_modified / apply_ranges，但**发送链路不同**：对方是阻塞
//  socket + 每连接线程，可以在同一栈帧里循环写；本库是异步 io 线程，
//  因此这里只服务「能一次放进 ResponseWriter 缓冲」的小文件。
// =============================================================================
#include <httpmech/static_file.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <sys/stat.h>
#else
#    include <sys/stat.h>
#endif

namespace httpmech {
namespace {

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
//  路径 / 字符串
// -----------------------------------------------------------------------------

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return s;
}

fs::path to_path(std::string_view u8)
{
    // Windows 上 std::filesystem::path 的 char 构造走本地代码页，
    // u8path 才按 UTF-8 → UTF-16 转换（C++17 标准函数）。
    return fs::u8path(std::string{u8});
}

int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

bool match_mount(const std::string& mount, std::string_view path) noexcept
{
    if (path.size() < mount.size() ||
        path.substr(0, mount.size()) != std::string_view{mount}) {
        return false;
    }
    if (mount == "/") { return true; }
    // 段边界：/public 匹配 /public 与 /public/x，不匹配 /publicity/x
    return path.size() == mount.size() || path[mount.size()] == '/';
}

// -----------------------------------------------------------------------------
//  文件元数据：统一用 stat 拿 size / mtime（filesystem::last_write_time 的
//  epoch 在 C++17 未规定，HTTP-date 需要真实的 unix 秒）。
// -----------------------------------------------------------------------------

struct FileInfo {
    bool          exists = false;
    bool          is_reg = false;
    bool          is_dir = false;
    std::uint64_t size   = 0;
    std::int64_t  mtime  = 0;
};

bool stat_file(const fs::path& p, FileInfo& out) noexcept
{
#if defined(_WIN32)
    struct _stat64 st{};
    if (::_wstat64(p.c_str(), &st) != 0) { return false; }
    out.is_dir = (st.st_mode & _S_IFMT) == _S_IFDIR;
    out.is_reg = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) { return false; }
    out.is_dir = S_ISDIR(st.st_mode);
    out.is_reg = S_ISREG(st.st_mode);
#endif
    out.exists = true;
    out.size   = static_cast<std::uint64_t>(st.st_size);
    out.mtime  = static_cast<std::int64_t>(st.st_mtime);
    return true;
}

/// 与 ServerSession::kProviderChunk 同量级：每轮一次 read + 一次投递。
/// 文件不再整块读入内存，内存占用与文件大小无关。
constexpr std::size_t kFileChunk = 256 * 1024;

/// 多段 Range 的段数上限（RFC 9110 §14.2 的 DoS 防护，cpp-httplib 用 1024）。
constexpr std::size_t kMaxRanges = 32;

// -----------------------------------------------------------------------------
//  MIME
// -----------------------------------------------------------------------------

struct MimeEntry { const char* ext; const char* type; };

constexpr MimeEntry kMimeTypes[] = {
    {"css",   "text/css"},          {"csv",   "text/csv"},
    {"htm",   "text/html"},         {"html",  "text/html"},
    {"js",    "text/javascript"},   {"mjs",   "text/javascript"},
    {"txt",   "text/plain"},        {"vtt",   "text/vtt"},
    {"json",  "application/json"},  {"xml",   "application/xml"},
    {"pdf",   "application/pdf"},   {"wasm",  "application/wasm"},
    {"zip",   "application/zip"},   {"gz",    "application/gzip"},
    {"apng",  "image/apng"},        {"avif",  "image/avif"},
    {"bmp",   "image/bmp"},         {"gif",   "image/gif"},
    {"ico",   "image/x-icon"},      {"jpeg",  "image/jpeg"},
    {"jpg",   "image/jpeg"},        {"png",   "image/png"},
    {"svg",   "image/svg+xml"},     {"tif",   "image/tiff"},
    {"tiff",  "image/tiff"},        {"webp",  "image/webp"},
    {"mp3",   "audio/mp3"},         {"wav",   "audio/wave"},
    {"mp4",   "video/mp4"},         {"mpeg",  "video/mpeg"},
    {"webm",  "video/webm"},
    {"otf",   "font/otf"},          {"ttf",   "font/ttf"},
    {"woff",  "font/woff"},         {"woff2", "font/woff2"},
};

// -----------------------------------------------------------------------------
//  HTTP 日期 / ETag
// -----------------------------------------------------------------------------

std::string http_date(std::int64_t unix_seconds)
{
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    if (::gmtime_s(&tm, &t) != 0) { return {}; }
#else
    if (::gmtime_r(&t, &tm) == nullptr) { return {}; }
#endif
    char buf[64]{};
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return {};
    }
    return buf;
}

// 把公历日期换算成 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil）。
// 不用 timegm / _mkgmtime，避免平台与 feature macro 差异。
std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept
{
    y -= (m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp  = static_cast<unsigned>(static_cast<int>(m) + (m > 2 ? -3 : 9));
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int month_index(const char* s) noexcept
{
    static const char* const names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(s, names[i], 3) == 0) { return i; }
    }
    return -1;
}

/// 只解析 IMF-fixdate（我们自己发出的格式）；其它两种历史格式忽略即可 ——
/// 顶多少回一次 304，不影响正确性。
bool parse_http_date(std::string_view s, std::int64_t& out)
{
    if (s.size() < 29) { return false; }
    const std::string text{s};
    char wday[8]{};
    char mon[8]{};
    int  day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(text.c_str(), "%7s %d %3s %d %d:%d:%d GMT",
                    wday, &day, mon, &year, &hh, &mm, &ss) != 7) {
        return false;
    }
    const int mi = month_index(mon);
    if (mi < 0 || day < 1 || day > 31 || year < 1970 || year > 9999 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
        return false;
    }
    const auto days = days_from_civil(year, static_cast<unsigned>(mi + 1),
                                      static_cast<unsigned>(day));
    out = days * 86400 + hh * 3600 + mm * 60 + ss;
    return true;
}

std::string compute_etag(const FileInfo& fi)
{
    // 弱 ETag：mtime + size 已经足够区分版本；压缩表示若将来引入，
    // 必须把编码名拼进 suffix（cpp-httplib 的做法）。
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "W/\"%llx-%llx\"",
                  static_cast<unsigned long long>(fi.mtime),
                  static_cast<unsigned long long>(fi.size));
    return buf;
}

// -----------------------------------------------------------------------------
//  条件请求 / Range
// -----------------------------------------------------------------------------

bool not_modified(const HttpRequest& req, const std::string& etag, std::int64_t mtime)
{
    const auto inm = req.header("If-None-Match");
    if (!inm.empty()) {
        // RFC 9110 §13.1.2：If-None-Match 优先于 If-Modified-Since
        if (etag.empty()) { return false; }
        std::size_t pos = 0;
        while (pos <= inm.size()) {
            const auto comma = inm.find(',', pos);
            const auto part  = trim_ows(inm.substr(
                pos, comma == std::string_view::npos ? comma : comma - pos));
            if (part == "*" || part == std::string_view{etag}) { return true; }
            if (comma == std::string_view::npos) { break; }
            pos = comma + 1;
        }
        return false;
    }
    const auto ims = req.header("If-Modified-Since");
    if (!ims.empty()) {
        std::int64_t t = 0;
        if (parse_http_date(ims, t) && mtime <= t) { return true; }
    }
    return false;
}

bool if_range_allows(const HttpRequest& req, const std::string& etag, std::int64_t mtime)
{
    const auto v = trim_ows(req.header("If-Range"));
    if (v.empty()) { return true; }

    // RFC 9110 §13.1.5：If-Range 只接受强 ETag 或 HTTP-date。本处理器只产生
    // 弱 ETag（W/"..."），严格按规范它不能用于 If-Range；但主流客户端会原样
    // 回传，完全拒绝会让 Range 静默退化成 200。折中：精确匹配我们发出的那个
    // 值就接受，其它 ETag 形式一律不匹配，非 ETag 形式按 HTTP-date 比较。
    const bool looks_like_etag = v.front() == '"' || v.front() == 'W' || v.front() == 'w';
    if (looks_like_etag) {
        return !etag.empty() && v == std::string_view{etag};
    }
    std::int64_t t = 0;
    if (parse_http_date(v, t) && mtime <= t) { return true; }
    return false;   // 校验器不匹配：忽略 Range，回整文件
}

enum class RangeParse { ignore, invalid, ok };

struct RangeSpec {
    std::uint64_t first = 0;
    std::uint64_t last  = 0;
};

bool parse_u64(std::string_view s, std::uint64_t& out) noexcept
{
    if (s.empty()) { return false; }
    for (const char c : s) {
        if (c < '0' || c > '9') { return false; }
    }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool parse_one_range(std::string_view spec, std::uint64_t size,
                     RangeSpec& out) noexcept
{
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) { return false; }
    const auto lhs = spec.substr(0, dash);
    const auto rhs = spec.substr(dash + 1);

    if (lhs.empty()) {
        std::uint64_t suffix = 0;
        if (!parse_u64(rhs, suffix) || suffix == 0) { return false; }
        out.first = (suffix >= size) ? 0 : (size - suffix);
        out.last  = size - 1;
        return true;
    }

    std::uint64_t first = 0;
    if (!parse_u64(lhs, first)) { return false; }
    std::uint64_t last = size - 1;
    if (!rhs.empty() && !parse_u64(rhs, last)) { return false; }
    if (first >= size || first > last) { return false; }
    if (last >= size) { last = size - 1; }
    out.first = first;
    out.last  = last;
    return true;
}

/// 多段 Range。其它 unit 按 RFC 9110 忽略；语法错 / 越界 / 段数超限 → 416。
RangeParse parse_range(std::string_view v, std::uint64_t size,
                       std::vector<RangeSpec>& out) noexcept
{
    out.clear();
    const auto eq = v.find('=');
    if (eq == std::string_view::npos) { return RangeParse::invalid; }
    if (!iequals(v.substr(0, eq), "bytes")) { return RangeParse::ignore; }
    if (size == 0) { return RangeParse::ignore; }

    const auto spec = v.substr(eq + 1);
    std::size_t pos = 0;
    for (;;) {
        const auto comma = spec.find(',', pos);
        const auto part = trim_ows(spec.substr(
            pos, comma == std::string_view::npos ? comma : comma - pos));
        RangeSpec rs{};
        if (!parse_one_range(part, size, rs)) { return RangeParse::invalid; }
        out.push_back(rs);
        if (out.size() > kMaxRanges) { return RangeParse::invalid; }   // DoS 防护
        if (comma == std::string_view::npos) { break; }
        pos = comma + 1;
    }
    return out.empty() ? RangeParse::invalid : RangeParse::ok;
}

// --- multipart/byteranges 的流式拼装（阶段 4）---

struct MultipartPart {
    std::string   prefix;       // 段头：--boundary + Content-Type + Content-Range
    std::uint64_t file_offset = 0;
    std::uint64_t data_bytes  = 0;
    std::string   trailer;      // 段尾 "\r\n"，让下一个 boundary 另起一行
};

struct MultipartState {
    std::size_t   idx       = 0;
    int           phase     = 0;   // 0=prefix, 1=data, 2=trailer
    std::uint64_t data_sent = 0;
};

std::string make_multipart_boundary(const FileInfo& fi)
{
    return "httpmech-" + std::to_string(fi.mtime) + "-" + std::to_string(fi.size);
}

// -----------------------------------------------------------------------------
//  其它
// -----------------------------------------------------------------------------

std::string content_type_for(const fs::path& file, const StaticFileConfig& cfg)
{
    std::string ext = to_lower(file.extension().string());
    if (!ext.empty() && ext.front() == '.') { ext.erase(0, 1); }
    if (!ext.empty()) {
        const auto it = cfg.mime_types.find(ext);
        if (it != cfg.mime_types.end()) { return it->second; }
        const auto builtin = mime_type_for_extension(ext);
        if (!builtin.empty()) { return std::string{builtin}; }
    }
    return cfg.default_content_type;
}

bool within_base(const fs::path& resolved, const std::string& base_lower_slash)
{
    std::string s = to_lower(resolved.generic_string());
    if (s.empty()) { return false; }
    if (s.back() != '/') { s.push_back('/'); }
    // base 也带尾 '/'：既保证路径段边界（/www 不会匹配 /www2），
    // 也让 root 本身（目录）通过。
    return s.size() >= base_lower_slash.size() &&
           s.compare(0, base_lower_slash.size(), base_lower_slash) == 0;
}

void end_plain(ResponseWriter& out, Status code, std::string_view text)
{
    out.set_status(code);
    out.set_content_type("text/plain; charset=utf-8");
    out.set_header("Content-Length", std::to_string(text.size()));
    if (!text.empty()) { out.write(text); }
    out.end();
}

} // namespace

// =============================================================================
//  工具函数
// =============================================================================

bool percent_decode_path(std::string_view in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) { return false; }
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) { return false; }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

bool is_safe_url_path(std::string_view path) noexcept
{
    if (path.empty() || path.front() != '/') { return false; }
    std::size_t beg = 1;
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c == 0 || c == '\\' || c < 0x20 || c == 0x7f) { return false; }
#if defined(_WIN32)
            if (c == ':') { return false; }   // NTFS 流 a.txt::$DATA
#endif
            continue;
        }
        const std::size_t len = i - beg;
        if ((len == 0 && i < path.size()) ||
            (len > 0 && (path[i - 1] == '.' || path[i - 1] == ' '))) {
            return false;
        }
        beg = i + 1;
    }
    return true;
}

std::string_view mime_type_for_extension(std::string_view ext) noexcept
{
    if (!ext.empty() && ext.front() == '.') { ext.remove_prefix(1); }
    for (const auto& e : kMimeTypes) {
        if (iequals(ext, e.ext)) { return e.type; }
    }
    return {};
}

// =============================================================================
//  处理器
// =============================================================================

struct StaticFileHandler::Impl {
    StaticFileConfig cfg;
    std::string      mount;
    fs::path         root;
    fs::path         resolved_root;
    std::string      resolved_base;   // 小写 + 尾 '/'
    std::string      resolved_root_text;
    std::string      error;
    bool             valid = false;
};

StaticFileHandler::StaticFileHandler(StaticFileConfig cfg)
    : m_impl(std::make_unique<Impl>())
{
    Impl& d = *m_impl;
    d.cfg = std::move(cfg);

    if (d.cfg.mount_point.empty() || d.cfg.mount_point.front() != '/') {
        d.error = "mount_point must start with '/'";
        return;
    }
    d.mount = d.cfg.mount_point;
    while (d.mount.size() > 1 && d.mount.back() == '/') { d.mount.pop_back(); }

    if (d.cfg.root.empty()) {
        d.error = "root is empty";
        return;
    }
    std::error_code ec;
    d.root = to_path(d.cfg.root);
    d.resolved_root = fs::weakly_canonical(d.root, ec);
    if (ec || !fs::is_directory(d.resolved_root, ec) || ec) {
        d.error = "root is not an existing directory: " + d.cfg.root;
        return;
    }
    d.resolved_root_text = d.resolved_root.generic_string();
    d.resolved_base = to_lower(d.resolved_root_text);
    if (d.resolved_base.empty() || d.resolved_base.back() != '/') {
        d.resolved_base.push_back('/');
    }
    d.valid = true;
}

StaticFileHandler::~StaticFileHandler() = default;

bool StaticFileHandler::valid() const noexcept { return m_impl->valid; }
const std::string& StaticFileHandler::error_message() const noexcept { return m_impl->error; }
const std::string& StaticFileHandler::resolved_root() const noexcept { return m_impl->resolved_root_text; }
const StaticFileConfig& StaticFileHandler::config() const noexcept { return m_impl->cfg; }

void StaticFileHandler::on_request(HttpRequest& req, ResponseWriter& out)
{
    serve(req, out);
}

void StaticFileHandler::serve(const HttpRequest& req, ResponseWriter& out)
{
    Impl& d = *m_impl;

    if (!d.valid) {
        end_plain(out, Status::internal_error, "static file: " + d.error);
        return;
    }
    if (req.method != Method::get && req.method != Method::head) {
        out.set_status(Status::method_not_allowed);
        out.set_header("Allow", "GET, HEAD");
        out.end();
        return;
    }
    const std::string_view url_path{req.path};
    if (!match_mount(d.mount, url_path)) {
        end_plain(out, Status::not_found, "not found");
        return;
    }

    std::string decoded;
    if (!percent_decode_path(url_path, decoded) || !is_safe_url_path(decoded)) {
        end_plain(out, Status::forbidden, "forbidden");
        return;
    }

    std::string sub = decoded.substr(d.mount.size());
    if (sub.empty() || sub.front() != '/') { sub.insert(sub.begin(), '/'); }
    if (sub.back() == '/') { sub += d.cfg.index_file; }

    fs::path file = d.resolved_root / to_path(std::string_view{sub}.substr(1));

    // 解析真实路径并确认仍在 root 内（挡 symlink / junction 逃逸）。
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(file, ec);
    if (ec || !within_base(resolved, d.resolved_base)) {
        end_plain(out, Status::forbidden, "forbidden");
        return;
    }

    FileInfo fi{};
    if (!stat_file(resolved, fi) || !fi.is_reg) {
        if (fi.is_dir && d.cfg.redirect_directory &&
            !url_path.empty() && url_path.back() != '/') {
            out.set_status(Status::moved_permanently);
            out.set_header("Location", std::string{url_path} + "/");
            out.end();
            return;
        }
        end_plain(out, Status::not_found, "not found");
        return;
    }

    // --- 先做只依赖元数据的判定：304 / 416 / 超限，都不必读文件 ---
    const std::string etag = d.cfg.enable_etag ? compute_etag(fi) : std::string{};
    const std::string lm   = d.cfg.enable_etag ? http_date(fi.mtime) : std::string{};
    if (d.cfg.enable_etag && not_modified(req, etag, fi.mtime)) {
        // 304 由 finish_writer 抑制 Content-Length 与 body；保留校验器头
        out.set_status(Status::not_modified);
        if (!etag.empty()) { out.set_header("ETag", etag); }
        if (!lm.empty())   { out.set_header("Last-Modified", lm); }
        out.end();
        return;
    }

    std::uint64_t offset = 0;
    std::uint64_t length = fi.size;
    bool          partial   = false;
    bool          multipart = false;
    std::string   content_range;
    std::string   multipart_content_type;
    std::vector<MultipartPart> parts;
    std::uint64_t multipart_length = 0;

    if (d.cfg.enable_range) {
        const auto rv = req.header("Range");
        if (!rv.empty() && if_range_allows(req, etag, fi.mtime)) {
            std::vector<RangeSpec> ranges;
            const auto pr = parse_range(rv, fi.size, ranges);
            if (pr == RangeParse::invalid) {
                out.set_status(Status::range_not_satisfiable);
                out.set_header("Content-Range", "bytes */" + std::to_string(fi.size));
                out.set_header("Content-Length", "0");
                out.end();
                return;
            }
            if (pr == RangeParse::ok && ranges.size() == 1) {
                offset  = ranges[0].first;
                length  = ranges[0].last - ranges[0].first + 1;
                partial = true;
                content_range = "bytes " + std::to_string(ranges[0].first) + "-" +
                                std::to_string(ranges[0].last) + "/" +
                                std::to_string(fi.size);
            } else if (pr == RangeParse::ok && ranges.size() > 1) {
                // multipart/byteranges：每段前置头 + 文件数据 + 段尾 CRLF，
                // 最后以 --boundary-- 收尾。总长度必须先算准（Content-Length）。
                multipart = true;
                partial   = true;
                const auto boundary = make_multipart_boundary(fi);
                const auto ct = content_type_for(resolved, d.cfg);
                for (const auto& r : ranges) {
                    MultipartPart p;
                    p.prefix = "--" + boundary + "\r\nContent-Type: " + ct +
                               "\r\nContent-Range: bytes " +
                               std::to_string(r.first) + "-" +
                               std::to_string(r.last) + "/" +
                               std::to_string(fi.size) + "\r\n\r\n";
                    p.file_offset = r.first;
                    p.data_bytes  = r.last - r.first + 1;
                    p.trailer     = "\r\n";
                    multipart_length += p.prefix.size() + p.data_bytes + p.trailer.size();
                    parts.push_back(std::move(p));
                }
                MultipartPart fin;
                fin.prefix = "--" + boundary + "--\r\n";
                multipart_length += fin.prefix.size();
                parts.push_back(std::move(fin));

                length = multipart_length;
                multipart_content_type = "multipart/byteranges; boundary=" + boundary;
            }
        }
    }

    // 阶段 3：max_file_bytes 从「硬限制」变成「可选上限」。0 = 不限制；
    // 非零时拒绝超大文件，但不再回 501（大文件走异步响应体通道）。
    if (d.cfg.max_file_bytes != 0 && fi.size > d.cfg.max_file_bytes) {
        end_plain(out, Status::forbidden, "static file: file too large");
        return;
    }

    // --- 所有头必须在第一次 write() / set_body_provider() 之前设完：
    //     头一旦发出，set_status() / set_header() 会被静默忽略（a00718d）---
    out.set_status(partial ? Status::partial_content : Status::ok);
    if (multipart) {
        out.set_header("Content-Type", multipart_content_type);
    } else {
        out.set_content_type(content_type_for(resolved, d.cfg));
    }
    out.set_header("Accept-Ranges", d.cfg.enable_range ? "bytes" : "none");
    if (d.cfg.enable_etag) {
        if (!etag.empty()) { out.set_header("ETag", etag); }
        if (!lm.empty())   { out.set_header("Last-Modified", lm); }
    }
    // 多段时每段自带 Content-Range，顶层不能再带一个（RFC 9110 §14.4）。
    if (partial && !multipart) { out.set_header("Content-Range", content_range); }
    for (std::size_t i = 0; i < d.cfg.extra_headers.size(); ++i) {
        out.set_header(d.cfg.extra_headers.name_at(i),
                       d.cfg.extra_headers.value_at(i));
    }
    out.set_header("Content-Length", std::to_string(length));

    if (req.method == Method::head) {
        // 只报长度：注册 provider 让 finish_writer 拿到正确的 Content-Length，
        // 但 HEAD 不启动状态机，也不会打开/读取文件。
        out.set_body_provider(BodyProviderSpec{
            length, [](std::uint64_t, std::size_t, DataSink&) { return false; }});
        out.end();
        return;
    }

    // 注意：上面已有 `fs::path file`，这里用 stream 避免遮蔽。
    auto stream = std::make_shared<std::ifstream>(resolved, std::ios::binary);
    if (!stream->is_open()) {
        end_plain(out, Status::internal_error, "static file: open failed");
        return;
    }
    auto buf = std::make_shared<std::string>();
    buf->resize(kFileChunk);
    if (multipart) {
        auto parts_ptr = std::make_shared<std::vector<MultipartPart>>(std::move(parts));
        auto state     = std::make_shared<MultipartState>();
        out.set_body_provider(BodyProviderSpec{
            length,
            [stream, buf, parts_ptr, state](std::uint64_t /*off*/,
                                            std::size_t len, DataSink& sink) {
                for (;;) {
                    if (state->idx >= parts_ptr->size()) { return true; }
                    const auto& p = (*parts_ptr)[state->idx];

                    if (state->phase == 0) {
                        state->phase = 1;
                        if (!p.prefix.empty()) {
                            return sink.write(span<const std::uint8_t>{
                                reinterpret_cast<const std::uint8_t*>(p.prefix.data()),
                                p.prefix.size()});
                        }
                    }
                    if (state->phase == 1 && state->data_sent < p.data_bytes) {
                        const auto want = (std::min)(
                            static_cast<std::uint64_t>(len),
                            static_cast<std::uint64_t>(buf->size()));
                        const auto n = static_cast<std::size_t>((std::min)(
                            want, p.data_bytes - state->data_sent));
                        if (n == 0) { return false; }
                        stream->clear();
                        stream->seekg(static_cast<std::streamoff>(
                                          p.file_offset + state->data_sent),
                                      std::ios::beg);
                        if (!stream->good()) { return false; }
                        stream->read(&(*buf)[0], static_cast<std::streamsize>(n));
                        const auto got = static_cast<std::size_t>(stream->gcount());
                        if (got == 0) { return false; }
                        state->data_sent += got;
                        return sink.write(span<const std::uint8_t>{
                            reinterpret_cast<const std::uint8_t*>(buf->data()), got});
                    }
                    if (state->phase == 1) { state->phase = 2; }
                    if (state->phase == 2) {
                        state->phase = 3;
                        if (!p.trailer.empty()) {
                            return sink.write(span<const std::uint8_t>{
                                reinterpret_cast<const std::uint8_t*>(p.trailer.data()),
                                p.trailer.size()});
                        }
                    }
                    ++state->idx;
                    state->phase     = 0;
                    state->data_sent = 0;
                }
            }});
        out.end();
        return;
    }

    const std::uint64_t file_offset = offset;   // Range 起点

    out.set_body_provider(BodyProviderSpec{
        length,
        [stream, buf, file_offset](std::uint64_t off, std::size_t len,
                                   DataSink& sink) {
            const auto n = static_cast<std::size_t>(
                (std::min)(static_cast<std::uint64_t>(len),
                           static_cast<std::uint64_t>(buf->size())));
            if (n == 0) { return true; }
            stream->clear();   // 上一轮可能置了 eofbit
            stream->seekg(static_cast<std::streamoff>(file_offset + off),
                          std::ios::beg);
            if (!stream->good()) { return false; }
            stream->read(&(*buf)[0], static_cast<std::streamsize>(n));
            const auto got = static_cast<std::size_t>(stream->gcount());
            if (got == 0) { return false; }   // 文件被截断：宁可断连也不发短 body
            return sink.write(span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(buf->data()), got});
        }});
    out.end();
}

std::shared_ptr<StaticFileHandler> mount_static(const std::shared_ptr<Router>& router,
                                                StaticFileConfig cfg)
{
    auto handler = std::make_shared<StaticFileHandler>(std::move(cfg));
    if (router) {
        router->set_not_found(
            [handler](const HttpRequest& req, ResponseWriter& out) {
                handler->serve(req, out);
            });
    }
    return handler;
}

} // namespace httpmech
