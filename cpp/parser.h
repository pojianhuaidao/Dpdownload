// parser.h - 页面解析与文件名处理
#pragma once
#include "core.h"

struct VideoConf {
    std::string url;
    std::string type;
};

// 抓取页面 HTML
bool getPageHtml(const std::string& pageUrl, std::string& html, std::string& err);

std::string extractPageTitle(const std::string& html);
std::vector<VideoConf> parseDataConfig(const std::string& html);
std::vector<VideoConf> parseDplayerJs(const std::string& html);
std::string resolveUrl(const std::string& raw, const std::string& pageUrl);
std::string detectProtocol(const std::string& url, const std::string& type);
std::string sanitizeFilename(const std::string& name);
std::string uniquePath(const std::string& saveDir, const std::string& baseName);
std::string htmlUnescape(const std::string& s);
