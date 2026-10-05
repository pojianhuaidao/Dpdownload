# -*- coding: utf-8 -*-
"""
DPlayer 视频解析下载工具（多地址并发下载 + 暂停续传版）

功能：
1. 界面：多行 URL 输入（每行一个页面地址）+ 「解析全部」按钮
2. 解析：逐行抓取页面 HTML，解析 DPlayer 真实视频地址与协议
   - 优先 data-config 属性（JSON 中的 video.url / video.type）
   - 其次 new DPlayer({...}) / dplayer({...}) 配置段
   - 协议识别：m3u8(HLS) / mp4 直链(DIRECT) / flv(FLV)
3. 任务列表：每个任务一行，显示 序号/文件名/协议/状态，
   行内嵌进度条 + 「暂停/继续」按钮 + 「取消」按钮
4. 全部下载：任务级并发，最多同时 3 个，其余排队
5. 下载引擎：
   - mp4 直链：requests 流式下载 + Range 断点续传（.part 文件，
     暂停保留 part，继续时 HEAD 取总大小并用 Range 续传）
   - m3u8 无加密：解析分片列表逐片下载（.tmp_目录），
     暂停保留已下载分片，继续跳过已存在分片；完成后 ffmpeg 合并为 mp4
   - m3u8 带 AES-128 加密：退化为 ffmpeg 全量下载，不支持暂停续传
   - FLV / 未知协议：ffmpeg 全量下载，不支持暂停续传
   - 取消任务：删除该任务的 .part 与临时分片文件
6. 文件名冲突：同目录同名自动加序号（如 标题(1).mp4）
7. 全部 UI 更新走线程安全队列，界面不卡死；
   ffmpeg 子进程使用 CREATE_NO_WINDOW 隐藏窗口

依赖：requests（未安装自动回退 urllib）
启动：python dplayer_downloader.py
"""

import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor

import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

# requests 优先；未安装时回退到标准库 urllib
try:
    import requests
    HAS_REQUESTS = True
    # 模块级 Session 单例：连接复用，避免高频新建连接导致端口耗尽
    _SESSION = requests.Session()
except ImportError:
    HAS_REQUESTS = False
    _SESSION = None

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
)

MAX_CONCURRENT = 3          # 任务级最大并发数
CHUNK_SIZE = 64 * 1024      # 流式下载块大小
TASK_TIMEOUT = 7200         # 全局任务总超时上限（秒），防任务无限挂起

# 任务状态
ST_WAIT = "等待下载"
ST_DOWN = "下载中"
ST_PAUSE = "已暂停"
ST_DONE = "已完成"
ST_FAIL = "失败"
ST_CANCEL = "已取消"

# ========== 解析与识别核心逻辑 ==========

def get_page_html(page_url):
    """抓取页面 HTML，返回文本内容；失败抛异常"""
    headers = {
        "User-Agent": USER_AGENT,
        "Referer": page_url,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
    }
    if HAS_REQUESTS:
        resp = _SESSION.get(page_url, headers=headers, timeout=15,
                            allow_redirects=True)
        resp.raise_for_status()
        if resp.encoding is None or resp.encoding.lower() == "iso-8859-1":
            resp.encoding = resp.apparent_encoding or "utf-8"
        return resp.text

    import urllib.request
    req = urllib.request.Request(page_url, headers=headers)
    with urllib.request.urlopen(req, timeout=15) as resp:
        raw = resp.read()
        charset = resp.headers.get_content_charset() or "utf-8"
        try:
            return raw.decode(charset)
        except (LookupError, UnicodeDecodeError):
            return raw.decode("utf-8", errors="replace")


def extract_page_title(html):
    """从 HTML 中提取 <title> 内容，用于默认输出文件名"""
    m = re.search(r"<title[^>]*>(.*?)</title>", html, re.IGNORECASE | re.DOTALL)
    if not m:
        return None
    title = re.sub(r"\s+", " ", m.group(1)).strip()
    return title or None


def parse_data_config(html):
    """优先解析 data-config 属性：<div data-config='{...json...}'> 或 data-config="..." """
    pat = r"data-config\s*=\s*(['\"])(.*?)\1"
    for m in re.finditer(pat, html, re.IGNORECASE):
        raw = m.group(2)
        raw = raw.replace("&quot;", '"').replace("&amp;", "&").replace("&#39;", "'")
        raw = raw.strip()
        try:
            cfg = json.loads(raw)
        except (json.JSONDecodeError, ValueError):
            continue
        video = cfg.get("video") if isinstance(cfg, dict) else None
        if isinstance(video, dict) and video.get("url"):
            return video
    return None


def parse_dplayer_js(html):
    """匹配 new DPlayer({...}) 或 dplayer 配置段，提取 video.url"""
    candidates = []
    lower = html.lower()
    for kw in ["new dplayer", "new dplayer({", "dplayer({"]:
        idx = lower.find(kw)
        if idx >= 0:
            start = lower.find("{", idx)
            if start >= 0:
                depth = 0
                i = start
                in_str = False
                str_ch = ""
                for i in range(start, len(html)):
                    ch = html[i]
                    if in_str:
                        if ch == "\\":
                            i += 1
                            continue
                        if ch == str_ch:
                            in_str = False
                        continue
                    if ch in ("'", '"', "`"):
                        in_str = True
                        str_ch = ch
                        continue
                    if ch == "{":
                        depth += 1
                    elif ch == "}":
                        depth -= 1
                        if depth == 0:
                            candidates.append(html[start:i + 1])
                            break

    if not candidates:
        for m in re.finditer(r"dplayer\s*\(\s*(\{.*?\})\s*\)", html,
                             re.IGNORECASE | re.DOTALL):
            candidates.append(m.group(1))

    for block in candidates:
        vm = re.search(r"['\"]?video['\"]?\s*:\s*\{([^}]*)\}", block,
                       re.IGNORECASE | re.DOTALL)
        if not vm:
            continue
        video_body = vm.group(1)
        url_m = re.search(r"['\"]?url['\"]?\s*:\s*['\"]([^'\"]+)['\"]",
                          video_body, re.IGNORECASE)
        if not url_m:
            continue
        url = url_m.group(1).strip()
        type_m = re.search(r"['\"]?type['\"]?\s*:\s*['\"]([^'\"]+)['\"]",
                           video_body, re.IGNORECASE)
        vtype = type_m.group(1).strip() if type_m else ""
        if url:
            return {"url": url, "type": vtype}
    return None


def resolve_url(raw_url, page_url):
    """将相对地址解析为完整绝对地址"""
    if not raw_url:
        return raw_url
    if raw_url.startswith(("http://", "https://")):
        return raw_url
    if raw_url.startswith("//"):
        return "https:" + raw_url
    return urllib.parse.urljoin(page_url, raw_url)


def detect_protocol(url, vtype):
    """协议自动识别：返回 HLS / FLV / DIRECT / UNKNOWN"""
    lower_url = (url or "").lower()
    lower_type = (vtype or "").lower()
    if "m3u8" in lower_url or "hls" in lower_type:
        return "HLS"
    if lower_url.endswith(".flv") or "flv" in lower_type:
        return "FLV"
    if lower_url.endswith((".mp4", ".webm", ".ogg", ".mov", ".m4v")):
        return "DIRECT"
    if "mp4" in lower_type:
        return "DIRECT"
    return "UNKNOWN"


def sanitize_filename(name):
    """清洗文件名，去掉 Windows 非法字符"""
    name = re.sub(r'[\\/:*?"<>|]', "_", name).strip()
    name = re.sub(r"\s+", "_", name)
    return name[:120] or "video"


def unique_path(save_dir, base_name):
    """同目录同名自动加序号：标题(1).mp4；同时避免与 .part 冲突"""
    def exists(p):
        return os.path.exists(p) or os.path.exists(p + ".part")
    candidate = os.path.join(save_dir, base_name)
    if not exists(candidate):
        return candidate
    stem, ext = os.path.splitext(base_name)
    i = 1
    while True:
        candidate = os.path.join(save_dir, "{} ({}){}".format(stem, i, ext))
        if not exists(candidate):
            return candidate
        i += 1


# ========== 下载任务 ==========

class DownloadTask:
    """单个下载任务（数据 + 线程控制事件）"""

    def __init__(self, tid, page_url, video_url, protocol, title):
        self.tid = tid
        self.page_url = page_url
        self.video_url = video_url
        self.protocol = protocol      # HLS / FLV / DIRECT / UNKNOWN
        self.title = title or "video"
        self.base_name = sanitize_filename(self.title) + ".mp4"
        self.status = ST_WAIT
        self.progress = 0.0
        self.note = ""
        self.error = ""
        self.pause_event = threading.Event()
        self.cancel_event = threading.Event()
        self.ffmpeg_proc = None
        self.final_path = None
        self.started_at = None        # 任务实际开始时间（全局超时用）

    def pause(self):
        """请求暂停（下载循环检测后生效）"""
        self.pause_event.set()

    def cancel(self):
        """请求取消（下载循环检测后清理文件）"""
        self.cancel_event.set()


# ========== 下载引擎 ==========

class DownloadManager:
    """任务级并发下载引擎，最多 MAX_CONCURRENT 个同时下载"""

    def __init__(self, app):
        self.app = app
        self.pool = ThreadPoolExecutor(max_workers=MAX_CONCURRENT,
                                       thread_name_prefix="dl")
        # 防重复提交：正在执行的任务集合 + 锁
        self._active = set()
        self._active_lock = threading.Lock()

    def start_task(self, task):
        """提交任务到线程池（自动排队）；已在执行的任务拒绝重复提交"""
        with self._active_lock:
            if task.tid in self._active:
                return False
            self._active.add(task.tid)
        task.pause_event.clear()
        self.pool.submit(self._run_task, task)
        return True

    def _run_task(self, task):
        """任务主流程：按协议分派（含全局总超时兜底）"""
        try:
            if task.cancel_event.is_set():
                self._set_status(task, ST_CANCEL)
                return
            task.status = ST_DOWN
            task.started_at = time.time()
            self.app.post("task_status", task.tid, ST_DOWN, task.note)
            try:
                if task.protocol == "DIRECT":
                    self._run_direct(task)
                elif task.protocol == "HLS":
                    self._run_hls(task)
                else:
                    self._run_ffmpeg(task)
            except Exception as exc:  # noqa: BLE001
                if task.cancel_event.is_set():
                    self._cleanup_task(task)
                    self._set_status(task, ST_CANCEL)
                else:
                    task.error = str(exc)
                    self.app.log("[失败] 任务 #{} 异常：{}".format(task.tid, exc))
                    self._set_status(task, ST_FAIL)
            # 协议方法返回后兜底：仍处于下载态且超总时长上限 -> 终止置失败
            if (task.status == ST_DOWN
                    and time.time() - task.started_at > TASK_TIMEOUT):
                task.error = "任务总耗时超过 {} 秒上限，已终止".format(TASK_TIMEOUT)
                self.app.log("[失败] 任务 #{} {}".format(task.tid, task.error))
                self._cleanup_task(task)
                self._set_status(task, ST_FAIL)
        finally:
            # 任务结束（成功/失败/取消/暂停）后移出 active 集合
            with self._active_lock:
                self._active.discard(task.tid)

    # ---------- mp4 直链：断点续传 ----------
    def _run_direct(self, task):
        save_dir = self.app.save_dir_var.get().strip()
        if task.final_path:
            final = task.final_path
        else:
            final = unique_path(save_dir, task.base_name)
            task.final_path = final
        part = final + ".part"
        total = None
        accepts_range = True

        # 暂停即退出（保留 .part）
        if task.pause_event.is_set():
            self._set_status(task, ST_PAUSE)
            return

        # HEAD 探测总大小与 Range 支持
        try:
            total, accepts_range = self._head_info(task.video_url, task.page_url)
        except Exception as exc:  # noqa: BLE001
            self.app.log("[提示] 任务 #{} HEAD 探测失败（{}），将从头下载".format(task.tid, exc))

        offset = 0
        if os.path.exists(part):
            offset = os.path.getsize(part)
            if offset > 0 and accepts_range:
                self.app.log("任务 #{} 检测到断点 {} 字节，继续续传".format(task.tid, offset))
            else:
                offset = 0

        try:
            resp, stream, close, status_code = self._open_stream(
                task.video_url, offset, accepts_range, task.page_url)
        except Exception as exc:  # noqa: BLE001
            task.error = str(exc)
            self.app.log("[失败] 任务 #{} 无法建立下载连接：{}".format(task.tid, exc))
            self._set_status(task, ST_FAIL)
            return

        # 服务器忽略 Range 返回 200：从头重下
        if offset > 0 and status_code is not None and status_code != 206:
            self.app.log("任务 #{} 服务器不支持续传，从头下载".format(task.tid))
            offset = 0
            try:
                resp.close()
                resp, stream, close, status_code = self._open_stream(
                    task.video_url, 0, False, task.page_url)
            except Exception as exc:  # noqa: BLE001
                task.error = str(exc)
                self._set_status(task, ST_FAIL)
                return

        mode = "ab" if offset > 0 else "wb"
        downloaded = offset
        if not total:
            self.app.post("task_progress", task.tid, -1)
        last_data = time.time()
        try:
            with open(part, mode) as f:
                while True:
                    # 无数据看门狗：60 秒未收到任何数据 -> 判定网络断流
                    if time.time() - last_data > 60:
                        close()
                        task.error = "网络断流：60 秒无数据"
                        self.app.log("[失败] 任务 #{} {}".format(task.tid, task.error))
                        self._cleanup_task(task)
                        self._set_status(task, ST_FAIL)
                        return
                    # 全局总超时看门狗
                    if time.time() - task.started_at > TASK_TIMEOUT:
                        close()
                        task.error = "任务总耗时超过 {} 秒上限，已终止".format(TASK_TIMEOUT)
                        self.app.log("[失败] 任务 #{} {}".format(task.tid, task.error))
                        self._cleanup_task(task)
                        self._set_status(task, ST_FAIL)
                        return
                    try:
                        chunk = next(stream)
                    except StopIteration:
                        break
                    if task.pause_event.is_set():
                        close()
                        self._set_status(task, ST_PAUSE)
                        return
                    if task.cancel_event.is_set():
                        close()
                        self._cleanup_task(task)
                        self._set_status(task, ST_CANCEL)
                        return
                    if not chunk:
                        continue
                    last_data = time.time()
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total:
                        pct = downloaded * 100.0 / total
                        task.progress = min(100.0, pct)
                        self.app.post("task_progress", task.tid, task.progress)
                        if int(pct * 50) != int((downloaded - len(chunk)) * 50.0 / total):
                            self.app.log("  任务 #{} {:.1f}% （{:.1f} MB / {:.1f} MB）".format(
                                task.tid, pct, downloaded / 1048576.0, total / 1048576.0))
                    else:
                        task.progress = 0.0
                        if downloaded % (5 * 1048576) < 65536:
                            self.app.post("task_progress", task.tid, -1)
                            self.app.log("  任务 #{} 已下载 {:.1f} MB（未知总大小）".format(
                                task.tid, downloaded / 1048576.0))
        except Exception as exc:  # noqa: BLE001
            try:
                close()
            except Exception:  # noqa: BLE001
                pass
            task.error = str(exc)
            self._set_status(task, ST_FAIL)
            return

        try:
            close()
        except Exception:  # noqa: BLE001
            pass

        # 完成：改名 .part -> 正式名
        if os.path.exists(part) and os.path.getsize(part) > 0:
            os.rename(part, final)
            task.progress = 100.0
            self.app.post("task_progress", task.tid, 100.0)
            self.app.log("[完成] 任务 #{} 保存为：{}".format(task.tid, final))
            self._set_status(task, ST_DONE)
        else:
            task.error = "下载结果为空"
            self._cleanup_task(task)
            self._set_status(task, ST_FAIL)

    def _head_info(self, url, referer):
        """HEAD 请求获取总大小与是否支持 Range"""
        headers = {"User-Agent": USER_AGENT, "Referer": referer}
        if HAS_REQUESTS:
            resp = _SESSION.head(url, headers=headers, timeout=15,
                                 allow_redirects=True)
            resp.raise_for_status()
            total = int(resp.headers.get("Content-Length") or 0) or None
            accepts_range = resp.headers.get("Accept-Ranges", "").lower() == "bytes"
            return total, accepts_range
        import urllib.request
        req = urllib.request.Request(url, headers=headers, method="HEAD")
        with urllib.request.urlopen(req, timeout=15) as resp:
            total = int(resp.headers.get("Content-Length") or 0) or None
            accepts_range = resp.headers.get("Accept-Ranges", "").lower() == "bytes"
            return total, accepts_range

    def _open_stream(self, url, offset, accepts_range, referer):
        """打开流式下载连接；返回 (resp, stream_iter, close_fn, status_code)"""
        headers = {"User-Agent": USER_AGENT, "Referer": referer}
        if offset > 0 and accepts_range:
            headers["Range"] = "bytes={}-".format(offset)
        if HAS_REQUESTS:
            resp = _SESSION.get(url, headers=headers, stream=True,
                                timeout=(10, 60), allow_redirects=True)
            resp.raise_for_status()
            return (resp, resp.iter_content(chunk_size=CHUNK_SIZE),
                    resp.close, resp.status_code)
        import urllib.request
        req = urllib.request.Request(url, headers=headers)
        resp = urllib.request.urlopen(req, timeout=30)
        stream = iter(lambda: resp.read(CHUNK_SIZE), b"")
        return (resp, stream, resp.close, getattr(resp, "status", None))

    # ---------- m3u8 无加密：分片下载 + 续传 ----------
    def _run_hls(self, task):
        save_dir = self.app.save_dir_var.get().strip()
        if task.final_path:
            final = task.final_path
        else:
            final = unique_path(save_dir, task.base_name)
            task.final_path = final
        tmp_dir = os.path.join(save_dir, ".tmp_{}_{}".format(
            task.tid, sanitize_filename(task.title)))

        if task.pause_event.is_set():
            self._set_status(task, ST_PAUSE)
            return

        # 抓取 m3u8 文本
        try:
            text = self._fetch_text(task.video_url, task.page_url)
        except Exception as exc:  # noqa: BLE001
            task.error = str(exc)
            self._set_status(task, ST_FAIL)
            return

        # 检测 AES-128 加密 -> 退化 ffmpeg（不支持暂停）
        if re.search(r"#EXT-X-KEY:.*METHOD\s*=\s*AES-128", text, re.IGNORECASE):
            task.note = "AES-128 加密，ffmpeg 全量下载（不支持暂停续传）"
            self.app.post("task_status", task.tid, ST_DOWN, task.note)
            self.app.log("[提示] 任务 #{} {}（不支持暂停）".format(task.tid, task.note))
            self._run_ffmpeg(task)
            return

        segs = self._parse_ts_list(text, task.video_url)
        if not segs:
            task.error = "m3u8 中未解析到分片列表"
            self._set_status(task, ST_FAIL)
            return
        self.app.log("任务 #{} 解析到 {} 个分片".format(task.tid, len(segs)))

        os.makedirs(tmp_dir, exist_ok=True)
        try:
            for idx, seg_url in enumerate(segs):
                if task.cancel_event.is_set():
                    self._cleanup_task(task)
                    self._set_status(task, ST_CANCEL)
                    return
                if task.pause_event.is_set():
                    self._set_status(task, ST_PAUSE)
                    return
                # 全局总超时检查
                if time.time() - task.started_at > TASK_TIMEOUT:
                    task.error = "任务总耗时超过 {} 秒上限，已终止".format(TASK_TIMEOUT)
                    self.app.log("[失败] 任务 #{} {}".format(task.tid, task.error))
                    self._cleanup_task(task)
                    self._set_status(task, ST_FAIL)
                    return
                ts_path = os.path.join(tmp_dir, "{:04d}.ts".format(idx))
                # 续传：已存在且非空的跳过
                if os.path.exists(ts_path) and os.path.getsize(ts_path) > 0:
                    continue
                try:
                    self._download_seg(seg_url, ts_path, task)
                except Exception as exc:  # noqa: BLE001
                    if task.cancel_event.is_set():
                        self._cleanup_task(task)
                        self._set_status(task, ST_CANCEL)
                        return
                    task.error = "分片 {} 下载失败：{}".format(idx, exc)
                    self._set_status(task, ST_FAIL)
                    return
                task.progress = (idx + 1) * 100.0 / len(segs)
                self.app.post("task_progress", task.tid, task.progress)
        except Exception as exc:  # noqa: BLE001
            task.error = str(exc)
            self._set_status(task, ST_FAIL)
            return

        # 合并为 mp4
        try:
            self._merge_ts(tmp_dir, final)
        except Exception as exc:  # noqa: BLE001
            task.error = "合并失败：{}".format(exc)
            self.app.log("[错误] 任务 #{} {}".format(task.tid, task.error))
            self._set_status(task, ST_FAIL)
            return
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

        if os.path.exists(final) and os.path.getsize(final) > 0:
            task.progress = 100.0
            self.app.post("task_progress", task.tid, 100.0)
            self.app.log("[完成] 任务 #{} 保存为：{}".format(task.tid, final))
            self._set_status(task, ST_DONE)
        else:
            task.error = "合并结果为空"
            self._set_status(task, ST_FAIL)

    def _fetch_text(self, url, referer):
        """抓取文本内容（m3u8 列表）"""
        headers = {"User-Agent": USER_AGENT, "Referer": referer}
        if HAS_REQUESTS:
            resp = _SESSION.get(url, headers=headers, timeout=15,
                                allow_redirects=True)
            resp.raise_for_status()
            if resp.encoding is None or resp.encoding.lower() == "iso-8859-1":
                resp.encoding = resp.apparent_encoding or "utf-8"
            return resp.text
        import urllib.request
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.read().decode("utf-8", errors="replace")

    @staticmethod
    def _parse_ts_list(text, m3u8_url):
        """解析 m3u8 分片 URL 列表（处理相对地址与嵌套多码率列表）"""
        segs = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.endswith(".m3u8"):
                # 多码率主列表：取第一路（简化处理）
                continue
            segs.append(resolve_url(line, m3u8_url))
        return segs

    def _download_seg(self, seg_url, ts_path, task):
        """下载单个 ts 分片到临时目录；失败重试 2 次（退避 1s/2s）"""
        headers = {"User-Agent": USER_AGENT, "Referer": task.page_url}
        last_exc = None
        for attempt in range(3):  # 首次 + 2 次重试
            try:
                if HAS_REQUESTS:
                    resp = _SESSION.get(seg_url, headers=headers,
                                        timeout=(10, 30), allow_redirects=True)
                    resp.raise_for_status()
                    with open(ts_path, "wb") as f:
                        for chunk in resp.iter_content(chunk_size=CHUNK_SIZE):
                            if task.cancel_event.is_set():
                                resp.close()
                                return
                            if chunk:
                                f.write(chunk)
                    resp.close()
                else:
                    import urllib.request
                    req = urllib.request.Request(seg_url, headers=headers)
                    with urllib.request.urlopen(req, timeout=30) as resp:
                        with open(ts_path, "wb") as f:
                            while True:
                                chunk = resp.read(CHUNK_SIZE)
                                if not chunk:
                                    break
                                if task.cancel_event.is_set():
                                    return
                                f.write(chunk)
                return
            except Exception as exc:  # noqa: BLE001
                last_exc = exc
                if task.cancel_event.is_set():
                    raise
                if attempt < 2:
                    time.sleep(1 + attempt)  # 退避 1s / 2s
        raise last_exc

    def _merge_ts(self, tmp_dir, final):
        """合并分片：优先 ffmpeg concat，回退二进制拼接"""
        if self._ffmpeg_available():
            try:
                list_file = os.path.join(tmp_dir, "concat.txt")
                lines = []
                for name in sorted(os.listdir(tmp_dir)):
                    if name.endswith(".ts"):
                        full = os.path.join(tmp_dir, name).replace("\\", "/")
                        full = full.replace("'", "'\\''")
                        lines.append("file '{}'".format(full))
                with open(list_file, "w", encoding="utf-8") as f:
                    f.write("\n".join(lines) + "\n")
                cmd = ["ffmpeg", "-y", "-f", "concat", "-safe", "0",
                       "-i", list_file, "-c", "copy", final]
                proc = subprocess.run(
                    cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                    timeout=600)
                if proc.returncode == 0 and os.path.exists(final):
                    return
            except Exception:  # noqa: BLE001
                pass
            # ffmpeg 失败回退二进制拼接
            if os.path.exists(final):
                os.remove(final)
        # 二进制拼接
        with open(final, "wb") as out:
            for name in sorted(os.listdir(tmp_dir)):
                if name.endswith(".ts"):
                    p = os.path.join(tmp_dir, name)
                    with open(p, "rb") as fp:
                        shutil.copyfileobj(fp, out)

    # ---------- ffmpeg 全量下载（加密 m3u8 / FLV / 未知） ----------
    def _run_ffmpeg(self, task):
        if not self._ffmpeg_available():
            task.error = "未找到 ffmpeg"
            self.app.log("[错误] 任务 #{} 需要 ffmpeg，未在 PATH 中找到。".format(task.tid))
            self._set_status(task, ST_FAIL)
            return

        save_dir = self.app.save_dir_var.get().strip()
        if task.final_path:
            final = task.final_path
        else:
            final = unique_path(save_dir, task.base_name)
            task.final_path = final

        # 尝试获取总时长用于确定进度
        duration = None
        try:
            if task.protocol == "HLS":
                text = self._fetch_text(task.video_url, task.page_url)
                duration = self._sum_extinf(text)
        except Exception:  # noqa: BLE001
            duration = None
        if not duration:
            duration = self._probe_duration(task.video_url)

        ff_headers = "User-Agent: {}\r\nReferer: {}\r\n".format(USER_AGENT, task.page_url)
        cmd = ["ffmpeg", "-headers", ff_headers, "-stats", "-i", task.video_url,
               "-c", "copy", final, "-y"]
        self.app.log("任务 #{} 执行：{}".format(task.tid, " ".join(cmd)))
        proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        task.ffmpeg_proc = proc

        # 后台读 stderr，主循环可及时响应取消
        stderr_q = queue.Queue()

        def _reader():
            for line in proc.stderr:
                stderr_q.put(line)

        threading.Thread(target=_reader, daemon=True).start()

        last_time = ""
        last_activity = time.time()
        while True:
            if task.cancel_event.is_set():
                if proc.poll() is None:
                    proc.terminate()
                self._cleanup_task(task)
                self._set_status(task, ST_CANCEL)
                return
            try:
                line = stderr_q.get(timeout=0.3)
            except queue.Empty:
                if proc.poll() is not None and stderr_q.empty():
                    break
                # 看门狗：60 秒无任何新进度且 ffmpeg 仍在运行 -> 判定网络断流
                if proc.poll() is None and time.time() - last_activity > 60:
                    self.app.log("[失败] 任务 #{} ffmpeg 60 秒无新进度，判定网络断流，已终止".format(task.tid))
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except Exception:  # noqa: BLE001
                        proc.kill()
                    task.error = "网络断流：ffmpeg 60 秒无新进度"
                    self._set_status(task, ST_FAIL)
                    return
                # 全局总超时检查
                if (proc.poll() is None
                        and time.time() - task.started_at > TASK_TIMEOUT):
                    self.app.log("[失败] 任务 #{} 超过总时长上限，已终止".format(task.tid))
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except Exception:  # noqa: BLE001
                        proc.kill()
                    task.error = "任务总耗时超过 {} 秒上限，已终止".format(TASK_TIMEOUT)
                    self._cleanup_task(task)
                    self._set_status(task, ST_FAIL)
                    return
                continue
            m = re.search(r"time=(\d+:\d+:\d+(?:\.\d+)?)", line)
            if m:
                t = m.group(1)
                if t != last_time:
                    last_time = t
                    last_activity = time.time()
                    secs = self._parse_ffmpeg_time(t)
                    if duration and secs is not None:
                        task.progress = min(100.0, secs * 100.0 / duration)
                        self.app.post("task_progress", task.tid, task.progress)

        proc.wait()
        if proc.returncode == 0 and os.path.exists(final) and os.path.getsize(final) > 0:
            task.progress = 100.0
            self.app.post("task_progress", task.tid, 100.0)
            self.app.log("[完成] 任务 #{} 保存为：{}".format(task.tid, final))
            self._set_status(task, ST_DONE)
        else:
            task.error = "ffmpeg 退出码 {}".format(proc.returncode)
            self._cleanup_task(task)
            self._set_status(task, ST_FAIL)

    @staticmethod
    def _sum_extinf(text):
        """累加 #EXTINF 分片时长（秒）"""
        total = 0.0
        for m in re.finditer(r"#EXTINF:\s*([0-9]+(?:\.[0-9]+)?)", text):
            total += float(m.group(1))
        return total if total > 0 else None

    def _probe_duration(self, url):
        """ffprobe 探测总时长（秒）；失败返回 None"""
        try:
            cmd = ["ffprobe", "-v", "error", "-show_entries",
                   "format=duration",
                   "-of", "default=noprint_wrappers=1:nokey=1", url]
            out = subprocess.check_output(
                cmd, stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                errors="replace",
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                timeout=60)
            val = float(out.strip())
            return val if val > 0 else None
        except Exception:  # noqa: BLE001
            return None

    @staticmethod
    def _parse_ffmpeg_time(time_str):
        """把 ffmpeg 的 HH:MM:SS[.xx] 或 MM:SS[.xx] 转成秒数"""
        parts = time_str.split(":")
        try:
            if len(parts) == 3:
                return int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
            if len(parts) == 2:
                return int(parts[0]) * 60 + float(parts[1])
        except (ValueError, IndexError):
            return None
        return None

    def _ffmpeg_available(self):
        """检查 ffmpeg 是否在 PATH"""
        try:
            subprocess.run(["ffmpeg", "-version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False,
                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            return True
        except FileNotFoundError:
            return False

    def _cleanup_task(self, task):
        """取消/失败清理：删除 .part、临时分片目录与 ffmpeg 半成品"""
        if task.final_path:
            part = task.final_path + ".part"
            if os.path.exists(part):
                try:
                    os.remove(part)
                except OSError:
                    pass
            # ffmpeg 直出 final：失败/取消时删除半成品，避免残留坏文件
            if os.path.exists(task.final_path):
                try:
                    os.remove(task.final_path)
                except OSError:
                    pass
        tmp_dir = os.path.join(self.app.save_dir_var.get().strip(),
                               ".tmp_{}_{}".format(task.tid, sanitize_filename(task.title)))
        if os.path.isdir(tmp_dir):
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def _set_status(self, task, status):
        task.status = status
        self.app.post("task_status", task.tid, status, task.note)


# ========== 图形界面 ==========

class TaskRow:
    """任务列表中的一行：信息 + 进度条 + 暂停/继续 + 取消"""

    def __init__(self, parent, task, app):
        self.task = task
        self.app = app
        self.frame = tk.Frame(parent, padx=6, pady=3)
        self.frame.pack(fill=tk.X)

        info_text = "#{} {} [{}]".format(task.tid, task.base_name, task.protocol)
        self.info_label = tk.Label(self.frame, text=info_text, anchor="w",
                                   font=("Microsoft YaHei", 9))
        self.info_label.pack(fill=tk.X)

        bar_row = tk.Frame(self.frame)
        bar_row.pack(fill=tk.X)
        self.bar = ttk.Progressbar(bar_row, mode="determinate", maximum=100)
        self.bar.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        self.pause_btn = tk.Button(bar_row, text="暂停", width=6,
                                   command=self.toggle_pause)
        self.pause_btn.pack(side=tk.LEFT, padx=2)
        self.cancel_btn = tk.Button(bar_row, text="取消", width=6,
                                    command=self.do_cancel)
        self.cancel_btn.pack(side=tk.LEFT, padx=2)
        self.retry_btn = tk.Button(bar_row, text="重试", width=6,
                                   command=self.do_retry,
                                   state=tk.DISABLED)
        self.retry_btn.pack(side=tk.LEFT, padx=2)

        self.status_label = tk.Label(self.frame, text=task.status, anchor="w",
                                     fg="#666666", font=("Microsoft YaHei", 8))
        self.status_label.pack(fill=tk.X)

    def toggle_pause(self):
        """暂停 / 继续 切换"""
        t = self.task
        if t.status == ST_DOWN or t.status == ST_WAIT:
            if t.status == ST_WAIT:
                t.pause_event.set()
                t.status = ST_PAUSE
                self.app.post("task_status", t.tid, ST_PAUSE, t.note)
            else:
                t.pause()
                self.app.log("[提示] 任务 #{} 暂停请求已发送，保留 .part/分片 ...".format(t.tid))
        elif t.status == ST_PAUSE:
            t.pause_event.clear()
            self.app.manager.start_task(t)
            self.app.log("[提示] 任务 #{} 继续下载".format(t.tid))

    def do_cancel(self):
        """取消/再次：活动任务点击取消；非活动任务点击"再次"重新下载"""
        t = self.task
        if t.status in (ST_DOWN, ST_WAIT, ST_PAUSE):
            t.cancel()
            # 终止本程序启动的 ffmpeg 子进程
            proc = t.ffmpeg_proc
            if proc is not None and proc.poll() is None:
                try:
                    proc.terminate()
                except Exception:  # noqa: BLE001
                    try:
                        proc.close()
                    except Exception:  # noqa: BLE001
                        pass
            if t.status == ST_PAUSE or t.status == ST_WAIT:
                # 未在运行：直接清理并置状态
                self.app.manager._cleanup_task(t)
                t.status = ST_CANCEL
                self.app.post("task_status", t.tid, ST_CANCEL, t.note)
            else:
                self.app.log("[提示] 任务 #{} 取消请求已发送，正在清理文件 ...".format(t.tid))
        elif t.status in (ST_DONE, ST_FAIL, ST_CANCEL):
            # 非活动任务：按钮为"再次"，与重试相同（含清理后重下）
            self.do_retry()

    def do_retry(self):
        """重试/再次：彻底删除旧下载内容后重新下载"""
        t = self.task
        if t.status not in (ST_FAIL, ST_CANCEL, ST_DONE):
            return
        # 彻底删除此前下载内容（.part / 分片临时目录 / ffmpeg 半成品 final）
        self.app.manager._cleanup_task(t)
        self.app.log("[提示] 任务 #{} 已清理旧下载内容，重新下载".format(t.tid))
        # 重置失败痕迹与进度
        t.error = ""
        t.note = ""
        t.progress = 0.0
        t.final_path = None
        # 复位线程控制事件（防止残留 cancel/pause 导致提交后立即终止）
        t.cancel_event.clear()
        t.pause_event.clear()
        t.ffmpeg_proc = None
        self.bar.stop()
        self.bar.configure(mode="determinate", value=0)
        t.status = ST_WAIT
        self.app.post("task_status", t.tid, ST_WAIT, t.note)
        self.app.manager.start_task(t)
        self.app.log("[提示] 任务 #{} 已重新提交下载（从头开始）".format(t.tid))


class DplayerDownloaderApp:
    """主窗口：多行 URL + 解析全部 + 任务列表 + 全部下载"""

    def _set_app_icon(self):
        """设置窗口/任务栏图标（DPlayer 官方图标）。

        路径解析顺序：
        1. 打包运行时：sys._MEIPASS 解压目录下的 dplayer_icon.ico
        2. exe 同目录（未打包开发运行时为 python 所在目录）
        3. 开发目录 D:\\DPlayer下载器\\output\\dplayer_icon.ico
        找不到图标时静默跳过，不影响程序启动。
        """
        candidates = []
        if hasattr(sys, "_MEIPASS"):
            candidates.append(os.path.join(sys._MEIPASS, "dplayer_icon.ico"))
        candidates.append(os.path.join(os.path.dirname(sys.executable),
                                       "dplayer_icon.ico"))
        candidates.append(r"D:\DPlayer下载器\output\dplayer_icon.ico")
        for ico in candidates:
            if os.path.isfile(ico):
                try:
                    self.root.iconbitmap(ico)
                except Exception:  # noqa: BLE001 图标加载失败静默跳过
                    pass
                break

    def __init__(self, root):
        self.root = root
        self.root.title("DPlayer 视频解析下载工具（多任务版）")
        self.root.geometry("860x640")
        self.root.minsize(720, 520)
        # 窗口初始化后设置 DPlayer 图标（窗口/任务栏）
        self._set_app_icon()

        self.tasks = []          # 任务对象列表（仅主线程读写）
        self.rows = {}           # tid -> TaskRow
        self.tid_counter = 0
        self._parse_pending = 0  # 尚未完成的解析线程计数
        self.ui_queue = queue.Queue()
        self.manager = DownloadManager(self)

        # ---- 顶部：URL 多行输入 ----
        top = tk.Frame(root, padx=10, pady=6)
        top.pack(fill=tk.X)
        tk.Label(top, text="视频页面 URL（每行一个，支持多地址）：",
                 anchor="w").pack(fill=tk.X)
        self.url_text = tk.Text(top, height=5, font=("Consolas", 10))
        self.url_text.pack(fill=tk.X, pady=(2, 4))

        # ---- 按钮区 ----
        btns = tk.Frame(root, padx=10, pady=4)
        btns.pack(fill=tk.X)
        self.parse_btn = tk.Button(btns, text="解析全部", width=12,
                                   command=self.do_parse_all)
        self.parse_btn.pack(side=tk.LEFT)
        self.download_all_btn = tk.Button(btns, text="全部下载", width=12,
                                          command=self.do_download_all)
        self.download_all_btn.pack(side=tk.LEFT, padx=8)
        self.cancel_all_btn = tk.Button(btns, text="全部取消", width=12,
                                        command=self.do_cancel_all)
        self.cancel_all_btn.pack(side=tk.LEFT, padx=8)
        tk.Button(btns, text="清空日志", width=10,
                  command=self.clear_log).pack(side=tk.RIGHT)

        # ---- 保存位置行 ----
        save_row = tk.Frame(root, padx=10, pady=4)
        save_row.pack(fill=tk.X)
        tk.Label(save_row, text="保存位置：").pack(side=tk.LEFT)
        default_dir = os.path.expanduser("~") + os.sep + "Downloads"
        if not os.path.isdir(default_dir):
            default_dir = os.path.expanduser("~")
        self.save_dir_var = tk.StringVar(value=default_dir)
        tk.Entry(save_row, textvariable=self.save_dir_var,
                 width=50).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=6)
        tk.Button(save_row, text="浏览...", width=8,
                  command=self.browse_save_dir).pack(side=tk.LEFT)

        # ---- 状态行 ----
        self.status_var = tk.StringVar(value="就绪：粘贴多个 URL 后点「解析全部」，再点「全部下载」")
        tk.Label(root, textvariable=self.status_var, anchor="w",
                 fg="#333333", padx=10).pack(fill=tk.X)

        # ---- 任务列表 ----
        list_label = tk.Label(root, text="任务列表（最多同时下载 3 个，其余排队）：",
                              anchor="w", padx=10).pack(fill=tk.X)
        list_frame = tk.Frame(root, padx=6)
        list_frame.pack(fill=tk.X)
        self.canvas = tk.Canvas(list_frame, height=190, highlightthickness=0)
        self.canvas.pack(side=tk.LEFT, fill=tk.X, expand=True)
        vsb = ttk.Scrollbar(list_frame, orient="vertical", command=self.canvas.yview)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self.canvas.configure(yscrollcommand=vsb.set)
        self.task_container = tk.Frame(self.canvas)
        self._win = self.canvas.create_window((0, 0), window=self.task_container,
                                              anchor="nw")
        self.task_container.bind("<Configure>",
                                 lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>",
                         lambda e: self.canvas.itemconfigure(self._win, width=e.width))

        # ---- 总进度行：总进度条 + 全部完成提示按钮 ----
        total_row = tk.Frame(root, padx=10, pady=4)
        total_row.pack(fill=tk.X)
        tk.Label(total_row, text="总进度：").pack(side=tk.LEFT)
        self.total_bar = ttk.Progressbar(total_row, mode="determinate",
                                         maximum=100, value=0)
        self.total_bar.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))
        self.done_all_btn = tk.Button(total_row, text="全部完成", width=10,
                                      state=tk.DISABLED,
                                      command=self.show_done_summary)
        self.done_all_btn.pack(side=tk.LEFT)

        # ---- 日志区 ----
        log_frame = tk.Frame(root, padx=10, pady=6)
        log_frame.pack(fill=tk.BOTH, expand=True)
        self.log_text = scrolledtext.ScrolledText(
            log_frame, state=tk.DISABLED, wrap=tk.WORD, height=8,
            font=("Consolas", 9))
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # 主线程定时刷新队列
        self.root.after(100, self._drain_queue)

        # 初始刷新总进度（任务为空：进度条 0、按钮禁用）
        self._refresh_total_progress()

        if not HAS_REQUESTS:
            self.log("提示：未安装 requests，已自动回退标准库 urllib。")
            self.log("建议执行：pip install requests 以获得更稳定的网络能力。")

    # ---------- 队列与日志 ----------
    def post(self, kind, *args):
        """子线程安全地向 UI 队列投递消息"""
        self.ui_queue.put((kind, args))

    def log(self, text):
        self.post("log", text)

    def _drain_queue(self):
        """主线程消费 UI 队列，更新界面"""
        try:
            while True:
                kind, args = self.ui_queue.get_nowait()
                if kind == "log":
                    text = args[0]
                    self.log_text.configure(state=tk.NORMAL)
                    self.log_text.insert(tk.END, text + "\n")
                    self.log_text.see(tk.END)
                    self.log_text.configure(state=tk.DISABLED)
                elif kind == "add_task":
                    task = args[0]
                    self.tasks.append(task)
                    self.rows[task.tid] = TaskRow(self.task_container, task, self)
                    self.log("任务 #{} 已加入：{} [{}]".format(
                        task.tid, task.base_name, task.protocol))
                    self._refresh_total_progress()
                elif kind == "parsed_task":
                    page_url, video_url, protocol, title = args
                    self.tid_counter += 1
                    task = DownloadTask(self.tid_counter, page_url,
                                        video_url, protocol, title)
                    self.tasks.append(task)
                    self.rows[task.tid] = TaskRow(self.task_container, task, self)
                    self.log("任务 #{} 已加入：{} [{}]".format(
                        task.tid, task.base_name, task.protocol))
                    self._refresh_total_progress()
                elif kind == "parse_done":
                    self._parse_pending = max(0, self._parse_pending - 1)
                    if self._parse_pending == 0:
                        self.parse_btn.configure(state=tk.NORMAL)
                        self.set_status("解析完成，点「全部下载」开始（同时最多 3 个）")
                elif kind == "task_status":
                    tid, status, note = args
                    row = self.rows.get(tid)
                    if row:
                        row.status_label.configure(
                            text=status + ("（" + note + "）" if note else ""))
                        if status in (ST_DONE, ST_FAIL, ST_CANCEL):
                            row.pause_btn.configure(state=tk.DISABLED,
                                                    text="暂停")
                            row.cancel_btn.configure(state=tk.NORMAL,
                                                     text="再次")
                            row.retry_btn.configure(
                                state=(tk.NORMAL if status == ST_FAIL
                                       else tk.DISABLED))
                        elif status == ST_PAUSE:
                            row.pause_btn.configure(state=tk.NORMAL, text="继续")
                            row.cancel_btn.configure(state=tk.NORMAL, text="取消")
                            row.retry_btn.configure(state=tk.DISABLED)
                        elif status == ST_DOWN:
                            row.pause_btn.configure(state=tk.NORMAL, text="暂停")
                            row.cancel_btn.configure(state=tk.NORMAL, text="取消")
                            row.retry_btn.configure(state=tk.DISABLED)
                        elif status == ST_WAIT:
                            row.pause_btn.configure(state=tk.NORMAL, text="暂停")
                            row.cancel_btn.configure(state=tk.NORMAL, text="取消")
                            row.retry_btn.configure(state=tk.DISABLED)
                    if status == ST_DONE:
                        self.set_status("任务 #{} 下载完成".format(tid))
                    # 任意任务状态变化后刷新总进度（终态数/总数实时更新）
                    self._refresh_total_progress()
                elif kind == "task_progress":
                    tid, pct = args
                    row = self.rows.get(tid)
                    if row:
                        if pct is None or pct < 0:
                            row.bar.configure(mode="indeterminate")
                            row.bar.start(12)
                        else:
                            if row.bar.cget("mode") == "indeterminate":
                                row.bar.stop()
                            row.bar.configure(mode="determinate", value=pct)
                elif kind == "status":
                    self.status_var.set(args[0])
        except queue.Empty:
            pass
        self.root.after(100, self._drain_queue)

    def set_status(self, text):
        self.status_var.set(text)

    def _refresh_total_progress(self):
        """刷新总进度：已完成（终态）任务数 / 总任务数。

        任意任务状态变化或任务列表增删后调用；全部进入终态时
        进度条置满、状态栏显示「全部完成」并激活提示按钮。
        """
        total = len(self.tasks)
        if total == 0:
            # 任务为空：进度 0，按钮不可用
            self.total_bar.configure(value=0)
            self.done_all_btn.configure(state=tk.DISABLED)
            return
        done = sum(1 for t in self.tasks
                   if t.status in (ST_DONE, ST_FAIL, ST_CANCEL))
        pct = done * 100.0 / total
        self.total_bar.configure(value=pct)
        if done == total:
            # 全部进入终态：进度条置满 + 状态栏提示 + 激活按钮
            self.total_bar.configure(value=100)
            self.done_all_btn.configure(state=tk.NORMAL)
            self.set_status("全部完成")
        else:
            self.done_all_btn.configure(state=tk.DISABLED)

    def show_done_summary(self):
        """全部完成提示按钮回调：弹窗统计成功/失败/取消数，关闭后恢复置灰"""
        ok = sum(1 for t in self.tasks if t.status == ST_DONE)
        fail = sum(1 for t in self.tasks if t.status == ST_FAIL)
        cancel = sum(1 for t in self.tasks if t.status == ST_CANCEL)
        messagebox.showinfo(
            "全部完成",
            "成功 {} 个 / 失败 {} 个 / 已取消 {} 个".format(ok, fail, cancel))
        # 窗口关闭或点击确定后，按钮恢复禁用（等待下一次状态变化重新计算）
        self.done_all_btn.configure(state=tk.DISABLED)

    def clear_log(self):
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.configure(state=tk.DISABLED)

    def browse_save_dir(self):
        cur = self.save_dir_var.get().strip()
        chosen = filedialog.askdirectory(
            title="选择保存目录",
            initialdir=cur if os.path.isdir(cur) else os.path.expanduser("~"))
        if chosen:
            self.save_dir_var.set(chosen)
            self.log("保存位置已设置为：{}".format(chosen))

    # ---------- 解析全部 ----------
    def do_parse_all(self):
        urls = [ln.strip() for ln in self.url_text.get("1.0", "end").splitlines()
                if ln.strip()]
        if not urls:
            self.log("[错误] 请先在输入框中粘贴至少一个 URL。")
            return
        for u in urls:
            if not u.startswith(("http://", "https://")):
                self.log("[错误] 跳过非法 URL：{}（必须以 http(s):// 开头）".format(u))
        valid = [u for u in urls if u.startswith(("http://", "https://"))]
        if not valid:
            return
        self.parse_btn.configure(state=tk.DISABLED)
        self.log("开始解析 {} 个页面地址 ...".format(len(valid)))
        self.set_status("正在解析 ...")
        self._parse_pending = len(valid)
        for u in valid:
            threading.Thread(target=self._parse_one_worker,
                             args=(u,), daemon=True).start()

    def _parse_one_worker(self, page_url):
        """解析单个页面（子线程），成功后投递 add_task"""
        try:
            html = get_page_html(page_url)
        except Exception as exc:  # noqa: BLE001
            self.log("[错误] 解析失败 {}：{}".format(page_url, exc))
            self.log("提示：站点可能反爬、需要登录、超时或证书异常；可用浏览器 F12 -> Network -> media/xhr 手动获取。")
            self.post("parse_done", None)
            return

        video = parse_data_config(html)
        source = "data-config 属性"
        if not video:
            video = parse_dplayer_js(html)
            source = "new DPlayer / dplayer 配置段"
        if not video:
            self.log("[错误] 未在页面中找到 DPlayer 配置：{}".format(page_url))
            self.log("提示：请用浏览器 F12 -> Network -> media/xhr 手动查找真实地址。")
            self.post("parse_done", None)
            return

        video_url = resolve_url(video["url"], page_url)
        protocol = detect_protocol(video_url, video.get("type", ""))
        title = extract_page_title(html) or "video"
        self.log("解析成功：{} [{}]".format(video_url, protocol))
        self.post("parsed_task", page_url, video_url, protocol, title)
        self.post("parse_done", None)

    # ---------- 全部取消 ----------
    def do_cancel_all(self):
        """全部取消：对所有活动任务发起取消并终止 ffmpeg 子进程"""
        if not self.tasks:
            self.log("[错误] 暂无任务。")
            return
        cancelled = 0
        for t in self.tasks:
            if t.status in (ST_DOWN, ST_WAIT, ST_PAUSE):
                t.cancel()
                # 终止本程序启动的 ffmpeg 子进程（不用 taskkill）
                proc = t.ffmpeg_proc
                if proc is not None and proc.poll() is None:
                    try:
                        proc.terminate()
                    except Exception:  # noqa: BLE001
                        try:
                            proc.close()
                        except Exception:  # noqa: BLE001
                            pass
                if t.status == ST_PAUSE or t.status == ST_WAIT:
                    # 未在运行：直接清理并置状态
                    self.manager._cleanup_task(t)
                    t.status = ST_CANCEL
                    self.post("task_status", t.tid, ST_CANCEL, t.note)
                cancelled += 1
        if cancelled:
            self.log("已对 {} 个活动任务发起取消，ffmpeg 子进程已终止".format(cancelled))
            self.set_status("全部取消：任务将清理并复位为可再次下载状态")
        else:
            self.set_status("当前没有活动任务")

    # ---------- 全部下载 ----------
    def do_download_all(self):
        if not self.tasks:
            self.log("[错误] 暂无已解析任务，请先「解析全部」。")
            return
        save_dir = self.save_dir_var.get().strip()
        if not save_dir or not os.path.isdir(save_dir):
            self.log("[错误] 保存目录无效：{}".format(save_dir))
            return
        started = 0
        for t in self.tasks:
            if t.status in (ST_WAIT, ST_PAUSE) and not t.cancel_event.is_set():
                # start_task 内部对已在执行的任务拒绝重复提交
                if self.manager.start_task(t):
                    started += 1
        self.log("已提交 {} 个任务下载（最多同时 3 个）".format(started))
        if started == 0:
            self.set_status("没有等待下载的任务")


def main():
    root = tk.Tk()
    DplayerDownloaderApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
