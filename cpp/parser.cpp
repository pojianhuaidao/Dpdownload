// parser.cpp - DPlayer 配置解析、URL 处理、文件名工具
#include "parser.h"
#include "http.h"
#include <set>
#include <mutex>
#include <cstring>

// ------------------------- 迷你 JSON 解析 -------------------------
struct JVal {
    int type = 0;                       // 0 null 1 bool 2 num 3 str 4 arr 5 obj
    std::string str;                    // type==3 时的值
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* find(const std::string& key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    explicit JParser(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                                s[i] == '\n' || s[i] == '\xef'))
            ++i;
    }

    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i];
            if (c == '\\') {
                if (i + 1 >= s.size()) return false;
                char e = s[i + 1];
                i += 2;
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (i + 4 > s.size()) return false;
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        i += 4;
                        // UTF-8 编码（仅处理 BMP 基本平面）
                        if (cp < 0x80) {
                            out.push_back((char)cp);
                        } else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else if (c == '"') {
                ++i;
                return true;
            } else {
                out.push_back(c);
                ++i;
            }
        }
        return false;
    }

    bool value(JVal& out, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"') {
            out.type = 3;
            return str(out.str);
        }
        if (c == '{') {
            out.type = 5;
            ++i;
            for (;;) {
                ws();
                if (i >= s.size()) return false;
                if (s[i] == '}') { ++i; return true; }
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                JVal v;
                if (!value(v, depth + 1)) return false;
                out.obj.emplace_back(key, std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.type = 4;
            ++i;
            for (;;) {
                ws();
                if (i >= s.size()) return false;
                if (s[i] == ']') { ++i; return true; }
                JVal v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        // 数字 / true / false / null
        size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t')
            ++i;
        if (i == start) return false;
        std::string tok = s.substr(start, i - start);
        if (tok == "null") { out.type = 0; return true; }
        if (tok == "true" || tok == "false") { out.type = 1; out.str = tok; return true; }
        out.type = 2;
        out.str = tok;
        return true;
    }
};

static bool jsonParse(const std::string& text, JVal& out) {
    JParser p(text);
    if (!p.value(out)) return false;
    return true;
}

// ------------------------- HTML 实体 -------------------------
std::string htmlUnescape(const std::string& s) {
    std::string r = s;
    auto rep = [&](const char* from, const char* to) {
        size_t pos = 0;
        size_t fl = strlen(from);
        while ((pos = r.find(from, pos)) != std::string::npos) {
            r.replace(pos, fl, to);
            pos += strlen(to);
        }
    };
    rep("&quot;", "\"");
    rep("&amp;", "&");
    rep("&#39;", "'");
    rep("&apos;", "'");
    rep("&lt;", "<");
    rep("&gt;", ">");
    rep("&nbsp;", " ");
    return r;
}

// ------------------------- 抓取页面 -------------------------
bool getPageHtml(const std::string& pageUrl, std::string& html, std::string& err) {
    return httpGetText(pageUrl, pageUrl, html, err, 15000);
}

// ------------------------- 提取标题 -------------------------
std::string extractPageTitle(const std::string& html) {
    std::string low = lowerAscii(html);
    size_t p = low.find("<title");
    if (p == std::string::npos) return std::string();
    size_t gt = low.find('>', p);
    if (gt == std::string::npos) return std::string();
    size_t end = low.find("</title>", gt);
    if (end == std::string::npos) return std::string();
    std::string t = htmlUnescape(html.substr(gt + 1, end - gt - 1));
    // 折叠空白
    std::string out;
    bool lastSpace = false;
    for (char c : t) {
        bool sp = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (sp) {
            if (!lastSpace && !out.empty()) out.push_back(' ');
            lastSpace = true;
        } else {
            out.push_back(c);
            lastSpace = false;
        }
    }
    return trimStr(out);
}

// ------------------------- data-config -------------------------
std::vector<VideoConf> parseDataConfig(const std::string& html) {
    std::vector<VideoConf> out;
    std::set<std::string> seen;
    std::string low = lowerAscii(html);
    size_t pos = 0;
    while (pos < low.size()) {
        size_t hit = low.find("data-config", pos);
        if (hit == std::string::npos) break;
        size_t p = hit + 11;
        while (p < html.size() && (unsigned char)html[p] <= 0x20) ++p;
        if (p >= html.size() || html[p] != '=') { pos = hit + 11; continue; }
        ++p;
        while (p < html.size() && (unsigned char)html[p] <= 0x20) ++p;
        if (p >= html.size()) break;
        char q = html[p];
        if (q != '\'' && q != '"') { pos = p; continue; }
        ++p;
        size_t e = html.find(q, p);
        if (e == std::string::npos) break;
        std::string raw = htmlUnescape(html.substr(p, e - p));
        pos = e + 1;

        JVal v;
        if (!jsonParse(trimStr(raw), v) || v.type != 5) continue;
        const JVal* video = v.find("video");
        if (!video || video->type != 5) continue;
        const JVal* u = video->find("url");
        if (!u || u->type != 3 || u->str.empty()) continue;
        if (seen.count(u->str)) continue;
        seen.insert(u->str);
        VideoConf c;
        c.url = u->str;
        const JVal* t = video->find("type");
        if (t && t->type == 3) c.type = t->str;
        out.push_back(c);
    }
    return out;
}

// ------------------------- new DPlayer 块解析 -------------------------
// 从 open 位置的花括号开始，返回配对的 '}' 下标；失败返回 npos
static size_t matchBrace(const std::string& s, size_t open) {
    int depth = 0;
    bool inStr = false;
    char q = 0;
    for (size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') { ++i; continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            inStr = true;
            q = c;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

// 在 s 中寻找 key: 形式（key 可带引号），返回 ':' 之后的位置；失败返回 npos
static size_t findKeyColon(const std::string& s, const char* key, size_t from) {
    size_t kl = strlen(key);
    size_t pos = from;
    while ((pos = s.find(key, pos)) != std::string::npos) {
        // 词边界检查
        bool leftOk = true;
        if (pos > 0) {
            unsigned char pc = (unsigned char)s[pos - 1];
            bool isWord = (isalnum(pc) || pc == '_' || pc == '$');
            // 允许前置引号
            if (isWord) leftOk = false;
        }
        size_t p = pos + kl;
        if (leftOk && p < s.size() && (s[p] == '\'' || s[p] == '"')) ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
        if (leftOk && p < s.size() && s[p] == ':') return p + 1;
        pos += kl;
    }
    return std::string::npos;
}

static std::string readJsString(const std::string& s, size_t from, bool& ok) {
    ok = false;
    size_t p = from;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    if (p >= s.size()) return std::string();
    char q = s[p];
    if (q != '\'' && q != '"' && q != '`') return std::string();
    ++p;
    std::string out;
    while (p < s.size()) {
        char c = s[p];
        if (c == '\\' && p + 1 < s.size()) {
            out.push_back(s[p + 1]);
            p += 2;
            continue;
        }
        if (c == q) {
            ok = true;
            return out;
        }
        out.push_back(c);
        ++p;
    }
    return std::string();
}

std::vector<VideoConf> parseDplayerJs(const std::string& html) {
    std::vector<std::string> blocks;
    std::string low = lowerAscii(html);
    size_t pos = 0;
    while (pos < low.size()) {
        size_t a = low.find("new dplayer", pos);
        size_t b = low.find("dplayer(", pos);
        size_t m = std::string::npos;
        if (a != std::string::npos && b != std::string::npos) m = (a < b ? a : b);
        else if (a != std::string::npos) m = a;
        else if (b != std::string::npos) m = b;
        if (m == std::string::npos) break;
        size_t open = html.find('{', m);
        if (open == std::string::npos) break;
        size_t close = matchBrace(html, open);
        if (close == std::string::npos) { pos = m + 9; continue; }
        blocks.push_back(html.substr(open, close - open + 1));
        pos = close + 1;
    }

    std::vector<VideoConf> out;
    std::set<std::string> seen;
    for (const auto& blk : blocks) {
        size_t colon = findKeyColon(blk, "video", 0);
        if (colon == std::string::npos) continue;
        size_t p = colon;
        while (p < blk.size() && (blk[p] == ' ' || blk[p] == '\t' || blk[p] == '\r' || blk[p] == '\n')) ++p;
        if (p >= blk.size() || blk[p] != '{') continue;
        size_t vc = matchBrace(blk, p);
        if (vc == std::string::npos) continue;
        std::string body = blk.substr(p, vc - p + 1);

        size_t ucolon = findKeyColon(body, "url", 0);
        if (ucolon == std::string::npos) continue;
        bool ok = false;
        std::string url = readJsString(body, ucolon, ok);
        if (!ok) continue;
        url = trimStr(url);
        if (url.empty()) continue;
        std::string type;
        size_t tcolon = findKeyColon(body, "type", 0);
        if (tcolon != std::string::npos) {
            bool ok2 = false;
            std::string tv = readJsString(body, tcolon, ok2);
            if (ok2) type = trimStr(tv);
        }
        if (seen.count(url)) continue;
        seen.insert(url);
        VideoConf c;
        c.url = url;
        c.type = type;
        out.push_back(c);
    }
    return out;
}

// ------------------------- URL 处理 -------------------------
static std::string normalizeUrlPath(const std::string& url) {
    size_t sp = url.find("://");
    if (sp == std::string::npos) return url;
    size_t pathStart = url.find('/', sp + 3);
    if (pathStart == std::string::npos) return url;
    std::string origin = url.substr(0, pathStart);
    std::string path = url.substr(pathStart);
    std::string suffix;
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
        suffix = path.substr(q);
        path = path.substr(0, q);
    }
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (seg != "." && !seg.empty()) {
            parts.push_back(seg);
        }
        if (j == path.size()) break;
        i = j + 1;
    }
    std::string outPath;
    for (const auto& s : parts) outPath += "/" + s;
    if (outPath.empty()) outPath = "/";
    return origin + outPath + suffix;
}

std::string resolveUrl(const std::string& raw, const std::string& pageUrl) {
    std::string r = trimStr(raw);
    if (r.empty()) return r;
    std::string lr = lowerAscii(r);
    if (lr.rfind("http://", 0) == 0 || lr.rfind("https://", 0) == 0) return r;
    if (r.rfind("//", 0) == 0) return "https:" + r;

    size_t sp = pageUrl.find("://");
    if (sp == std::string::npos) return r;
    std::string origin = pageUrl.substr(0, sp + 3);
    size_t slash = pageUrl.find('/', sp + 3);
    if (slash != std::string::npos) origin = pageUrl.substr(0, slash);

    if (r[0] == '/') return origin + r;

    std::string base = pageUrl;
    size_t q = base.find_first_of("?#");
    if (q != std::string::npos) base = base.substr(0, q);
    size_t ls = base.rfind('/');
    std::string dir = (ls != std::string::npos) ? base.substr(0, ls + 1) : (origin + "/");
    return normalizeUrlPath(dir + r);
}

std::string detectProtocol(const std::string& url, const std::string& type) {
    std::string lu = lowerAscii(url);
    std::string lt = lowerAscii(type);
    if (lu.find("m3u8") != std::string::npos || lt.find("hls") != std::string::npos)
        return "HLS";
    if (lu.size() >= 4 && lu.compare(lu.size() - 4, 4, ".flv") == 0) return "FLV";
    if (lt.find("flv") != std::string::npos) return "FLV";
    const char* exts[] = {".mp4", ".webm", ".ogg", ".mov", ".m4v"};
    for (const char* e : exts) {
        size_t el = strlen(e);
        if (lu.size() >= el && lu.compare(lu.size() - el, el, e) == 0) return "DIRECT";
    }
    if (lu.find(".mp4?") != std::string::npos) return "DIRECT";
    if (lt.find("mp4") != std::string::npos) return "DIRECT";
    return "UNKNOWN";
}

// ------------------------- 文件名 -------------------------
std::string sanitizeFilename(const std::string& name) {
    std::string s;
    for (char c : name) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            s.push_back('_');
        else
            s.push_back(c);
    }
    // 空白折叠为 _
    std::string t;
    bool lastU = false;
    for (char c : s) {
        bool u = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (u) {
            if (!lastU) t.push_back('_');
            lastU = true;
        } else {
            t.push_back(c);
            lastU = false;
        }
    }
    t = trimStr(t);
    // 去掉首尾的下划线/点（Windows 不允许结尾点）
    while (!t.empty() && (t.front() == '_' || t.front() == '.')) t.erase(t.begin());
    while (!t.empty() && (t.back() == '_' || t.back() == '.')) t.pop_back();
    // 按 UTF-8 边界截断到 120 字节以内
    if (t.size() > 120) {
        size_t cut = 120;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) --cut;
        t = t.substr(0, cut);
    }
    if (t.empty()) t = "video";
    return t;
}

// 进程内已分配路径登记，避免多个任务并发时 TOCTOU 撞名互相覆盖
static std::mutex g_upMutex;
static std::set<std::wstring> g_reservedPaths;

static std::wstring lowerPath(const std::string& p) {
    std::wstring w = u2w(p);
    for (auto& c : w) c = (wchar_t)towlower(c);
    return w;
}

std::string uniquePath(const std::string& saveDir, const std::string& baseName) {
    auto joinP = [](const std::string& dir, const std::string& name) {
        if (dir.empty()) return name;
        char last = dir.back();
        if (last == '\\' || last == '/') return dir + name;
        return dir + "\\" + name;
    };

    std::lock_guard<std::mutex> lk(g_upMutex);
    auto taken = [](const std::string& p) {
        std::wstring wp = u2w(p);
        if (fileExistsW(wp) || fileExistsW(u2w(p + ".part"))) return true;
        return g_reservedPaths.count(lowerPath(p)) > 0;
    };

    std::string candidate = joinP(saveDir, baseName);
    if (!taken(candidate)) {
        g_reservedPaths.insert(lowerPath(candidate));
        return candidate;
    }

    size_t dot = baseName.rfind('.');
    std::string stem = (dot == std::string::npos) ? baseName : baseName.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? std::string() : baseName.substr(dot);
    for (int i = 1; i < 100000; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), " (%d)", i);
        candidate = joinP(saveDir, stem + buf + ext);
        if (!taken(candidate)) {
            g_reservedPaths.insert(lowerPath(candidate));
            return candidate;
        }
    }
    return joinP(saveDir, baseName);
}
