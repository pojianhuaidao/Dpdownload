// core.h - 公共类型、常量与工具声明
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>

extern const char* const kUserAgent;
extern const wchar_t* const kUserAgentW;

constexpr int kMaxConcurrent = 3;
constexpr long long kTaskTimeoutSec = 7200;
constexpr size_t kChunkSize = 64 * 1024;

// 任务状态
enum TaskStatus { ST_WAIT = 0, ST_DOWN, ST_PAUSE, ST_DONE, ST_FAIL, ST_CANCEL };
const char* statusText(int s);
bool isFinalStatus(int s);

struct Task {
    int tid = 0;
    int rowIndex = 0;
    std::string pageUrl;
    std::string videoUrl;
    std::string protocol;   // HLS / FLV / DIRECT / UNKNOWN
    std::string title;
    std::string baseName;   // 已含扩展名
    std::string finalPath;

    std::atomic<int> status{ST_WAIT};
    std::atomic<double> progress{-1.0};     // 0-100，-1 表示未知总大小
    std::atomic<bool> pauseReq{false};
    std::atomic<bool> cancelReq{false};
    std::atomic<long long> downloadedBytes{0};
    std::atomic<long long> totalBytes{0};
    long long startedAt = 0;

    std::mutex strMtx;
    std::string note;
    std::string error;

    void setNote(const std::string& s) { std::lock_guard<std::mutex> lk(strMtx); note = s; }
    std::string getNote() { std::lock_guard<std::mutex> lk(strMtx); return note; }
    void setError(const std::string& s) { std::lock_guard<std::mutex> lk(strMtx); error = s; }
    std::string getError() { std::lock_guard<std::mutex> lk(strMtx); return error; }
};
using TaskPtr = std::shared_ptr<Task>;

// 字符串与编码
std::string w2u(const std::wstring& w);
std::wstring u2w(const std::string& s);
std::string lowerAscii(std::string s);
std::string trimStr(const std::string& s);
std::vector<std::string> splitLines(const std::string& s);
std::string fmtBytes(long long bytes);
std::string fmtProgress(double pct, long long done, long long total);

// 日志（线程安全队列，界面定时器消费）
void logLine(const std::string& utf8);
bool takeLog(std::string& out);

// 全局主窗口句柄与消息
extern HWND g_hMain;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_TASKS = WM_APP + 2;

// 保存目录
std::string getSaveDir();
void setSaveDir(const std::string& dir);

// 下载引擎
void engineEnqueue(TaskPtr t, const std::string& saveDir);
void engineCancelAll();
int engineActiveCount();

// 文件系统辅助
bool ensureDirW(const std::wstring& dir);
bool fileExistsW(const std::wstring& p);
long long fileSizeW(const std::wstring& p);
bool removeFileW(const std::wstring& p);
bool removeTreeW(const std::wstring& dir);
