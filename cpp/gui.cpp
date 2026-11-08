// gui.cpp - 主窗口、自绘任务列表、日志与交互
#include "core.h"
#include "parser.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <set>

#ifndef IDI_MAIN
#define IDI_MAIN 101
#endif

namespace {

// ---------- 控件 ID ----------
enum {
    IDC_URLS = 1001,
    IDC_PARSE,
    IDC_DOWNLOAD,
    IDC_CANCELALL,
    IDC_CLEARLOG,
    IDC_DIR,
    IDC_BROWSE,
    IDC_STATUS,
    IDC_LIST,
    IDC_TOTAL,
    IDC_LOG,
    IDC_LBL_URLS,
    IDC_LBL_DIR,
    IDC_LBL_LIST,
    IDC_LBL_TOTAL
};

const wchar_t* kListClass = L"DPTaskListWnd";

// ---------- 全局状态（仅 UI 线程访问任务表） ----------
HINSTANCE g_hInst = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hUrls = nullptr, g_hLog = nullptr, g_hList = nullptr, g_hDir = nullptr;
HWND g_hStatus = nullptr, g_hProg = nullptr;
HWND g_hLblUrls = nullptr, g_hLblDir = nullptr, g_hLblList = nullptr, g_hLblTotal = nullptr;

std::vector<TaskPtr> g_tasks;
std::mutex g_pendMtx;
std::vector<TaskPtr> g_pending;
std::atomic<int> g_parsePending{0};
int g_nextTid = 1;
bool g_doneNotified = false;
int g_listScroll = 0;
int g_dpi = 96;

HFONT g_fontUi = nullptr, g_fontSmall = nullptr, g_fontBold = nullptr;

int DP(int v) { return MulDiv(v, g_dpi, 96); }

// ---------- 日志 ----------
void appendLog(const std::string& utf8) {
    if (!g_hLog) return;
    int len = GetWindowTextLengthW(g_hLog);
    if (len > 300000) SetWindowTextW(g_hLog, L"");
    std::wstring w = u2w(utf8) + L"\r\n";
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

void setStatusBar(const std::wstring& text) {
    if (g_hStatus) SetWindowTextW(g_hStatus, text.c_str());
}

// ---------- 尺寸 ----------
int rowHeight() { return DP(48); }

RECT btnMainRect(int W, int rowTop) {
    int bw = DP(64), bh = DP(26), bh2 = DP(12);
    RECT r{W - DP(8) - bw * 2 - DP(8), rowTop + bh2, W - DP(8) - bw - DP(8), rowTop + bh2 + bh};
    return r;
}

RECT btnSubRect(int W, int rowTop) {
    int bw = DP(64), bh = DP(26), bh2 = DP(12);
    RECT r{W - DP(8) - bw, rowTop + bh2, W - DP(8), rowTop + bh2 + bh};
    return r;
}

// ---------- 交互动作 ----------
std::string uiSaveDir() {
    if (!g_hDir) return std::string();
    int n = GetWindowTextLengthW(g_hDir);
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(g_hDir, &w[0], n + 1);
    w.resize((size_t)n);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    return w2u(w);
}

bool ensureSaveDir(std::string& dirOut) {
    std::string dir = uiSaveDir();
    if (dir.empty()) {
        appendLog("[错误] 保存目录为空，请先设置保存位置。");
        return false;
    }
    if (!ensureDirW(u2w(dir))) {
        appendLog("[错误] 保存目录无法创建或不可写：" + dir);
        return false;
    }
    dirOut = dir;
    return true;
}

void startTask(const TaskPtr& t) {
    std::string dir;
    if (!ensureSaveDir(dir)) return;
    t->pauseReq = false;
    t->cancelReq = false;
    t->setError("");
    t->status = ST_WAIT;
    engineEnqueue(t, dir);
    setSaveDir(dir);
    appendLog("已提交任务 " + std::to_string(t->tid) + " 下载（最多同时 3 个）");
}

void pauseTask(const TaskPtr& t) {
    t->pauseReq = true;
    appendLog("任务 " + std::to_string(t->tid) + " 已请求暂停（保留断点）");
}

void cancelTask(const TaskPtr& t) {
    t->cancelReq = true;
    if (t->status.load() == ST_WAIT || t->status.load() == ST_PAUSE) {
        t->status = ST_CANCEL;
    }
    appendLog("任务 " + std::to_string(t->tid) + " 已请求取消");
}

void openTaskFile(const TaskPtr& t) {
    if (t->finalPath.empty()) return;
    std::wstring p = u2w(t->finalPath);
    std::wstring arg = L"/select,\"" + p + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void refreshTotal() {
    int total = (int)g_tasks.size();
    int done = 0;
    for (auto& t : g_tasks) {
        int s = t->status.load();
        if (s == ST_DONE || s == ST_FAIL || s == ST_CANCEL) ++done;
    }
    int pct = total > 0 ? (int)(done * 100.0 / total) : 0;
    if (total > 0 && done == total) pct = 100;
    if (g_hProg) SendMessageW(g_hProg, PBM_SETPOS, (WPARAM)pct, 0);

    if (total == 0) {
        setStatusBar(L"就绪：粘贴多个 URL 后点「解析全部」，再点「全部下载」");
        return;
    }
    int okCount = 0, failCount = 0, cancelCount = 0, runCount = 0;
    for (auto& t : g_tasks) {
        int s = t->status.load();
        if (s == ST_DONE) ++okCount;
        else if (s == ST_FAIL) ++failCount;
        else if (s == ST_CANCEL) ++cancelCount;
        else ++runCount;
    }
    wchar_t buf[256];
    swprintf(buf, 256, L"任务 %d 个：成功 %d / 失败 %d / 取消 %d / 进行中 %d",
             total, okCount, failCount, cancelCount, runCount);
    setStatusBar(done == total ? L"全部完成（" + std::wstring(buf) + L"）" : std::wstring(buf));

    if (done == total && total > 0 && !g_doneNotified) {
        g_doneNotified = true;
        swprintf(buf, 256, L"成功 %d 个 / 失败 %d 个 / 已取消 %d 个",
                 okCount, failCount, cancelCount);
        MessageBoxW(g_hMainWnd, buf, L"全部完成", MB_OK | MB_ICONINFORMATION);
    }
    if (done != total) g_doneNotified = false;
}

// ---------- 解析线程 ----------
void parseOneUrl(const std::string& pageUrl) {
    std::string html, err;
    if (!getPageHtml(pageUrl, html, err)) {
        logLine("[错误] 解析失败 " + pageUrl + "：" + err);
        logLine("提示：站点可能反爬、需要登录、超时或证书异常；可用浏览器 F12 -> Network -> media/xhr 手动获取。");
        return;
    }
    std::vector<VideoConf> vids = parseDataConfig(html);
    std::string source = "data-config 属性";
    if (vids.empty()) {
        vids = parseDplayerJs(html);
        source = "new DPlayer / dplayer 配置段";
    }
    if (vids.empty()) {
        logLine("[错误] 未在页面中找到 DPlayer 配置：" + pageUrl);
        logLine("提示：请用浏览器 F12 -> Network -> media/xhr 手动查找真实地址。");
        return;
    }
    std::string title = extractPageTitle(html);
    if (title.empty()) title = "video";
    int idx = 0;
    for (const auto& v : vids) {
        ++idx;
        std::string videoUrl = resolveUrl(v.url, pageUrl);
        std::string proto = detectProtocol(videoUrl, v.type);
        logLine("解析成功（" + std::to_string(idx) + "/" + std::to_string(vids.size()) +
                "，" + source + "）：" + videoUrl + " [" + proto + "]");
        TaskPtr t = std::make_shared<Task>();
        t->tid = g_nextTid++;
        t->pageUrl = pageUrl;
        t->videoUrl = videoUrl;
        t->protocol = proto;
        t->title = title;
        t->baseName = sanitizeFilename(title) + ".mp4";
        t->progress = -1.0;
        {
            std::lock_guard<std::mutex> lk(g_pendMtx);
            g_pending.push_back(t);
        }
    }
}

// ---------- 文本输入读取 ----------
std::vector<std::string> readUrlLines() {
    std::vector<std::string> out;
    if (!g_hUrls) return out;
    int n = GetWindowTextLengthW(g_hUrls);
    if (n <= 0) return out;
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(g_hUrls, &w[0], n + 1);
    w.resize((size_t)n);
    for (const auto& line : splitLines(w2u(w))) {
        std::string s = trimStr(line);
        if (s.empty()) continue;
        out.push_back(s);
    }
    return out;
}

bool startsWithHttp(const std::string& s) {
    std::string l = lowerAscii(s);
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

void doParseAll() {
    std::vector<std::string> urls = readUrlLines();
    if (urls.empty()) {
        appendLog("[错误] 请先在输入框中粘贴至少一个 URL。");
        return;
    }
    std::vector<std::string> valid;
    std::set<std::string> seen;
    for (const auto& u : urls) {
        if (!startsWithHttp(u)) {
            appendLog("[错误] 跳过非法 URL：" + u + "（必须以 http(s):// 开头）");
            continue;
        }
        if (seen.insert(u).second) valid.push_back(u);
    }
    if (valid.empty()) return;
    appendLog("开始解析 " + std::to_string(valid.size()) + " 个页面地址 ...");
    setStatusBar(L"正在解析 ...");
    for (const auto& u : valid) {
        ++g_parsePending;
        std::thread([u]() {
            parseOneUrl(u);
            --g_parsePending;
            logLine("[解析] 页面处理结束：" + u);
        }).detach();
    }
}

void doDownloadAll() {
    if (g_tasks.empty()) {
        appendLog("[错误] 暂无已解析任务，请先「解析全部」。");
        return;
    }
    std::string dir;
    if (!ensureSaveDir(dir)) return;
    setSaveDir(dir);
    g_doneNotified = false;
    int started = 0;
    for (auto& t : g_tasks) {
        int s = t->status.load();
        if ((s == ST_WAIT || s == ST_PAUSE || s == ST_FAIL) && !t->cancelReq.load()) {
            t->pauseReq = false;
            t->setError("");
            t->status = ST_WAIT;
            engineEnqueue(t, dir);
            ++started;
        }
    }
    appendLog("已提交 " + std::to_string(started) + " 个任务下载（最多同时 3 个）");
}

void doCancelAll() {
    if (g_tasks.empty()) {
        appendLog("[错误] 暂无任务。");
        return;
    }
    int n = 0;
    for (auto& t : g_tasks) {
        int s = t->status.load();
        if (s == ST_DOWN || s == ST_WAIT || s == ST_PAUSE) {
            t->cancelReq = true;
            if (s == ST_WAIT || s == ST_PAUSE) t->status = ST_CANCEL;
            ++n;
        }
    }
    appendLog("已对 " + std::to_string(n) + " 个活动任务发起取消");
    setStatusBar(L"全部取消：任务将清理并复位为可再次下载状态");
}

void doBrowseDir() {
    std::string cur = uiSaveDir();
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"选择保存目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH * 2];
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(g_hDir, path);
        if (!cur.empty()) appendLog("保存位置已设置为：" + w2u(path));
    }
    CoTaskMemFree(pidl);
}

void clearLog() {
    if (g_hLog) SetWindowTextW(g_hLog, L"");
}

// ---------- 自绘任务列表 ----------
void drawRounded(HDC dc, RECT rc, COLORREF fill, COLORREF border, int radius) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

std::wstring mainBtnLabel(int st) {
    switch (st) {
        case ST_WAIT: return L"开始";
        case ST_DOWN: return L"暂停";
        case ST_PAUSE: return L"继续";
        case ST_FAIL: return L"重试";
        default: return L"";
    }
}

std::wstring subBtnLabel(int st) {
    switch (st) {
        case ST_DONE: return L"打开";
        case ST_DOWN:
        case ST_PAUSE:
        case ST_WAIT:
        case ST_FAIL: return L"取消";
        default: return L"";
    }
}

void drawRow(HDC dc, int idx, const TaskPtr& t, int W, int rowTop) {
    int rh = rowHeight();
    RECT rc{0, rowTop, W, rowTop + rh};
    HBRUSH bg = CreateSolidBrush(idx % 2 ? RGB(250, 251, 253) : RGB(255, 255, 255));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(235, 237, 241));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, 0, rc.bottom - 1, nullptr);
    LineTo(dc, W, rc.bottom - 1);
    SelectObject(dc, op);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);

    // 标题
    SelectObject(dc, g_fontBold);
    SetTextColor(dc, RGB(28, 30, 34));
    std::wstring title = L"#" + std::to_wstring(t->tid) + L"  " + u2w(t->title);
    RECT titleRc{DP(10), rowTop + DP(5), W - DP(160), rowTop + DP(5) + DP(20)};
    DrawTextW(dc, title.c_str(), -1, &titleRc,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    int st = t->status.load();
    double pct = t->progress.load();
    long long done = t->downloadedBytes.load();
    long long total = t->totalBytes.load();

    // 状态文本
    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, st == ST_FAIL ? RGB(200, 40, 40)
                    : st == ST_DONE ? RGB(20, 140, 80)
                    : st == ST_PAUSE ? RGB(190, 120, 20)
                    : RGB(90, 95, 105));
    wchar_t stBuf[256];
    std::string progTxt;
    if (st == ST_DOWN && pct >= 0) progTxt = fmtProgress(pct, done, total);
    else if (st == ST_DONE) progTxt = fmtBytes(done);
    else if (st == ST_FAIL) progTxt = t->getError();
    else if (st == ST_PAUSE && pct >= 0 && total > 0) progTxt = fmtProgress(pct, done, total) + " 已暂停";
    else if (st == ST_DOWN) progTxt = fmtBytes(done);
    swprintf(stBuf, 256, L"%s  %s  |  %s", u2w(statusText(st)).c_str(),
             u2w(t->protocol).c_str(), u2w(progTxt).c_str());
    RECT stRc{DP(10), rowTop + DP(26), W - DP(160), rowTop + DP(26) + DP(16)};
    DrawTextW(dc, stBuf, -1, &stRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 进度条
    RECT barRc{DP(200), rowTop + DP(27), W - DP(160), rowTop + DP(27) + DP(13)};
    if (barRc.right > barRc.left) {
        drawRounded(dc, barRc, RGB(236, 238, 242), RGB(226, 228, 233), DP(6));
        double shown = pct;
        if (st == ST_DONE) shown = 100.0;
        else if (st == ST_CANCEL) shown = pct > 0 ? pct : 0;
        else if (shown < 0) shown = 0;
        if (shown > 100.0) shown = 100.0;
        if (shown > 0.5) {
            RECT fillRc = barRc;
            fillRc.right = barRc.left + (LONG)((barRc.right - barRc.left) * shown / 100.0);
            if (fillRc.right - fillRc.left < DP(8)) fillRc.right = fillRc.left + DP(8);
            COLORREF c = (st == ST_DONE) ? RGB(60, 175, 110)
                          : (st == ST_FAIL) ? RGB(212, 96, 96)
                          : (st == ST_PAUSE) ? RGB(226, 168, 74)
                          : RGB(52, 120, 218);
            drawRounded(dc, fillRc, c, c, DP(6));
        }
    }

    // 按钮
    std::wstring ml = mainBtnLabel(st);
    std::wstring sl = subBtnLabel(st);
    SelectObject(dc, g_fontSmall);
    if (!sl.empty()) {
        RECT sr = btnSubRect(W, rowTop);
        drawRounded(dc, sr, RGB(246, 247, 250), RGB(210, 214, 222), DP(6));
        SetTextColor(dc, RGB(60, 64, 72));
        DrawTextW(dc, sl.c_str(), -1, &sr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    if (!ml.empty()) {
        RECT mr = btnMainRect(W, rowTop);
        COLORREF bgc = (st == ST_DONE) ? RGB(60, 175, 110) : RGB(52, 120, 218);
        if (st == ST_FAIL) bgc = RGB(226, 168, 74);
        drawRounded(dc, mr, bgc, bgc, DP(6));
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, ml.c_str(), -1, &mr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

void updateListScrollbar() {
    if (!g_hList) return;
    RECT rc;
    GetClientRect(g_hList, &rc);
    int totalH = (int)g_tasks.size() * rowHeight();
    int pageH = rc.bottom;
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = totalH > 0 ? totalH - 1 : 0;
    si.nPage = (UINT)pageH;
    si.nPos = g_listScroll;
    SetScrollInfo(g_hList, SB_VERT, &si, TRUE);
}

LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            int W = rc.right, H = rc.bottom;

            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, W > 0 ? W : 1, H > 0 ? H : 1);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            HBRUSH bgBr = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(mem, &rc, bgBr);
            DeleteObject(bgBr);

            int y = -g_listScroll;
            for (size_t i = 0; i < g_tasks.size(); ++i) {
                int rowTop = y + (int)i * rowHeight();
                if (rowTop > H) break;
                if (rowTop + rowHeight() < 0) continue;
                drawRow(mem, (int)i, g_tasks[i], W, rowTop);
            }
            if (g_tasks.empty()) {
                SelectObject(mem, g_fontUi);
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, RGB(150, 155, 165));
                RECT trc = rc;
                DrawTextW(mem, L"暂无任务：粘贴 URL 后点击「解析全部」", -1, &trc,
                          DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }

            BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            int row = (my + g_listScroll) / rowHeight();
            if (row < 0 || row >= (int)g_tasks.size()) return 0;
            RECT rc;
            GetClientRect(h, &rc);
            int W = rc.right;
            int rowTop = -g_listScroll + row * rowHeight();
            RECT mr = btnMainRect(W, rowTop);
            RECT sr = btnSubRect(W, rowTop);
            TaskPtr t = g_tasks[(size_t)row];
            int st = t->status.load();
            bool inMain = (mx >= mr.left && mx <= mr.right && my >= mr.top && my <= mr.bottom);
            bool inSub = (mx >= sr.left && mx <= sr.right && my >= sr.top && my <= sr.bottom);
            if (inMain) {
                if (st == ST_WAIT || st == ST_FAIL || st == ST_PAUSE) {
                    startTask(t);
                } else if (st == ST_DOWN) {
                    pauseTask(t);
                }
                InvalidateRect(h, nullptr, FALSE);
            } else if (inSub) {
                if (st == ST_DONE) openTaskFile(t);
                else if (st != ST_CANCEL) cancelTask(t);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            RECT rc;
            GetClientRect(h, &rc);
            int totalH = (int)g_tasks.size() * rowHeight();
            int maxScroll = totalH > rc.bottom ? totalH - rc.bottom : 0;
            g_listScroll -= delta / WHEEL_DELTA * DP(48);
            if (g_listScroll < 0) g_listScroll = 0;
            if (g_listScroll > maxScroll) g_listScroll = maxScroll;
            updateListScrollbar();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            RECT rc;
            GetClientRect(h, &rc);
            int totalH = (int)g_tasks.size() * rowHeight();
            int maxScroll = totalH > rc.bottom ? totalH - rc.bottom : 0;
            switch (LOWORD(w)) {
                case SB_LINEUP: g_listScroll -= DP(24); break;
                case SB_LINEDOWN: g_listScroll += DP(24); break;
                case SB_PAGEUP: g_listScroll -= rc.bottom; break;
                case SB_PAGEDOWN: g_listScroll += rc.bottom; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si;
                    ZeroMemory(&si, sizeof(si));
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    GetScrollInfo(h, SB_VERT, &si);
                    g_listScroll = si.nTrackPos;
                    break;
                }
                default: break;
            }
            if (g_listScroll < 0) g_listScroll = 0;
            if (g_listScroll > maxScroll) g_listScroll = maxScroll;
            updateListScrollbar();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            // 手型光标在按钮上
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            int row = (my + g_listScroll) / rowHeight();
            bool onBtn = false;
            if (row >= 0 && row < (int)g_tasks.size()) {
                RECT rc;
                GetClientRect(h, &rc);
                int rowTop = -g_listScroll + row * rowHeight();
                RECT mr = btnMainRect(rc.right, rowTop);
                RECT sr = btnSubRect(rc.right, rowTop);
                onBtn = (mx >= mr.left && mx <= mr.right && my >= mr.top && my <= mr.bottom) ||
                        (mx >= sr.left && mx <= sr.right && my >= sr.top && my <= sr.bottom);
            }
            static bool lastOn = false;
            if (onBtn != lastOn) {
                lastOn = onBtn;
                SetCursor(LoadCursorW(nullptr, onBtn ? IDC_HAND : IDC_ARROW));
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---------- 布局 ----------
void layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = DP(12);
    int y = DP(8);

    MoveWindow(g_hLblUrls, pad, y, W - pad * 2, DP(18), TRUE);
    y += DP(20);
    MoveWindow(g_hUrls, pad, y, W - pad * 2, DP(84), TRUE);
    y += DP(90);

    int bw = DP(96), bh = DP(28), gap = DP(8);
    int x = pad;
    MoveWindow(GetDlgItem(h, IDC_PARSE), x, y, bw, bh, TRUE);
    x += bw + gap;
    MoveWindow(GetDlgItem(h, IDC_DOWNLOAD), x, y, bw, bh, TRUE);
    x += bw + gap;
    MoveWindow(GetDlgItem(h, IDC_CANCELALL), x, y, bw, bh, TRUE);
    MoveWindow(GetDlgItem(h, IDC_CLEARLOG), W - pad - bw, y, bw, bh, TRUE);
    y += bh + DP(10);

    MoveWindow(g_hLblDir, pad, y + DP(4), DP(70), DP(20), TRUE);
    MoveWindow(g_hDir, pad + DP(74), y, W - pad * 2 - DP(74) - DP(84) - DP(6), DP(26), TRUE);
    MoveWindow(GetDlgItem(h, IDC_BROWSE), W - pad - DP(84), y, DP(84), DP(26), TRUE);
    y += DP(32);

    MoveWindow(g_hStatus, pad, y, W - pad * 2, DP(20), TRUE);
    y += DP(24);

    MoveWindow(g_hLblList, pad, y, W - pad * 2, DP(18), TRUE);
    y += DP(20);

    int listH = H - y - DP(150);
    if (listH < DP(140)) listH = DP(140);
    MoveWindow(g_hList, pad, y, W - pad * 2, listH, TRUE);
    y += listH + DP(8);

    MoveWindow(g_hLblTotal, pad, y + DP(6), DP(64), DP(20), TRUE);
    MoveWindow(g_hProg, pad + DP(68), y + DP(4), W - pad * 2 - DP(68), DP(22), TRUE);
    y += DP(34);

    int logH = H - y - DP(8);
    if (logH < DP(70)) logH = DP(70);
    MoveWindow(g_hLog, pad, y, W - pad * 2, logH, TRUE);

    updateListScrollbar();
    InvalidateRect(g_hList, nullptr, FALSE);
}

// ---------- 定时器：刷新 ----------
void onTimer() {
    std::string line;
    int n = 0;
    while (takeLog(line) && n < 200) {
        appendLog(line);
        ++n;
    }
    {
        std::vector<TaskPtr> add;
        {
            std::lock_guard<std::mutex> lk(g_pendMtx);
            add.swap(g_pending);
        }
        if (!add.empty()) {
            for (auto& t : add) g_tasks.push_back(t);
            g_doneNotified = false;
            updateListScrollbar();
        }
    }
    InvalidateRect(g_hList, nullptr, FALSE);
    refreshTotal();
}

// ---------- 主窗口过程 ----------
void createControls(HWND h) {
    g_hLblUrls = CreateWindowExW(0, L"STATIC",
                                 L"视频页面 URL（每行一个，支持多地址、多视频页面）：",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)IDC_LBL_URLS, g_hInst, nullptr);
    g_hUrls = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                  ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL,
                              0, 0, 10, 10, h, (HMENU)IDC_URLS, g_hInst, nullptr);

    CreateWindowExW(0, L"BUTTON", L"解析全部",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 10, 10, h, (HMENU)IDC_PARSE, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"全部下载",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 10, 10, h, (HMENU)IDC_DOWNLOAD, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"全部取消",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 10, 10, h, (HMENU)IDC_CANCELALL, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"清空日志",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 10, 10, h, (HMENU)IDC_CLEARLOG, g_hInst, nullptr);

    g_hLblDir = CreateWindowExW(0, L"STATIC", L"保存位置：", WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, h, (HMENU)IDC_LBL_DIR, g_hInst, nullptr);
    g_hDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             0, 0, 10, 10, h, (HMENU)IDC_DIR, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"浏览...",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 10, 10, h, (HMENU)IDC_BROWSE, g_hInst, nullptr);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, h, (HMENU)IDC_STATUS, g_hInst, nullptr);

    g_hLblList = CreateWindowExW(0, L"STATIC", L"任务列表（最多同时下载 3 个，其余排队）：",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h,
                                 (HMENU)IDC_LBL_LIST, g_hInst, nullptr);

    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, kListClass, L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                              0, 0, 10, 10, h, (HMENU)IDC_LIST, g_hInst, nullptr);

    g_hLblTotal = CreateWindowExW(0, L"STATIC", L"总进度：", WS_CHILD | WS_VISIBLE,
                                  0, 0, 10, 10, h, (HMENU)IDC_LBL_TOTAL, g_hInst, nullptr);
    g_hProg = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 10, 10, h, (HMENU)IDC_TOTAL, g_hInst, nullptr);
    SendMessageW(g_hProg, PBM_SETRANGE32, 0, 100);
    SendMessageW(g_hProg, PBM_SETPOS, 0, 0);

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             0, 0, 10, 10, h, (HMENU)IDC_LOG, g_hInst, nullptr);

    // 字体
    int base = -MulDiv(9, g_dpi, 72);
    int small = -MulDiv(8, g_dpi, 72);
    g_fontUi = CreateFontW(base, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(small, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontBold = CreateFontW(base, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    HWND ctrls[] = {g_hLblUrls, g_hUrls, g_hLblDir, g_hDir, g_hStatus,
                    g_hLblList, g_hLblTotal, g_hLog};
    for (HWND c : ctrls) {
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    }
    SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    for (int id = IDC_PARSE; id <= IDC_BROWSE; ++id) {
        HWND c = GetDlgItem(h, id);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    }
}

bool hasActiveTasks(int& activeCount) {
    activeCount = 0;
    for (auto& t : g_tasks) {
        int s = t->status.load();
        if (s == ST_DOWN || s == ST_WAIT || s == ST_PAUSE) ++activeCount;
    }
    return activeCount > 0;
}

void initDefaultDir() {
    wchar_t path[MAX_PATH * 2];
    std::wstring dir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, path))) {
        dir = std::wstring(path) + L"\\Downloads";
        if (!fileExistsW(dir)) dir = path;
    }
    if (dir.empty()) dir = L"D:\\视频下载";
    SetWindowTextW(g_hDir, dir.c_str());
    setSaveDir(w2u(dir));
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWnd = h;
            g_hMain = h;
            createControls(h);
            initDefaultDir();
            layout(h);
            SetTimer(h, 1, 150, nullptr);
            appendLog("DPlayer 视频解析下载工具（C++ 单文件版）已启动");
            appendLog("提示：粘贴页面地址后点「解析全部」，再点「全部下载」。");
            return 0;
        }
        case WM_SIZE:
            layout(h);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            mmi->ptMinTrackSize.x = DP(780);
            mmi->ptMinTrackSize.y = DP(560);
            return 0;
        }
        case WM_TIMER:
            if (w == 1) onTimer();
            return 0;
        case WM_COMMAND: {
            switch (LOWORD(w)) {
                case IDC_PARSE: doParseAll(); break;
                case IDC_DOWNLOAD: doDownloadAll(); break;
                case IDC_CANCELALL: doCancelAll(); break;
                case IDC_CLEARLOG: clearLog(); break;
                case IDC_BROWSE: doBrowseDir(); break;
                case IDC_DIR: {
                    if (HIWORD(w) == EN_CHANGE) {
                        std::string d = uiSaveDir();
                        if (!d.empty()) setSaveDir(d);
                    }
                    break;
                }
                default: break;
            }
            return 0;
        }
        case WM_CLOSE: {
            int active = 0;
            if (hasActiveTasks(active)) {
                wchar_t buf[256];
                swprintf(buf, 256, L"还有 %d 个任务未完成，确定退出吗？\n（退出会中断下载，未完成文件将被清理）", active);
                if (MessageBoxW(h, buf, L"确认退出", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                for (auto& t : g_tasks) {
                    int s = t->status.load();
                    if (s == ST_DOWN || s == ST_WAIT || s == ST_PAUSE) t->cancelReq = true;
                }
                Sleep(300);
            }
            DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(h, 1);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int runGui(HINSTANCE hInst, int nCmdShow) {
    g_hInst = hInst;
    // 动态取系统 DPI，避免旧头文件缺符号
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * PFN_GetDpiForSystem)();
        UINT dpi = 96;
        if (u32) {
            PFN_GetDpiForSystem gd =
                (PFN_GetDpiForSystem)GetProcAddress(u32, "GetDpiForSystem");
            if (gd) dpi = gd();
        }
        g_dpi = (int)(dpi > 0 ? dpi : 96);
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW lw;
    ZeroMemory(&lw, sizeof(lw));
    lw.cbSize = sizeof(lw);
    lw.style = CS_HREDRAW | CS_VREDRAW;
    lw.lpfnWndProc = ListProc;
    lw.hInstance = hInst;
    lw.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lw.hbrBackground = nullptr;
    lw.lpszClassName = kListClass;
    if (!RegisterClassExW(&lw)) return 1;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(247, 248, 250));
    wc.lpszClassName = L"DPDownloaderMainWnd";
    wc.lpszMenuName = nullptr;
    HANDLE hIcon = LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MAIN), IMAGE_ICON,
                              GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HANDLE hIconSm = LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MAIN), IMAGE_ICON,
                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!hIcon) {
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hIcon = (HICON)hIcon;
    wc.hIconSm = (HICON)hIconSm;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"DPDownloaderMainWnd",
                                L"DPlayer 视频解析下载工具（C++ 版）",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                DP(980), DP(760),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
