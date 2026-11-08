// http.cpp - WinHTTP 实现
#include "http.h"
#include <winhttp.h>
#include <vector>
#include <cstring>

std::string winErrText(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&buf, 0, nullptr);
    std::string out;
    if (n && buf) {
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
        out = w2u(w);
    }
    if (buf) LocalFree(buf);
    if (out.empty()) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "错误码 %lu", (unsigned long)code);
        out = tmp;
    }
    return out;
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

static bool crackUrl(const std::string& url, ParsedUrl& out, std::string& err) {
    std::wstring w = u2w(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) {
        err = "URL 格式无法解析：" + url;
        return false;
    }
    if (uc.dwHostNameLength == 0) {
        err = "URL 缺少主机名：" + url;
        return false;
    }
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    if (uc.dwUrlPathLength) out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    return true;
}

struct ReqCtx {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    ~ReqCtx() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

static void addHeader(HINTERNET req, const std::wstring& line) {
    WinHttpAddRequestHeaders(req, line.c_str(), (DWORD)line.size(),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

static bool buildRequest(const std::string& url, const std::string& referer,
                         const wchar_t* verb, long long rangeStart,
                         ReqCtx& ctx, ParsedUrl& pu, std::string& err) {
    if (!crackUrl(url, pu, err)) return false;

    ctx.session = WinHttpOpen(kUserAgentW, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ctx.session) {
        err = "初始化 WinHTTP 会话失败：" + winErrText(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(ctx.session, 15000, 15000, 30000, 60000);

    ctx.connect = WinHttpConnect(ctx.session, pu.host.c_str(), pu.port, 0);
    if (!ctx.connect) {
        err = "连接主机失败：" + winErrText(GetLastError());
        return false;
    }

    DWORD flags = pu.https ? WINHTTP_FLAG_SECURE : 0;
    ctx.request = WinHttpOpenRequest(ctx.connect, verb, pu.path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!ctx.request) {
        err = "创建请求失败：" + winErrText(GetLastError());
        return false;
    }

    addHeader(ctx.request, std::wstring(L"Accept: */*"));
    addHeader(ctx.request, std::wstring(L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8"));
    if (!referer.empty()) {
        addHeader(ctx.request, L"Referer: " + u2w(referer));
    }
    if (rangeStart > 0) {
        wchar_t rangeBuf[64];
        swprintf(rangeBuf, 64, L"Range: bytes=%lld-", rangeStart);
        addHeader(ctx.request, std::wstring(rangeBuf));
    }
    return true;
}

static bool sendAndWait(HINTERNET req, DWORD* statusOut, std::string& err) {
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = "发送请求失败：" + winErrText(GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        err = "接收响应失败：" + winErrText(GetLastError());
        return false;
    }
    DWORD code = 0;
    DWORD len = sizeof(code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        err = "读取状态码失败：" + winErrText(GetLastError());
        return false;
    }
    if (statusOut) *statusOut = code;
    return true;
}

static long long queryContentLength(HINTERNET req) {
    DWORD len = 0;
    DWORD v = 0;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &v, &len, WINHTTP_NO_HEADER_INDEX)) {
        return (long long)v;
    }
    return -1;
}

static bool queryHasBytesRange(HINTERNET req) {
    wchar_t buf[128];
    DWORD len = sizeof(buf) - sizeof(wchar_t);
    ZeroMemory(buf, sizeof(buf));
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Accept-Ranges", buf, &len,
                            WINHTTP_NO_HEADER_INDEX)) {
        std::wstring v(buf);
        for (auto& c : v) c = (wchar_t)towlower(c);
        return v.find(L"bytes") != std::wstring::npos;
    }
    return false;
}

bool httpGetText(const std::string& url, const std::string& referer,
                 std::string& body, std::string& err, int timeoutMs) {
    body.clear();
    ReqCtx ctx;
    ParsedUrl pu;
    if (!buildRequest(url, referer, L"GET", 0, ctx, pu, err)) return false;
    WinHttpSetTimeouts(ctx.session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    DWORD code = 0;
    if (!sendAndWait(ctx.request, &code, err)) return false;
    if (code < 200 || code >= 300) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "HTTP %lu", (unsigned long)code);
        err = tmp;
        return false;
    }
    std::vector<char> buf(kChunkSize);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(ctx.request, &avail)) {
            err = "读取数据失败：" + winErrText(GetLastError());
            return false;
        }
        if (avail == 0) break;
        DWORD want = avail > (DWORD)buf.size() ? (DWORD)buf.size() : avail;
        DWORD got = 0;
        if (!WinHttpReadData(ctx.request, buf.data(), want, &got)) {
            err = "读取数据失败：" + winErrText(GetLastError());
            return false;
        }
        if (got == 0) break;
        body.append(buf.data(), got);
    }
    return true;
}

bool httpHeadInfo(const std::string& url, const std::string& referer,
                  long long& total, bool& acceptRanges, std::string& err) {
    total = -1;
    acceptRanges = false;
    ReqCtx ctx;
    ParsedUrl pu;
    if (!buildRequest(url, referer, L"HEAD", 0, ctx, pu, err)) return false;
    DWORD code = 0;
    if (!sendAndWait(ctx.request, &code, err)) return false;
    if (code < 200 || code >= 400) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "HTTP %lu", (unsigned long)code);
        err = tmp;
        return false;
    }
    total = queryContentLength(ctx.request);
    acceptRanges = queryHasBytesRange(ctx.request);
    return true;
}

bool httpStream(const std::string& url, const std::string& referer,
                long long rangeStart, HttpStreamResult& info,
                const HttpSink& sink, std::string& err) {
    info = HttpStreamResult();
    ReqCtx ctx;
    ParsedUrl pu;
    if (!buildRequest(url, referer, L"GET", rangeStart, ctx, pu, err)) return false;
    DWORD code = 0;
    if (!sendAndWait(ctx.request, &code, err)) return false;
    info.statusCode = (int)code;
    if (code < 200 || code >= 400) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "HTTP %lu", (unsigned long)code);
        err = tmp;
        return false;
    }
    info.contentLength = queryContentLength(ctx.request);
    info.acceptRanges = queryHasBytesRange(ctx.request);

    std::vector<char> buf(kChunkSize);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(ctx.request, &avail)) {
            DWORD e = GetLastError();
            if (e == ERROR_WINHTTP_OPERATION_CANCELLED) {
                err = "已中断";
                return false;
            }
            err = "读取数据失败：" + winErrText(e);
            return false;
        }
        if (avail == 0) break;
        DWORD want = avail > (DWORD)buf.size() ? (DWORD)buf.size() : avail;
        DWORD got = 0;
        if (!WinHttpReadData(ctx.request, buf.data(), want, &got)) {
            err = "读取数据失败：" + winErrText(GetLastError());
            return false;
        }
        if (got == 0) break;
        if (!sink(buf.data(), got)) {
            err = "已中断";
            return false;
        }
    }
    return true;
}
