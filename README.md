# DPlayer 下载器（C++ 版）

把 DPlayer 播放页的视频抓下来：解析页面拿到直链 / HLS 播放列表，再逐段下载并合并成可播放文件。

当前版本已用 **C++17 + Win32 + WinHTTP** 重写，产出**单文件绿色 exe**（约 1.4 MB，静态链接，只依赖系统 DLL，无需安装任何运行库）。

## 特性

- 粘贴 DPlayer 播放页地址即可解析，自动识别直链 MP4 与 HLS（m3u8）
- HLS 多分片并发下载、乱序顺序保持、断点感知失败重试
- 并发下载同名分片不会互相覆盖（已修复的同名冲突缺陷）
- AES-128 加密的 HLS 需要外部 FFmpeg（放到程序同目录或 PATH 即可），未加密流不需要
- 图形界面基于 Win32 原生控件，启动快、无控制台窗口

## 目录结构

```
DPlayer下载器/
├── cpp/                     # C++ 源码（项目主体）
│   ├── core.h / core.cpp    # 通用工具、字符串与文件处理
│   ├── http.h / http.cpp    # WinHTTP 封装（GET / 分段下载）
│   ├── parser.h/.cpp        # 页面解析、直链与 m3u8 提取
│   ├── engine.cpp           # 下载引擎（并发分片、合并、进度回传）
│   ├── gui.cpp              # Win32 界面
│   ├── main.cpp             # 入口
│   ├── app.rc               # 图标资源脚本
│   └── dplayer_icon.ico     # 程序图标
├── .github/workflows/       # CI：推送 main 自动编译并发 Release
└── DPlayer下载器C.exe        # 本地编译产物（不进仓库）
```

## 本地编译

需要 MinGW-w64（g++ 支持 C++17）与 binutils 的 `windres`：

```bash
cd cpp
windres -i app.rc -o app_res.o
g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ \
    core.cpp http.cpp parser.cpp engine.cpp gui.cpp main.cpp app_res.o \
    -o DPlayerDownloaderC.exe \
    -lcomctl32 -lwinhttp -lshell32 -lole32 -lshlwapi -luuid -lgdi32 -luser32 -ladvapi32 -s
```

## 发布

推送到 `main` 后，GitHub Actions 会用 MSYS2 / MinGW-w64 自动编译并创建 Release，产物为 `DPlayerDownloaderC.exe`。

## 说明

早期的 Python 实现（`dplayer_downloader.py` / `dplayer_url_grabber.py`）已退役，仅在本机 `_legacy_python/` 留档，不纳入版本管理。
