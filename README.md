屏幕捕获 + Web 实时预览

跨语言实现的屏幕捕获服务：捕获桌面画面，以 MJPEG 方式推流，通过浏览器实时查看。

提供三种语言实现，功能一致，按需选用：

| 实现 | 文件 | 平台 | 依赖 |
|------|------|------|------|
| **C# (.NET 8)** | `Program.cs` | Windows | .NET 8 SDK |
| **Python** | `app.py` | 跨平台 | Python 3 + OpenCV / mss / Flask |
| **Go** | `main.go` | Windows | Go 1.22+ |

## 快速开始

### C#（推荐，可发布为单文件 EXE）

```powershell
# 安装 .NET 8 SDK 后
dotnet restore
./build.ps1
```

编译产出在 `dist/` 目录：

- `obs-standalone.exe` — 自包含，无需安装 .NET Runtime（体积较大）
- `obs-small.exe` — 需目标机已安装 .NET 8 Runtime（体积小）

运行：

```powershell
./dist/obs-standalone.exe
```

C# 版特性：
- 系统托盘驻留（透明图标），右键菜单退出
- 单实例限制，重复启动自动忽略

### Python

```bash
pip install -r requirements.txt
python app.py
```

> Python 版使用 `mss` 截屏 + `opencv` 编码，跨平台可用。显示器编号从 1 开始（`MONITOR_INDEX`）。

### Go

```bash
go build -o obs.exe .
./obs.exe
```

> Go 版使用 `kbinani/screenshot` 截屏，仅支持 Windows。

## 访问地址

启动后默认监听 `0.0.0.0:8000`：

| 地址 | 说明 |
|------|------|
| `http://127.0.0.1:8000` | 控制面板 / 预览页 |
| `http://127.0.0.1:8000/stream.mjpg` | MJPEG 视频流（可直接嵌入 `<img>` 标签） |

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `FPS` | `12` | 采样帧率 |
| `JPEG_QUALITY` | `75` | JPEG 质量（20–95） |
| `DISPLAY_INDEX` / `MONITOR_INDEX` | `0`（Go/C#）/ `1`（Python） | 显示器编号 |
| `HOST` | `0.0.0.0` | 监听地址 |
| `PORT` | `8000` | 监听端口 |

示例：

```powershell
$env:FPS="15"; $env:JPEG_QUALITY="80"; ./dist/obs-standalone.exe
```

```bash
FPS=15 JPEG_QUALITY=80 python app.py
```

## 项目结构

```
├── Program.cs          # C# 实现（主程序 + 内嵌 HTML）
├── obs.csproj          # C# 项目文件
├── build.ps1           # C# 编译脚本（PowerShell）
├── app.py              # Python 实现
├── requirements.txt    # Python 依赖
├── templates/
│   └── index.html      # Python 版 Web 页面模板
├── main.go             # Go 实现
├── go.mod              # Go 模块定义
└── README.md
```

## 说明

- 编码方式：MJPEG（逐帧 JPEG 编码）
- 带宽不足时可降低 `FPS` 或 `JPEG_QUALITY`
- 所有实现均监听同一默认端口，请勿同时运行多个版本
