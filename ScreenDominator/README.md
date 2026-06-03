# ScreenDominator - 小小霸屏


## 组件

| 文件 | 说明 |
|------|------|
| `ScreenDominator.exe` | 32 位控制端 — 枚举窗口 / 选择注入路径 |
| `InjectHookDll32.dll` | 注入 32 位目标进程 |
| `InjectHookDll64.dll` | 注入 64 位目标进程 |
| `wow64ext.dll` | 32 位进程操作 64 位进程的辅助库 |

## 功能

程序找到设置了 `WindowDisplayAffinity` 的窗口（即启用了显示保护/反采集的窗口），然后将对应的 hook DLL 注入到目标进程中，处理反截图行为。

## 工作流程

```
1. 初始化 locale，获取 Windows 版本号
2. 从 user32.dll 获取 Set/GetWindowDisplayAffinity
3. 注册隐藏窗口类 "ShareBit ScreenDominator class"
4. 创建隐藏窗口测试 DisplayAffinity API 是否可用
5. 消息循环处理直到 WM_QUIT
6. 检查测试结果，确定系统架构 (32/64位)
7. 启用 SeDebugPrivilege 调试权限
8. 启动后台监控线程
9. 线程每 10 秒枚举一次窗口，筛选 affinity 非 0 的窗口
10. 对每个受保护窗口：获取 PID → 判断 32/64 位 → 注入对应 DLL
```

## 架构

```
               +----------------------+
               | ScreenDominator.exe  |
               | 32-bit controller    |
               +----------+-----------+
                          |
                          v
               +----------------------+
               | Hidden Window        |
               | Message Loop         |
               +----------+-----------+
                          |
                          v
               +----------------------+
               | Worker Thread        |
               | EnumWindows every 10s|
               +----------+-----------+
                          |
                          v
               +----------------------+
               | GetWindowDisplay     |
               | Affinity filter      |
               +----------+-----------+
                          |
               +----------+-----------+
               |                      |
               v                      v
      +----------------+      +----------------+
      | 32-bit target  |      | 64-bit target  |
      +-------+--------+      +-------+--------+
              |                       |
              v                       v
  InjectHookDll32.dll      InjectHookDll64.dll
  via CreateRemoteThread   via wow64ext / X64Call
  / NtCreateThreadEx       / RtlCreateUserThread
```

## 编译

### 前置条件

- MinGW-w64 i686-w64-mingw32 工具链（32 位编译）
- 或任何支持 `-m32` 的 GCC 编译器

### PowerShell 编译命令

```powershell
$env:Path = 'C:\msys64\mingw32\bin;' + $env:Path; & 'C:\msys64\mingw32\bin\gcc.exe' -Wall -O2 -D_WIN32_WINNT=0x0601 -o 'ScreenDominator.exe' 'src\main.c' -lpsapi -luser32 -lkernel32 -lgdi32; Write-Host RC=$LASTEXITCODE
```

### 使用 Makefile 编译

```bash
make
```

或手动编译：

```bash
gcc -Wall -O2 -m32 -D_WIN32_WINNT=0x0601 -c src/main.c -o src/main.o
gcc -m32 -o ScreenDominator.exe src/main.o -lpsapi -luser32 -lkernel32 -lgdi32
```

### 清理

```bash
make clean
```

## 运行

1. 将 `ScreenDominator.exe`、`InjectHookDll32.dll`、`InjectHookDll64.dll`、`wow64ext.dll` 放在同一目录
2. 以管理员权限运行 `ScreenDominator.exe`
3. 程序自动每 10 秒扫描一次受保护窗口并注入
4. 按 Enter 退出

### 命令行参数

- 不带参数启动：扫描全部目标进程
- 带参数启动（如 `wechat` 或 `wechat.exe`）：只注入指定的应用进程

## 注入机制

### 32 位注入

1. `OpenProcess(PROCESS_ALL_ACCESS)` 打开目标
2. 在目标进程分配参数块和 shellcode 内存
3. 参数块包含：`RtlInitUnicodeString` 地址 + `LdrLoadDll` 地址 + DLL 路径
4. 写入 x86 shellcode 到目标进程
5. 通过 `NtCreateThreadEx` 或 `CreateRemoteThread` 执行 shellcode
6. Shellcode 调用 `RtlInitUnicodeString` + `LdrLoadDll` 加载 DLL

### 64 位注入

1. 通过 `wow64ext.dll` 的 `VirtualAllocEx64` / `WriteProcessMemory64` 操作目标
2. 在 64 位 ntdll 中查找 `RtlInitUnicodeString`、`LdrLoadDll`、`RtlCreateUserThread`
3. 写入 x64 shellcode 和参数块到目标进程
4. 通过 `X64Call` 调用 `RtlCreateUserThread` 执行 shellcode
5. Shellcode 在 64 位目标进程中加载 DLL

## 许可

仅供学习研究使用。
