// engine.cpp - 任务级并发下载引擎（直链续传 / HLS 分片 / ffmpeg 兜底）
#include "core.h"
#include "http.h"
#include "parser.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <algorithm>
#include <cwctype>

// 全局入口（worker 线程按协议分发）
void runDirectTask(const TaskPtr& t, const std::string& saveDir);
void runHlsTask(const TaskPtr& t, const std::string& saveDir);
void runGenericTask(const TaskPtr& t, const std::string& saveDir);

namespace {

void runFfmpegCore(const TaskPtr& t, const std::string& saveDir);

long long nowSec() { return (long long)(GetTickCount64() / 1000ULL); }

void notifyUi() {
    if (g_hMain) PostMessageW(g_hMain, WM_APP_TASKS, 0, 0);
}

void setStatus(const TaskPtr& t, int st) {
    t->status = st;
    notifyUi();
}

std::string T(int tid) {
    char b[32];
    snprintf(b, sizeof(b), "#%d", tid);
    return std::string(b);
}

// ---------------- 队列与工作线程 ----------------
struct QItem {
    TaskPtr task;
    std::string saveDir;
};

std::queue<QItem> g_q;
std::mutex g_qm;
std::condition_variable g_cv;
std::atomic<int> g_active{0};
std::once_flag g_once;

void workerLoop() {
    for (;;) {
        QItem it;
        {
            std::unique_lock<std::mutex> lk(g_qm);
            g_cv.wait(lk, [] { return !g_q.empty(); });
            it = g_q.front();
            g_q.pop();
        }
        ++g_active;
        {
            TaskPtr t = it.task;
            std::string saveDir = it.saveDir;
            if (t->cancelReq.load()) {
                setStatus(t, ST_CANCEL);
            } else {
                t->pauseReq = false;
                t->startedAt = nowSec();
                t->setError("");
                setStatus(t, ST_DOWN);
                try {
                    if (t->protocol == "DIRECT") {
                        runDirectTask(t, saveDir);
                    } else if (t->protocol == "HLS") {
                        runHlsTask(t, saveDir);
                    } else {
                        runGenericTask(t, saveDir);
                    }
                } catch (const std::exception& e) {
                    t->setError(e.what());
                    logLine("[失败] 任务 " + T(t->tid) + " 异常：" + std::string(e.what()));
                    setStatus(t, ST_FAIL);
                } catch (...) {
                    t->setError("未知异常");
                    logLine("[失败] 任务 " + T(t->tid) + " 出现未知异常");
                    setStatus(t, ST_FAIL);
                }
                if (t->status.load() == ST_DOWN) {
                    t->setError("任务异常结束");
                    setStatus(t, ST_FAIL);
                }
            }
        }
        --g_active;
        notifyUi();
    }
}

void ensureWorkers() {
    std::call_once(g_once, [] {
        for (int i = 0; i < kMaxConcurrent; ++i) std::thread(workerLoop).detach();
    });
}

}  // namespace

void engineEnqueue(TaskPtr t, const std::string& saveDir) {
    ensureWorkers();
    {
        std::lock_guard<std::mutex> lk(g_qm);
        g_q.push(QItem{t, saveDir});
    }
    g_cv.notify_one();
}

void engineCancelAll() {
    // 仅标记取消，实际清理由各下载循环完成
}

int engineActiveCount() { return g_active.load(); }

// ================= 下载实现 =================
namespace {

bool checkAbort(const TaskPtr& t, std::string& reason) {
    if (t->cancelReq.load()) { reason = "cancel"; return true; }
    if (t->pauseReq.load()) { reason = "pause"; return true; }
    if (nowSec() - t->startedAt > kTaskTimeoutSec) { reason = "timeout"; return true; }
    return false;
}

void cleanupTask(const TaskPtr& t, const std::string& saveDir) {
    if (!t->finalPath.empty()) {
        removeFileW(u2w(t->finalPath + ".part"));
        removeFileW(u2w(t->finalPath));
    }
    std::string tmp = saveDir + "\\.tmp_" + std::to_string(t->tid) + "_" +
                      sanitizeFilename(t->title);
    removeTreeW(u2w(tmp));
}

std::wstring tmpDirOf(const TaskPtr& t, const std::string& saveDir) {
    std::string tmp = saveDir + "\\.tmp_" + std::to_string(t->tid) + "_" +
                      sanitizeFilename(t->title);
    return u2w(tmp);
}

// ---------------- 直链下载（Range 断点续传） ----------------
void runDirectCore(const TaskPtr& t, const std::string& saveDir) {
    std::string final = t->finalPath.empty() ? uniquePath(saveDir, t->baseName) : t->finalPath;
    t->finalPath = final;
    std::string part = final + ".part";

    if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }

    long long total = -1;
    bool acceptRanges = false;
    std::string headErr;
    if (!httpHeadInfo(t->videoUrl, t->pageUrl, total, acceptRanges, headErr)) {
        logLine("[提示] 任务 " + T(t->tid) + " HEAD 探测失败（" + headErr + "），将从头下载");
        total = -1;
    }

    long long offset = 0;
    if (fileExistsW(u2w(part))) {
        long long sz = fileSizeW(u2w(part));
        if (sz > 0 && acceptRanges) {
            offset = sz;
            logLine("任务 " + T(t->tid) + " 检测到断点 " + std::to_string(offset) + " 字节，继续续传");
        } else {
            offset = 0;
        }
    }

    auto attempt = [&](long long startOffset, bool& interrupted) -> bool {
        std::ofstream f;
        std::ios::openmode mode = std::ios::binary | std::ios::out;
        if (startOffset > 0) mode |= std::ios::app;
        else mode |= std::ios::trunc;
        f.open(std::filesystem::path(u2w(part)), mode);
        if (!f.is_open()) {
            t->setError("无法写入文件：" + part);
            return false;
        }
        long long downloaded = startOffset;
        t->downloadedBytes = downloaded;
        t->totalBytes = total;
        if (total > 0) t->progress = downloaded * 100.0 / (double)total;
        else t->progress = -1.0;

        double lastLogPct = -100.0;
        HttpStreamResult info;
        std::string err;
        bool ok = httpStream(t->videoUrl, t->pageUrl, startOffset, info,
                             [&](const char* data, size_t len) -> bool {
                                 std::string reason;
                                 if (checkAbort(t, reason)) {
                                     if (reason == "timeout")
                                         t->setError("任务总耗时超过 " + std::to_string(kTaskTimeoutSec) + " 秒上限，已终止");
                                     return false;
                                 }
                                 f.write(data, (std::streamsize)len);
                                 if (!f.good()) {
                                     t->setError("磁盘写入失败");
                                     return false;
                                 }
                                 downloaded += (long long)len;
                                 t->downloadedBytes = downloaded;
                                 if (total > 0) {
                                     double pct = downloaded * 100.0 / (double)total;
                                     if (pct > 100.0) pct = 100.0;
                                     t->progress = pct;
                                     if (pct - lastLogPct >= 5.0 || lastLogPct < 0) {
                                         lastLogPct = pct;
                                         logLine("  任务 " + T(t->tid) + " " +
                                                 fmtProgress(pct, downloaded, total));
                                     }
                                     notifyUi();
                                 } else {
                                     t->progress = -1.0;
                                     if (downloaded % (5LL * 1048576LL) < (long long)len) {
                                         logLine("  任务 " + T(t->tid) + " 已下载 " +
                                                 fmtBytes(downloaded) + "（未知总大小）");
                                         notifyUi();
                                     }
                                 }
                                 return true;
                             }, err);
        f.flush();
        f.close();
        if (!ok) {
            std::string reason;
            if (checkAbort(t, reason)) {
                interrupted = true;
                return false;
            }
            t->setError(err.empty() ? std::string("下载中断") : err);
            return false;
        }
        interrupted = false;
        return true;
    };

    bool interrupted = false;
    bool started = false;
    if (offset > 0 && acceptRanges) {
        started = attempt(offset, interrupted);
        if (!started && interrupted) {
            if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }
            if (t->cancelReq.load()) { cleanupTask(t, saveDir); setStatus(t, ST_CANCEL); return; }
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            cleanupTask(t, saveDir);
            setStatus(t, ST_FAIL);
            return;
        }
    }
    if (!started) {
        // 从头下载（offset==0 或续传失败）
        offset = 0;
        started = attempt(0, interrupted);
        if (!started) {
            if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }
            if (t->cancelReq.load()) { cleanupTask(t, saveDir); setStatus(t, ST_CANCEL); return; }
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            cleanupTask(t, saveDir);
            setStatus(t, ST_FAIL);
            return;
        }
    }

    long long sz = fileSizeW(u2w(part));
    if (sz > 0) {
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(u2w(part)),
                                std::filesystem::path(u2w(final)), ec);
        if (ec) {
            // 跨卷或目标占用时退回复制
            std::filesystem::copy_file(std::filesystem::path(u2w(part)),
                                       std::filesystem::path(u2w(final)),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) removeFileW(u2w(part));
        }
        if (fileExistsW(u2w(final))) {
            t->progress = 100.0;
            logLine("[完成] 任务 " + T(t->tid) + " 保存为：" + final);
            setStatus(t, ST_DONE);
            return;
        }
    }
    t->setError("下载结果为空");
    cleanupTask(t, saveDir);
    setStatus(t, ST_FAIL);
}

// ---------------- HLS 分片下载 ----------------
bool fetchToMemory(const std::string& url, const std::string& referer,
                   const TaskPtr& t, std::string& out, std::string& err) {
    out.clear();
    HttpStreamResult info;
    bool ok = httpStream(url, referer, 0, info,
                         [&](const char* data, size_t len) -> bool {
                             std::string reason;
                             if (checkAbort(t, reason)) return false;
                             out.append(data, len);
                             return true;
                         }, err);
    return ok && !out.empty();
}

std::vector<std::string> parseTsList(const std::string& text, const std::string& m3u8Url) {
    std::vector<std::string> segs;
    for (const auto& line0 : splitLines(text)) {
        std::string line = trimStr(line0);
        if (line.empty() || line[0] == '#') continue;
        std::string low = lowerAscii(line);
        if (low.size() >= 5 && low.compare(low.size() - 5, 5, ".m3u8") == 0) continue;
        segs.push_back(resolveUrl(line, m3u8Url));
    }
    return segs;
}

bool hasAes128(const std::string& text) {
    std::string low = lowerAscii(text);
    size_t p = 0;
    while ((p = low.find("#ext-x-key:", p)) != std::string::npos) {
        size_t e = low.find('\n', p);
        std::string line = low.substr(p, (e == std::string::npos ? low.size() : e) - p);
        if (line.find("aes-128") != std::string::npos) return true;
        p = (e == std::string::npos) ? low.size() : e;
    }
    return false;
}

void mergeTs(const std::wstring& tmpDirW, const std::string& finalPath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> parts;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(tmpDirW), ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() == L".ts") parts.push_back(e.path());
    }
    std::sort(parts.begin(), parts.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().wstring() < b.filename().wstring();
    });
    std::ofstream out(fs::path(u2w(finalPath)), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) throw std::runtime_error("无法创建输出文件");
    std::vector<char> buf(1 << 20);
    for (auto& p : parts) {
        std::ifstream in(p, std::ios::binary);
        if (!in.is_open()) continue;
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize got = in.gcount();
            if (got > 0) out.write(buf.data(), got);
        }
    }
    out.flush();
    if (!out.good()) throw std::runtime_error("写入输出文件失败");
}

void runHlsCore(const TaskPtr& t, const std::string& saveDir) {
    extern void runFfmpegCore(const TaskPtr&, const std::string&);

    std::string final = t->finalPath.empty() ? uniquePath(saveDir, t->baseName) : t->finalPath;
    t->finalPath = final;
    std::wstring tmpW = tmpDirOf(t, saveDir);
    std::string tmpA = saveDir + "\\.tmp_" + std::to_string(t->tid) + "_" + sanitizeFilename(t->title);

    if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }

    std::string text;
    std::string err;
    if (!httpGetText(t->videoUrl, t->pageUrl, text, err, 20000)) {
        t->setError("m3u8 获取失败：" + err);
        logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
        setStatus(t, ST_FAIL);
        return;
    }

    if (hasAes128(text)) {
        t->setNote("AES-128 加密，ffmpeg 全量下载（不支持暂停续传）");
        logLine("[提示] 任务 " + T(t->tid) + " " + t->getNote() + "（不支持暂停）");
        runFfmpegCore(t, saveDir);
        return;
    }

    std::vector<std::string> segs = parseTsList(text, t->videoUrl);
    if (segs.empty()) {
        t->setError("m3u8 中未解析到分片列表");
        logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
        setStatus(t, ST_FAIL);
        return;
    }
    logLine("任务 " + T(t->tid) + " 解析到 " + std::to_string(segs.size()) + " 个分片");

    if (!ensureDirW(tmpW)) {
        t->setError("无法创建临时目录");
        setStatus(t, ST_FAIL);
        return;
    }

    for (size_t idx = 0; idx < segs.size(); ++idx) {
        if (t->cancelReq.load()) {
            cleanupTask(t, saveDir);
            setStatus(t, ST_CANCEL);
            return;
        }
        if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }
        if (nowSec() - t->startedAt > kTaskTimeoutSec) {
            t->setError("任务总耗时超过 " + std::to_string(kTaskTimeoutSec) + " 秒上限，已终止");
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            cleanupTask(t, saveDir);
            setStatus(t, ST_FAIL);
            return;
        }

        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "%04d.ts", (int)idx);
        std::wstring tsPathW = tmpW + L"\\" + u2w(nameBuf);
        long long existing = fileSizeW(tsPathW);
        if (existing > 0) {
            // 已下载分片：跳过（续传）
            t->progress = (double)(idx + 1) * 100.0 / (double)segs.size();
            continue;
        }

        bool segOk = false;
        std::string segErr;
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::string data;
            std::string ferr;
            if (fetchToMemory(segs[idx], t->pageUrl, t, data, ferr)) {
                std::ofstream f(std::filesystem::path(tsPathW), std::ios::binary | std::ios::trunc);
                if (f.is_open()) {
                    f.write(data.data(), (std::streamsize)data.size());
                    f.close();
                    segOk = true;
                    break;
                }
                ferr = "无法写入分片文件";
            }
            segErr = ferr;
            if (t->cancelReq.load()) break;
            if (t->pauseReq.load()) break;
            if (attempt < 2) Sleep((DWORD)(1000 * (attempt + 1)));
        }

        if (!segOk) {
            if (t->cancelReq.load()) {
                cleanupTask(t, saveDir);
                setStatus(t, ST_CANCEL);
                return;
            }
            if (t->pauseReq.load()) { setStatus(t, ST_PAUSE); return; }
            t->setError("分片 " + std::to_string(idx) + " 下载失败：" + segErr);
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            setStatus(t, ST_FAIL);
            return;
        }

        t->progress = (double)(idx + 1) * 100.0 / (double)segs.size();
        t->downloadedBytes += fileSizeW(tsPathW);
        notifyUi();
    }

    try {
        mergeTs(tmpW, final);
    } catch (const std::exception& e) {
        t->setError(std::string("合并失败：") + e.what());
        logLine("[错误] 任务 " + T(t->tid) + " " + t->getError());
        removeTreeW(tmpW);
        setStatus(t, ST_FAIL);
        return;
    }
    removeTreeW(tmpW);
    (void)tmpA;

    if (fileExistsW(u2w(final)) && fileSizeW(u2w(final)) > 0) {
        t->progress = 100.0;
        logLine("[完成] 任务 " + T(t->tid) + " 保存为：" + final);
        setStatus(t, ST_DONE);
    } else {
        t->setError("合并结果为空");
        setStatus(t, ST_FAIL);
    }
}

// ---------------- ffmpeg 兜底 ----------------
bool findFfmpeg(std::wstring& outPath) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH * 2, buf, nullptr);
    if (n > 0 && n < MAX_PATH * 2) {
        outPath = buf;
        return true;
    }
    // 与 exe 同目录
    wchar_t mod[MAX_PATH * 2];
    DWORD m = GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (m > 0) {
        std::wstring dir(mod, m);
        size_t p = dir.rfind(L'\\');
        if (p != std::wstring::npos) {
            std::wstring cand = dir.substr(0, p) + L"\\ffmpeg.exe";
            if (fileExistsW(cand)) { outPath = cand; return true; }
        }
    }
    return false;
}

std::string extractTimeToken(const std::string& line) {
    size_t p = line.find("time=");
    if (p == std::string::npos) return std::string();
    p += 5;
    size_t e = p;
    while (e < line.size() && (isdigit((unsigned char)line[e]) || line[e] == ':' || line[e] == '.')) ++e;
    return line.substr(p, e - p);
}

double parseFfmpegTime(const std::string& ts) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : ts) {
        if (c == ':') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    try {
        if (parts.size() == 3)
            return atof(parts[0].c_str()) * 3600 + atof(parts[1].c_str()) * 60 + atof(parts[2].c_str());
        if (parts.size() == 2)
            return atof(parts[0].c_str()) * 60 + atof(parts[1].c_str());
    } catch (...) {
    }
    return -1;
}

double sumExtinf(const std::string& text) {
    double total = 0;
    size_t p = 0;
    std::string low = lowerAscii(text);
    while ((p = low.find("#extinf:", p)) != std::string::npos) {
        p += 8;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        size_t e = p;
        while (e < text.size() && (isdigit((unsigned char)text[e]) || text[e] == '.')) ++e;
        if (e > p) total += atof(text.substr(p, e - p).c_str());
    }
    return total;
}

void runFfmpegCore(const TaskPtr& t, const std::string& saveDir) {
    std::wstring ff;
    if (!findFfmpeg(ff)) {
        t->setError("未找到 ffmpeg（加密流需要 ffmpeg 合并），请将 ffmpeg.exe 放入程序目录或 PATH");
        logLine("[错误] 任务 " + T(t->tid) + " " + t->getError());
        setStatus(t, ST_FAIL);
        return;
    }

    std::string final = t->finalPath.empty() ? uniquePath(saveDir, t->baseName) : t->finalPath;
    t->finalPath = final;

    double duration = -1;
    if (t->protocol == "HLS") {
        std::string text, err;
        if (httpGetText(t->videoUrl, t->pageUrl, text, err, 20000)) {
            double d = sumExtinf(text);
            if (d > 0) duration = d;
        }
    }

    std::wstring cmd = L"\"" + ff + L"\" -headers \"User-Agent: " + std::wstring(kUserAgentW) +
                       L"\r\nReferer: " + u2w(t->pageUrl) + L"\r\n\" -stats -i \"" +
                       u2w(t->videoUrl) + L"\" -c copy \"" + u2w(final) + L"\" -y";
    logLine("任务 " + T(t->tid) + " 调用 ffmpeg 全量下载 ...");

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        t->setError("创建管道失败");
        setStatus(t, ST_FAIL);
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    BOOL created = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!created) {
        CloseHandle(rd);
        t->setError("启动 ffmpeg 失败：" + winErrText(GetLastError()));
        setStatus(t, ST_FAIL);
        return;
    }

    std::string acc;
    char buf[4096];
    double lastPct = -1;
    long long lastActivity = nowSec();
    std::string lastTime;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            if (ReadFile(rd, buf, want, &got, nullptr) && got > 0) {
                acc.append(buf, got);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, nl);
                    acc.erase(0, nl + 1);
                    std::string tok = extractTimeToken(line);
                    if (!tok.empty() && tok != lastTime) {
                        lastTime = tok;
                        lastActivity = nowSec();
                        double secs = parseFfmpegTime(tok);
                        if (duration > 0 && secs >= 0) {
                            double pct = secs * 100.0 / duration;
                            if (pct > 100.0) pct = 100.0;
                            t->progress = pct;
                            if (pct - lastPct >= 5.0 || lastPct < 0) {
                                lastPct = pct;
                                logLine("  任务 " + T(t->tid) + " " + std::to_string((int)pct) + "%");
                                notifyUi();
                            }
                        }
                    }
                }
            }
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) break;

        if (t->cancelReq.load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(rd);
            cleanupTask(t, saveDir);
            setStatus(t, ST_CANCEL);
            return;
        }
        if (t->pauseReq.load()) {
            // ffmpeg 模式不支持暂停续传，忽略暂停请求
            t->pauseReq = false;
            logLine("[提示] 任务 " + T(t->tid) + " 当前为 ffmpeg 模式，不支持暂停，继续下载");
        }
        if (nowSec() - lastActivity > 60) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(rd);
            t->setError("网络断流：ffmpeg 60 秒无新进度");
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            cleanupTask(t, saveDir);
            setStatus(t, ST_FAIL);
            return;
        }
        if (nowSec() - t->startedAt > kTaskTimeoutSec) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(rd);
            t->setError("任务总耗时超过 " + std::to_string(kTaskTimeoutSec) + " 秒上限，已终止");
            logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
            cleanupTask(t, saveDir);
            setStatus(t, ST_FAIL);
            return;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);

    if (code == 0 && fileExistsW(u2w(final)) && fileSizeW(u2w(final)) > 0) {
        t->progress = 100.0;
        logLine("[完成] 任务 " + T(t->tid) + " 保存为：" + final);
        setStatus(t, ST_DONE);
    } else {
        char b[64];
        snprintf(b, sizeof(b), "ffmpeg 退出码 %lu", (unsigned long)code);
        t->setError(b);
        logLine("[失败] 任务 " + T(t->tid) + " " + t->getError());
        cleanupTask(t, saveDir);
        setStatus(t, ST_FAIL);
    }
}

// ---------------- 通用任务：FLV / UNKNOWN ----------------
void runGenericCore(const TaskPtr& t, const std::string& saveDir) {
    std::wstring ff;
    if (findFfmpeg(ff)) {
        runFfmpegCore(t, saveDir);
        return;
    }
    // 无 ffmpeg：直接流式保存原始流
    logLine("[提示] 任务 " + T(t->tid) + " 未检测到 ffmpeg，直接保存原始流");
    std::string ext = ".mp4";
    if (t->protocol == "FLV") ext = ".flv";
    TaskPtr tt = t;
    std::string baseNoExt = t->baseName;
    size_t dot = baseNoExt.rfind('.');
    if (dot != std::string::npos) baseNoExt = baseNoExt.substr(0, dot);
    tt->baseName = baseNoExt + ext;
    tt->protocol = "DIRECT";
    runDirectCore(tt, saveDir);
}

}  // namespace

// 对外可见入口（供 workerLoop 调用）
namespace {
}

void runDirectTask(const TaskPtr& t, const std::string& saveDir) { runDirectCore(t, saveDir); }
void runHlsTask(const TaskPtr& t, const std::string& saveDir) { runHlsCore(t, saveDir); }
void runGenericTask(const TaskPtr& t, const std::string& saveDir) { runGenericCore(t, saveDir); }
