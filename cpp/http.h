// http.h - 基于 WinHTTP 的 HTTP 访问封装（无第三方依赖）
#pragma once
#include "core.h"

struct HttpStreamResult {
    int statusCode = 0;
    long long contentLength = -1;
    bool acceptRanges = false;
};

// 抓取文本（HTML / m3u8）
bool httpGetText(const std::string& url, const std::string& referer,
                 std::string& body, std::string& err, int timeoutMs = 15000);

// HEAD 探测：总字节数与是否支持 Range
bool httpHeadInfo(const std::string& url, const std::string& referer,
                  long long& total, bool& acceptRanges, std::string& err);

// 流式 GET。sink 返回 false 表示调用方要求中断（暂停/取消/写盘失败）
using HttpSink = std::function<bool(const char* data, size_t len)>;
bool httpStream(const std::string& url, const std::string& referer,
                long long rangeStart, HttpStreamResult& info,
                const HttpSink& sink, std::string& err);

std::string winErrText(DWORD code);
