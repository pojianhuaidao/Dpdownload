# -*- coding: utf-8 -*-
"""
DPlayer 视频页面真实地址抓取工具

功能：
1. 输入视频页面 URL（命令行参数或交互输入）
2. 抓取页面 HTML（带浏览器 UA），解析 DPlayer 播放器配置中的真实视频地址：
   - 优先解析 data-config 属性（其中含 JSON，取 video.url / video.type）
   - 其次匹配页面中的 new DPlayer({...}) 或 dplayer 配置段提取 video.url
3. 协议自动识别：m3u8 / hls -> HLS；.mp4/.webm -> 直链；.flv -> FLV
4. 自动生成对应 PowerShell 下载命令：
   - m3u8 / flv -> ffmpeg -i "地址" -c copy "输出文件名.mp4" -y
   - mp4 直链 -> curl -L "地址" -o "输出文件名.mp4"
   - 输出文件名默认取自页面标题或固定名，可被用户覆盖
5. 输出检测到的视频协议、视频完整地址、生成的完整 PowerShell 命令
6. 抓取失败或解析不到地址时给出清晰错误提示，不报错崩溃
7. 支持 --exec 参数直接执行生成的下载命令（需显式添加）

用法示例：
    python dplayer_url_grabber.py "https://example.com/video.html"
    python dplayer_url_grabber.py "https://example.com/video.html" --output my_video.mp4
    python dplayer_url_grabber.py "https://example.com/video.html" --exec
"""

import argparse
import json
import re
import subprocess
import sys
import urllib.parse

# requests 优先；未安装时回退到标准库 urllib，保证脚本零依赖可运行
try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False

# 浏览器 UA，避免被服务器识别为脚本请求
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
)

# 常见 DPlayer 配置关键词
DPLAYER_KEYS = ["new DPlayer", "dplayer", "DPlayer", "video.url"]


def get_page_html(page_url):
    """抓取页面 HTML，返回文本内容；失败抛异常"""
    headers = {
        "User-Agent": USER_AGENT,
        "Referer": page_url,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
    }
    if HAS_REQUESTS:
        resp = requests.get(page_url, headers=headers, timeout=15,
                            allow_redirects=True)
        resp.raise_for_status()
        # 优先使用页面声明的编码，否则按响应头或默认 utf-8
        if resp.encoding is None or resp.encoding.lower() == "iso-8859-1":
            resp.encoding = resp.apparent_encoding or "utf-8"
        return resp.text

    # 标准库回退：urllib 实现同样能力（UA/Referer/超时/自动跟随重定向）
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
    # 首尾同种引号配对：单引号包裹时内部双引号 JSON 不受影响；
    # 双引号包裹时内部应使用 &quot; 转义，同样不受影响
    pat = r"data-config\s*=\s*(['\"])(.*?)\1"
    for m in re.finditer(pat, html, re.IGNORECASE):
        raw = m.group(2)
        # 去除可能存在的 HTML 转义
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

    # 1) new DPlayer({...}) 的 JS 对象，花括号需要配平
    # 先找所有包含 dplayer 关键词的括号起点
    lower = html.lower()
    for kw in ["new dplayer", "new dplayer({", "dplayer({"]:
        idx = lower.find(kw)
        if idx >= 0:
            # 从第一个 { 开始做花括号配平
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

    # 2) 如果没有配平结果，退而求其次用正则截取较大块
    if not candidates:
        for m in re.finditer(r"dplayer\s*\(\s*(\{.*?\})\s*\)", html,
                             re.IGNORECASE | re.DOTALL):
            candidates.append(m.group(1))

    for block in candidates:
        # 键名可带引号（JSON 形式 "video": 或 JS 裸键 video:）
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
    """协议自动识别：返回 (protocol, is_hls)"""
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
    return name[:120] or "video_output"


def build_download_command(protocol, video_url, out_name):
    """根据协议生成 PowerShell 下载命令"""
    quoted_url = '"' + video_url + '"'
    quoted_out = '"' + out_name + '"'
    if protocol == "HLS":
        cmd = "ffmpeg -i {url} -c copy {out} -y".format(url=quoted_url, out=quoted_out)
    elif protocol == "FLV":
        cmd = "ffmpeg -i {url} -c copy {out} -y".format(url=quoted_url, out=quoted_out)
    elif protocol == "DIRECT":
        # mp4/webm 直链用 curl -L 更可靠（不依赖 ffmpeg 编译包含对应协议）
        cmd = "curl -L {url} -o {out}".format(url=quoted_url, out=quoted_out)
    else:
        # 未知类型默认尝试 ffmpeg
        cmd = "ffmpeg -i {url} -c copy {out} -y".format(url=quoted_url, out=quoted_out)
    return cmd


def execute_command(cmd):
    """执行生成的 PowerShell 下载命令（仅 --exec 时调用）"""
    print("\n[执行下载] 正在执行命令：")
    print(cmd)
    print("-" * 60)
    try:
        proc = subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                              check=False)
        if proc.returncode == 0:
            print("\n[完成] 下载命令执行成功")
        else:
            print("\n[警告] 下载命令执行结束，返回码：{}（{}）".format(
                proc.returncode,
                "成功" if proc.returncode == 0 else "失败，请检查 ffmpeg/curl 是否安装或地址是否可访问"))
    except FileNotFoundError:
        print("\n[错误] 未找到 PowerShell，无法执行下载命令，请手动复制命令执行")
    except Exception as exc:  # noqa: BLE001
        print("\n[错误] 执行下载命令时出错：{}".format(exc))


def main():
    parser = argparse.ArgumentParser(
        description="DPlayer 视频页面真实地址抓取工具：解析视频真实地址并生成 PowerShell 下载命令")
    parser.add_argument("url", nargs="?", help="视频页面 URL（可选，不传则交互输入）")
    parser.add_argument("--output", "-o", help="输出文件名（默认取页面标题，否则 video_output）")
    parser.add_argument("--exec", action="store_true",
                        help="解析后直接执行生成的下载命令（需显式添加此参数）")
    args = parser.parse_args()

    page_url = args.url
    if not page_url:
        try:
            page_url = input("请输入视频页面 URL：").strip()
        except EOFError:
            page_url = ""
    if not page_url:
        print("[错误] 未提供视频页面 URL，程序退出。")
        sys.exit(1)
    if not page_url.startswith(("http://", "https://")):
        print("[错误] URL 必须以 http:// 或 https:// 开头：{}".format(page_url))
        sys.exit(1)

    print("[1/4] 正在抓取页面：{}".format(page_url))
    try:
        html = get_page_html(page_url)
    except requests.exceptions.Timeout:
        print("[错误] 页面抓取超时（15 秒）。可能是网络问题或站点响应慢。")
        print("提示：如需手动获取，可打开浏览器开发者工具(F12) -> Network -> 过滤 media/xhr，")
        print("找到 .m3u8/.mp4/.flv 请求后复制其地址。")
        sys.exit(1)
    except requests.exceptions.RequestException as exc:
        print("[错误] 页面抓取失败：{}".format(exc))
        print("提示：站点可能反爬、需要登录或证书异常；可尝试在浏览器开发者工具(F12) -> ")
        print("Network -> media/xhr 中手动找到视频地址。")
        sys.exit(1)

    print("[2/4] 正在解析 DPlayer 配置...")
    video = parse_data_config(html)
    source = "data-config 属性"
    if not video:
        video = parse_dplayer_js(html)
        source = "new DPlayer / dplayer 配置段"
    if not video:
        print("[错误] 未在页面中找到 DPlayer 配置或 video.url。")
        print("可能原因：")
        print("  1. 页面播放器不是 DPlayer（可能是 video.js / flv.js / hls.js 等）；")
        print("  2. 视频地址由 JS 动态加载（页面 HTML 中不存在）；")
        print("  3. 页面需要登录或反爬拦截。")
        print("提示：请打开浏览器开发者工具(F12) -> Network -> 过滤 media/xhr，")
        print("找到 .m3u8/.mp4/.flv 请求后复制其真实地址，再用 ffmpeg/curl 下载。")
        sys.exit(1)

    raw_url = video["url"]
    vtype = video.get("type", "")
    video_url = resolve_url(raw_url, page_url)
    protocol = detect_protocol(video_url, vtype)

    # 默认输出文件名：用户指定 > 页面标题 > 固定名
    out_name = args.output
    if not out_name:
        title = extract_page_title(html)
        if title:
            out_name = sanitize_filename(title) + ".mp4"
        else:
            out_name = "video_output.mp4"
    if not out_name.lower().endswith((".mp4", ".flv", ".mkv", ".ts", ".webm")):
        out_name = out_name + ".mp4"

    print("[3/4] 检测结果：")
    print("  配置来源  ：{}".format(source))
    print("  视频协议  ：{}".format(protocol))
    print("  视频地址  ：{}".format(video_url))

    cmd = build_download_command(protocol, video_url, out_name)
    print("[4/4] 生成下载命令：")
    print("-" * 60)
    print(cmd)
    print("-" * 60)
    print("提示：请复制上面命令到 PowerShell 中执行（需提前安装 ffmpeg 或 curl）。")
    print("输出文件：{}".format(out_name))

    if args.exec:
        execute_command(cmd)
    else:
        print("\n（未添加 --exec，不执行下载。如需自动执行请加 --exec 参数）")


if __name__ == "__main__":
    main()
