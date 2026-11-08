// core.cpp - 工具函数、日志队列、全局状态
#include "core.h"
#include <filesystem>
#include <chrono>

const char* const kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
const wchar_t* const kUserAgentW =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

HWND g_hMain = nullptr;

const char* statusText(int s) {
    switch (s) {
        case ST_WAIT:   return "等待下载";
        case ST_DOWN:   return "下载中";
        case ST_PAUSE:  return "已暂停";
        case ST_DONE:   return "已完成";
        case ST_FAIL:   return "失败";
        case ST_CANCEL: return "已取消";
        default:        return "未知";
    }
}

bool isFinalStatus(int s) {
    return s == ST_DONE || s == ST_FAIL || s == ST_CANCEL;
}

std::string w2u(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring u2w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

std::string trimStr(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= 0x20) ++b;
    while (e > b && (unsigned char)s[e - 1] <= 0x20) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

std::string fmtBytes(long long bytes) {
    char buf[64];
    if (bytes < 0) bytes = 0;
    double mb = bytes / 1048576.0;
    if (mb >= 1024.0)
        snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return std::string(buf);
}

std::string fmtProgress(double pct, long long done, long long total) {
    char buf[128];
    if (total > 0)
        snprintf(buf, sizeof(buf), "%.1f%% (%s / %s)", pct, fmtBytes(done).c_str(), fmtBytes(total).c_str());
    else
        snprintf(buf, sizeof(buf), "%s", fmtBytes(done).c_str());
    return std::string(buf);
}

// ---------------- 日志队列 ----------------
static std::mutex g_logMtx;
static std::queue<std::string> g_logQ;

void logLine(const std::string& utf8) {
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logQ.push(utf8);
    }
    if (g_hMain) PostMessageW(g_hMain, WM_APP_LOG, 0, 0);
}

bool takeLog(std::string& out) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logQ.empty()) return false;
    out = g_logQ.front();
    g_logQ.pop();
    return true;
}

// ---------------- 保存目录 ----------------
static std::mutex g_dirMtx;
static std::string g_saveDir;

std::string getSaveDir() {
    std::lock_guard<std::mutex> lk(g_dirMtx);
    return g_saveDir;
}

void setSaveDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_dirMtx);
    g_saveDir = dir;
}

// ---------------- 文件系统 ----------------
bool ensureDirW(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    return std::filesystem::is_directory(std::filesystem::path(dir), ec);
}

bool fileExistsW(const std::wstring& p) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(p), ec);
}

long long fileSizeW(const std::wstring& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(std::filesystem::path(p), ec);
    if (ec) return -1;
    return (long long)sz;
}

bool removeFileW(const std::wstring& p) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(p), ec);
    return !ec;
}

bool removeTreeW(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(dir), ec);
    return !ec;
}
