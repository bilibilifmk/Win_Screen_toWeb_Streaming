

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <psapi.h>
#include <wchar.h>
#include <locale.h>
#include <wctype.h>
#include <shellapi.h>

/* ========================================================================
 * Compatibility Definitions
 * ======================================================================== */

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define APP_DISPLAY_NAME L"ScreenDominator"

/* Empty whitelist means inject all protected apps. */
static wchar_t g_whitelistApp[MAX_PATH] = L"";

static void NormalizeAppName(const wchar_t *in, wchar_t *out, size_t outCount)
{
    size_t i = 0;
    if (!in || !out || outCount == 0)
        return;

    while (*in && i + 1 < outCount)
    {
        out[i++] = (wchar_t)towlower(*in++);
    }
    out[i] = L'\0';

    if (i > 4 && _wcsicmp(out + i - 4, L".exe") == 0)
        out[i - 4] = L'\0';
}

static const wchar_t *BaseNameFromPath(const wchar_t *path)
{
    const wchar_t *base = path;
    const wchar_t *p;
    if (!path)
        return L"";
    for (p = path; *p; ++p)
    {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    return base;
}

static BOOL IsWhitelistedApp(const wchar_t *processPath)
{
    wchar_t normalizedTarget[MAX_PATH] = {0};
    wchar_t normalizedWhite[MAX_PATH] = {0};

    if (g_whitelistApp[0] == L'\0')
        return TRUE;

    NormalizeAppName(BaseNameFromPath(processPath), normalizedTarget, MAX_PATH);
    NormalizeAppName(g_whitelistApp, normalizedWhite, MAX_PATH);
    return normalizedTarget[0] != L'\0' && _wcsicmp(normalizedTarget, normalizedWhite) == 0;
}
/* ========================================================================
 * NT Native Definitions
 * ======================================================================== */

typedef LONG NTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef VOID (NTAPI *fnRtlInitUnicodeString)(
    PUNICODE_STRING DestinationString,
    PCWSTR SourceString
);

typedef NTSTATUS (NTAPI *fnLdrLoadDll)(
    PWCHAR PathToFile,
    PULONG Flags,
    PUNICODE_STRING ModuleFileName,
    PHANDLE ModuleHandle
);

typedef NTSTATUS (NTAPI *fnNtCreateThreadEx)(
    OUT PHANDLE ThreadHandle,
    IN ACCESS_MASK DesiredAccess,
    IN LPVOID ObjectAttributes,
    IN HANDLE ProcessHandle,
    IN LPTHREAD_START_ROUTINE lpStartAddress,
    IN LPVOID lpParameter,
    IN BOOL CreateSuspended,
    IN SIZE_T StackZeroBits,
    IN SIZE_T SizeOfStackCommit,
    IN SIZE_T SizeOfStackReserve,
    OUT LPVOID lpBytesBuffer
);

typedef VOID (NTAPI *fnRtlGetNtVersionNumbers)(
    PULONG MajorVersion,
    PULONG MinorVersion,
    PULONG BuildNumber
);

/* ========================================================================
 * wow64ext.dll Function Types
 * ======================================================================== */

typedef DWORD64 (WINAPI *fnVirtualAllocEx64)(
    HANDLE hProcess, DWORD64 lpAddress, SIZE_T dwSize,
    DWORD flAllocationType, DWORD flProtect
);

typedef BOOL (WINAPI *fnWriteProcessMemory64)(
    HANDLE hProcess, DWORD64 lpBaseAddress, LPCVOID lpBuffer,
    SIZE_T nSize, SIZE_T *lpNumberOfBytesWritten
);

typedef DWORD64 (WINAPI *fnGetProcAddress64)(
    DWORD64 hModule, LPCSTR lpProcName
);

typedef DWORD64 (WINAPI *fnGetModuleHandle64)(
    LPCSTR lpModuleName
);

typedef DWORD64 (WINAPI *fnX64Call)(
    DWORD64 func, DWORD64 argCount, ...
);

typedef BOOL (WINAPI *fnVirtualFreeEx64)(
    HANDLE hProcess, DWORD64 lpAddress, SIZE_T dwSize, DWORD dwFreeType
);

/* ========================================================================
 * Global Variables
 * ======================================================================== */

static HWND     g_hWnd[8192];           /* Matched window handles              */
static DWORD    g_windowCount = 0;       /* Current count of matched windows    */
static BOOL     (WINAPI *g_pGetWindowDisplayAffinity)(HWND, DWORD *) = NULL;
static BOOL     (WINAPI *g_pSetWindowDisplayAffinity)(HWND, DWORD) = NULL;
static volatile LONG g_exitFlag = 0;     /* Worker thread exit flag             */
static BOOL     g_is64BitSystem = FALSE; /* Running on 64-bit OS               */
static LONG     g_initResult = 0;        /* Display affinity API test result    */
static HMODULE  g_hWow64Ext = NULL;      /* wow64ext.dll module handle          */
static wchar_t  g_whitelist[64][MAX_PATH]; /* Whitelisted process names          */
static int      g_whitelistCount = 0;    /* Number of whitelist entries          */

/* ========================================================================
 * 32-bit Injection Shellcode (x86 machine code)
 *
 * Written to a 32-bit target process and executed via remote thread.
 * Receives a pointer to a parameter block:
 *   +0x00: address of RtlInitUnicodeString  (4 bytes)
 *   +0x04: address of LdrLoadDll            (4 bytes)
 *   +0x08: DLL path (null-terminated wide string)
 *
 * The stub calls RtlInitUnicodeString to build a UNICODE_STRING from
 * the DLL path, then calls LdrLoadDll to load the DLL.
 * ======================================================================== */

static unsigned char g_shellcode32[] = {
    0x55,                               /* push ebp                          */
    0x8B, 0xEC,                         /* mov ebp, esp                      */
    0x83, 0xEC, 0x10,                   /* sub esp, 0x10  (UNICODE + hModule)*/
    0x8B, 0x75, 0x08,                   /* mov esi, [ebp+8]   pParam         */
    0x8B, 0x1E,                         /* mov ebx, [esi]     RtlInitUniStr  */
    0x8B, 0x7E, 0x04,                   /* mov edi, [esi+4]   LdrLoadDll     */
    0x8D, 0x46, 0x08,                   /* lea eax, [esi+8]   dllPath        */
    0x8D, 0x4D, 0xF8,                   /* lea ecx, [ebp-8]   &uniStr        */
    0x50,                               /* push eax          SourceString    */
    0x51,                               /* push ecx          DestString      */
    0xFF, 0xD3,                         /* call ebx          RtlInitUniStr   */
    0x8D, 0x55, 0xF4,                   /* lea edx, [ebp-0xC] &hModule       */
    0x52,                               /* push edx          ModuleHandle    */
    0x8D, 0x45, 0xF8,                   /* lea eax, [ebp-8]   &uniStr        */
    0x50,                               /* push eax          ModuleFileName  */
    0x6A, 0x00,                         /* push 0            Flags           */
    0x6A, 0x00,                         /* push 0            PathToFile      */
    0xFF, 0xD7,                         /* call edi          LdrLoadDll      */
    0x33, 0xC0,                         /* xor eax, eax                      */
    0x8B, 0xE5,                         /* mov esp, ebp                      */
    0x5D,                               /* pop ebp                           */
    0xC2, 0x04, 0x00                    /* ret 4                             */
};

/* ========================================================================
 * 64-bit Injection Shellcode (x86-64 machine code)
 *
 * Written to a 64-bit target process.  Follows the Microsoft x64 calling
 * convention.  Parameter block layout:
 *   +0x00: address of RtlInitUnicodeString  (8 bytes)
 *   +0x08: address of LdrLoadDll            (8 bytes)
 *   +0x10: DLL path (null-terminated wide string)
 *
 * Stack layout (after sub rsp, 0x40):
 *   [rsp+0x20] UNICODE_STRING (16 bytes on x64)
 *   [rsp+0x30] hModule        (8 bytes)
 *   [rsp+0x00] shadow space   (32 bytes)
 * ======================================================================== */

static unsigned char g_shellcode64[] = {
    0x53,                               /* push rbx                          */
    0x56,                               /* push rsi                          */
    0x57,                               /* push rdi                          */
    0x55,                               /* push rbp                          */
    0x48, 0x83, 0xEC, 0x40,             /* sub rsp, 0x40 (shadow+locals)     */
    0x48, 0x89, 0xCE,                   /* mov rsi, rcx        pParam        */
    0x48, 0x8B, 0x1E,                   /* mov rbx, [rsi]      RtlInitUniStr */
    0x48, 0x8B, 0x7E, 0x08,             /* mov rdi, [rsi+8]    LdrLoadDll    */
    0x48, 0x8D, 0x56, 0x10,             /* lea rdx, [rsi+0x10] dllPath       */
    0x48, 0x8D, 0x4C, 0x24, 0x20,       /* lea rcx, [rsp+0x20] &uniStr       */
    0xFF, 0xD3,                          /* call rbx           RtlInitUniStr  */
    0x33, 0xC9,                          /* xor ecx, ecx       PathToFile=NULL*/
    0x33, 0xD2,                          /* xor edx, edx       Flags=NULL     */
    0x4C, 0x8D, 0x44, 0x24, 0x20,       /* lea r8,  [rsp+0x20] &uniStr       */
    0x4C, 0x8D, 0x4C, 0x24, 0x30,       /* lea r9,  [rsp+0x30] &hModule      */
    0xFF, 0xD7,                          /* call rdi           LdrLoadDll     */
    0x48, 0x83, 0xC4, 0x40,             /* add rsp, 0x40                     */
    0x5D,                               /* pop rbp                           */
    0x5F,                               /* pop rdi                           */
    0x5E,                               /* pop rsi                           */
    0x5B,                               /* pop rbx                           */
    0xC3                                /* ret                               */
};

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */

static DWORD  WINAPI WorkerThread(LPVOID lpParam);
static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg,
                                    WPARAM wParam, LPARAM lParam);
static BOOL   EnableDebugPrivileges(void);
static HANDLE CreateRemoteThreadNt(HANDLE hProcess,
                                    LPTHREAD_START_ROUTINE lpStartAddress,
                                    LPVOID lpParameter);
static int    InjectDll32(DWORD dwProcessId, const wchar_t *dllPath);
static int    InjectDll64(DWORD dwProcessId, const wchar_t *dllPath);
static BOOL   CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam);
static BOOL   IsWhitelisted(const wchar_t *processPath);

/* ========================================================================
 * EnumWindows Callback
 * ======================================================================== */
/* ========================================================================
 * Whitelist Helper
 * ======================================================================== */

/**
 * Returns TRUE if the process should be injected.
 * When g_whitelistCount == 0 (no args) every process is allowed.
 * Otherwise only processes whose executable basename matches an entry
 * in g_whitelist are allowed. Matching is case-insensitive; entries
 * may include or omit the .exe extension.
 */
static BOOL IsWhitelisted(const wchar_t *processPath)
{
    if (g_whitelistCount == 0)
        return TRUE;  /* no whitelist = allow all */

    if (!processPath || !processPath[0])
        return FALSE; /* unknown process when whitelist is active → skip */

    /* Extract filename from full path */
    const wchar_t *base = wcsrchr(processPath, L'\\');
    base = base ? base + 1 : processPath;

    for (int i = 0; i < g_whitelistCount; i++)
    {
        /* Exact match (e.g. "weixin.exe" == "weixin.exe") */
        if (_wcsicmp(base, g_whitelist[i]) == 0)
            return TRUE;
        /* Match when user omitted .exe: append it and compare */
        wchar_t withExt[MAX_PATH];
        wcscpy_s(withExt, MAX_PATH, g_whitelist[i]);
        wcscat_s(withExt, MAX_PATH, L".exe");
        if (_wcsicmp(base, withExt) == 0)
            return TRUE;
    }
    return FALSE;
}


/**
 * Finds windows that have a non-zero WindowDisplayAffinity.
 * Such windows have enabled display protection / anti-capture.
 */
static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    DWORD affinity = 0;

    if (g_pGetWindowDisplayAffinity &&
        g_pGetWindowDisplayAffinity(hwnd, &affinity) &&
        affinity != 0)
    {
        if (g_windowCount < 8192)
        {
            g_hWnd[g_windowCount] = hwnd;
            g_windowCount++;
        }
    }

    return TRUE;  /* continue enumeration */
}

/* ========================================================================
 * Privilege Escalation
 * ======================================================================== */

/**
 * Enables SeDebugPrivilege for the current process token so that
 * OpenProcess(PROCESS_ALL_ACCESS) succeeds on other processes.
 */
static BOOL EnableDebugPrivileges(void)
{
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &hToken))
        return FALSE;

    if (!LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &luid))
    {
        CloseHandle(hToken);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(hToken);

    return (err == ERROR_SUCCESS);
}

/* ========================================================================
 * Remote Thread Creation
 * ======================================================================== */

/**
 * Creates a remote thread in the target process.
 * Uses NtCreateThreadEx on Vista+ (more reliable than CreateRemoteThread),
 * falls back to CreateRemoteThread on older systems.
 */
static HANDLE CreateRemoteThreadNt(HANDLE hProcess,
                                    LPTHREAD_START_ROUTINE lpStartAddress,
                                    LPVOID lpParameter)
{
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll)
    {
        fnNtCreateThreadEx pNtCreateThreadEx =
            (fnNtCreateThreadEx)GetProcAddress(hNtDll, "NtCreateThreadEx");

        if (pNtCreateThreadEx)
        {
            HANDLE hThread = NULL;
            NTSTATUS status = pNtCreateThreadEx(
                &hThread,
                0x1FFFFF,       /* THREAD_ALL_ACCESS */
                NULL,
                hProcess,
                lpStartAddress,
                lpParameter,
                FALSE,          /* not suspended */
                0,
                0,
                0,
                NULL
            );

            if (NT_SUCCESS(status) && hThread)
            {
                WaitForSingleObject(hThread, INFINITE);
                return hThread;
            }
        }
    }

    /* Fallback to CreateRemoteThread */
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
                                         lpStartAddress, lpParameter,
                                         0, NULL);
    if (hThread)
        WaitForSingleObject(hThread, INFINITE);

    return hThread;
}

/* ========================================================================
 * 32-bit DLL Injection
 * ======================================================================== */

/**
 * Injects a 32-bit DLL into a 32-bit target process.
 *
 * Writes a parameter block (function pointers + DLL path) and a small
 * shellcode stub to the target, then creates a remote thread that calls
 * RtlInitUnicodeString + LdrLoadDll to load the DLL.
 *
 * Returns 1 on success, or a positive error code on failure.
 */
static int InjectDll32(DWORD dwProcessId, const wchar_t *dllPath)
{
    HANDLE hProcess = NULL;
    LPVOID pRemoteBuffer = NULL;
    LPVOID pRemoteCode = NULL;
    HANDLE hThread = NULL;

    /* Open target process with full access */
    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
    if (!hProcess)
    {
        wprintf(L"[Inject32] Cannot open process %lu (err %lu)\n",
                dwProcessId, GetLastError());
        return 1;
    }

    /* Get ntdll function addresses – same in all 32-bit processes */
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll)
    {
        CloseHandle(hProcess);
        return 2;
    }

    fnRtlInitUnicodeString pRtlInitUnicodeString =
        (fnRtlInitUnicodeString)GetProcAddress(hNtdll, "RtlInitUnicodeString");
    fnLdrLoadDll pLdrLoadDll =
        (fnLdrLoadDll)GetProcAddress(hNtdll, "LdrLoadDll");

    if (!pRtlInitUnicodeString || !pLdrLoadDll)
    {
        CloseHandle(hProcess);
        return 3;
    }

    /* Allocate memory for parameter block in target */
    pRemoteBuffer = VirtualAllocEx(hProcess, NULL, 0x1000,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!pRemoteBuffer)
    {
        CloseHandle(hProcess);
        return 4;
    }

    /* Build parameter block:
     *   +0x00  pRtlInitUnicodeString  (4 bytes)
     *   +0x04  pLdrLoadDll            (4 bytes)
     *   +0x08  dllPath                (variable, wide string)
     */
    BYTE paramBlock[0x1000];
    ZeroMemory(paramBlock, sizeof(paramBlock));

    DWORD offset = 0;

    *(FARPROC *)(paramBlock + offset) = (FARPROC)pRtlInitUnicodeString;
    offset += sizeof(FARPROC);   /* 4 bytes on x86 */

    *(FARPROC *)(paramBlock + offset) = (FARPROC)pLdrLoadDll;
    offset += sizeof(FARPROC);   /* 4 bytes on x86 */

    DWORD dllPathBytes = (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t));
    memcpy(paramBlock + offset, dllPath, dllPathBytes);
    offset += dllPathBytes;

    /* Write parameter block to target */
    if (!WriteProcessMemory(hProcess, pRemoteBuffer,
                             paramBlock, offset, NULL))
    {
        VirtualFreeEx(hProcess, pRemoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 5;
    }

    /* Allocate memory for shellcode in target */
    pRemoteCode = VirtualAllocEx(hProcess, NULL, 0x1000,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!pRemoteCode)
    {
        VirtualFreeEx(hProcess, pRemoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 6;
    }

    /* Write shellcode to target */
    if (!WriteProcessMemory(hProcess, pRemoteCode,
                             g_shellcode32, sizeof(g_shellcode32), NULL))
    {
        VirtualFreeEx(hProcess, pRemoteCode, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, pRemoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 7;
    }

    /* Execute remote thread */
    hThread = CreateRemoteThreadNt(hProcess,
                                    (LPTHREAD_START_ROUTINE)pRemoteCode,
                                    pRemoteBuffer);
    int result;
    if (hThread)
    {
        result = 1;  /* success */
        CloseHandle(hThread);
    }
    else
    {
        wprintf(L"[Inject32] CreateRemoteThread failed (err %lu)\n", GetLastError());
        result = 8;
    }

    /* Cleanup remote memory */
    VirtualFreeEx(hProcess, pRemoteCode, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, pRemoteBuffer, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return result;
}

/* ========================================================================
 * 64-bit DLL Injection
 * ======================================================================== */

/**
 * Injects a 64-bit DLL into a 64-bit target process using wow64ext.dll.
 *
 * Uses VirtualAllocEx64 / WriteProcessMemory64 to place parameter block
 * and x64 shellcode in the target, then calls RtlCreateUserThread via
 * X64Call to start the shellcode.
 *
 * Returns 1 on success, or a positive error code on failure.
 */
static int InjectDll64(DWORD dwProcessId, const wchar_t *dllPath)
{
    if (!g_hWow64Ext)
    {
        wprintf(L"[Inject64] wow64ext.dll not loaded\n");
        return 20;
    }

    /* Resolve wow64ext functions */
    // fnVirtualAllocEx64       pVirtualAllocEx64       = (fnVirtualAllocEx64)      GetProcAddress(g_hWow64Ext, "VirtualAllocEx64");
    // fnWriteProcessMemory64   pWriteProcessMemory64   = (fnWriteProcessMemory64)  GetProcAddress(g_hWow64Ext, "WriteProcessMemory64");
    // fnGetProcAddress64       pGetProcAddress64        = (fnGetProcAddress64)      GetProcAddress(g_hWow64Ext, "GetProcAddress64");
    // fnGetModuleHandle64      pGetModuleHandle64       = (fnGetModuleHandle64)     GetProcAddress(g_hWow64Ext, "GetModuleHandle64");
    // fnX64Call                pX64Call                 = (fnX64Call)               GetProcAddress(g_hWow64Ext, "X64Call");
    // fnVirtualFreeEx64        pVirtualFreeEx64         = (fnVirtualFreeEx64)       GetProcAddress(g_hWow64Ext, "VirtualFreeEx64");

    if (!pVirtualAllocEx64 || !pWriteProcessMemory64 ||
        !pGetProcAddress64 || !pGetModuleHandle64 || !pX64Call)
    {
        wprintf(L"[Inject64] Failed to resolve wow64ext functions\n");
        return 21;
    }

    /* Open target process */
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
    if (!hProcess)
    {
        wprintf(L"[Inject64] Cannot open process %lu (err %lu)\n",
                dwProcessId, GetLastError());
        return 22;
    }

    /* Get ntdll addresses in 64-bit context (same base in all 64-bit procs) */
    DWORD64 ntdll64 = pGetModuleHandle64("ntdll.dll");
    if (!ntdll64)
    {
        CloseHandle(hProcess);
        return 23;
    }

    DWORD64 RtlInitUnicodeString64 = pGetProcAddress64(ntdll64, "RtlInitUnicodeString");
    DWORD64 LdrLoadDll64           = pGetProcAddress64(ntdll64, "LdrLoadDll");
    DWORD64 RtlCreateUserThread64  = pGetProcAddress64(ntdll64, "RtlCreateUserThread");

    if (!RtlInitUnicodeString64 || !LdrLoadDll64 || !RtlCreateUserThread64)
    {
        wprintf(L"[Inject64] Failed to resolve ntdll64 functions\n");
        CloseHandle(hProcess);
        return 24;
    }

    /* Allocate memory for parameter block in target (64-bit address) */
    DWORD64 remoteParam = pVirtualAllocEx64(hProcess, 0, 0x1000,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    if (!remoteParam)
    {
        CloseHandle(hProcess);
        return 25;
    }

    /* Build x64 parameter block:
     *   +0x00  RtlInitUnicodeString  (8 bytes)
     *   +0x08  LdrLoadDll            (8 bytes)
     *   +0x10  dllPath               (variable, wide string)
     */
    BYTE paramBlock64[0x1000];
    ZeroMemory(paramBlock64, sizeof(paramBlock64));

    DWORD off = 0;
    *(DWORD64 *)(paramBlock64 + off) = RtlInitUnicodeString64;
    off += 8;
    *(DWORD64 *)(paramBlock64 + off) = LdrLoadDll64;
    off += 8;
    DWORD dllPathBytes = (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t));
    memcpy(paramBlock64 + off, dllPath, dllPathBytes);
    off += dllPathBytes;

    /* Write parameter block to target */
    if (!pWriteProcessMemory64(hProcess, remoteParam,
                                paramBlock64, off, NULL))
    {
        if (pVirtualFreeEx64)
            pVirtualFreeEx64(hProcess, remoteParam, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 26;
    }

    /* Allocate memory for x64 shellcode in target */
    DWORD64 remoteCode = pVirtualAllocEx64(hProcess, 0, 0x1000,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!remoteCode)
    {
        if (pVirtualFreeEx64)
            pVirtualFreeEx64(hProcess, remoteParam, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 27;
    }

    /* Write x64 shellcode to target */
    if (!pWriteProcessMemory64(hProcess, remoteCode,
                                g_shellcode64, sizeof(g_shellcode64), NULL))
    {
        if (pVirtualFreeEx64)
        {
            pVirtualFreeEx64(hProcess, remoteCode, 0, MEM_RELEASE);
            pVirtualFreeEx64(hProcess, remoteParam, 0, MEM_RELEASE);
        }
        CloseHandle(hProcess);
        return 28;
    }

    /* Create remote thread in the 64-bit target via RtlCreateUserThread.
     *
     * RtlCreateUserThread(
     *   ProcessHandle,      rcx
     *   SecurityDescriptor, rdx
     *   CreateSuspended,    r8
     *   StackZeroBits,      r9
     *   MaximumStackSize,   [rsp+28h]
     *   InitialStackSize,   [rsp+30h]
     *   StartAddress,       [rsp+38h]
     *   Argument,           [rsp+40h]
     *   ThreadHandle,       [rsp+48h]
     *   ClientId            [rsp+50h]
     * )
     */
    HANDLE hThread = NULL;
    DWORD64 callResult = pX64Call(
        RtlCreateUserThread64, 10,
        (DWORD64)hProcess,     /* ProcessHandle   */
        (DWORD64)0,            /* SecurityDesc    */
        (DWORD64)FALSE,        /* CreateSuspended */
        (DWORD64)0,            /* StackZeroBits   */
        (DWORD64)0,            /* MaxStackSize    */
        (DWORD64)0,            /* InitialStackSize*/
        remoteCode,            /* StartAddress    */
        remoteParam,           /* Argument        */
        (DWORD64)&hThread,     /* ThreadHandle    */
        (DWORD64)0             /* ClientId        */
    );

    int result;
    if (NT_SUCCESS((NTSTATUS)callResult) && hThread)
    {
        WaitForSingleObject(hThread, 10000);
        CloseHandle(hThread);
        result = 1;  /* success */
    }
    else
    {
        wprintf(L"[Inject64] RtlCreateUserThread failed (status 0x%08lX)\n",
                (DWORD)callResult);
        result = 29;
    }

    /* Cleanup remote memory */
    if (pVirtualFreeEx64)
    {
        pVirtualFreeEx64(hProcess, remoteCode, 0, MEM_RELEASE);
        pVirtualFreeEx64(hProcess, remoteParam, 0, MEM_RELEASE);
    }
    CloseHandle(hProcess);

    return result;
}

/* ========================================================================
 * Worker Thread
 * ======================================================================== */

/**
 * Periodically scans for windows with display affinity set and injects
 * the appropriate DLL into their owner processes.
 *
 * Scan interval: 10 seconds (0x2710 ms).
 */
static DWORD WINAPI WorkerThread(LPVOID lpParam)
{
    (void)lpParam;

    /* Load wow64ext.dll for 64-bit injection support */
    g_hWow64Ext = LoadLibraryW(L"wow64ext.dll");
    if (!g_hWow64Ext && g_is64BitSystem)
    {
        wprintf(L"[Worker] Warning: wow64ext.dll not found, "
                L"64-bit injection unavailable\n");
    }
    else if (g_hWow64Ext)
    {
        wprintf(L"[Worker] wow64ext.dll loaded\n");
    }

    /* Get the EXE's own directory to construct DLL paths */
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t *lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash)
        lastSlash[1] = L'\0';
    else
        wcscpy_s(exeDir, MAX_PATH, L".\\");

    /* Pre-build DLL paths */
    wchar_t dllPath32[MAX_PATH];
    wchar_t dllPath64[MAX_PATH];
    wcscpy_s(dllPath32, MAX_PATH, exeDir);
    wcscat_s(dllPath32, MAX_PATH, L"InjectHookDll32.dll");
    wcscpy_s(dllPath64, MAX_PATH, exeDir);
    wcscat_s(dllPath64, MAX_PATH, L"InjectHookDll64.dll");

    wprintf(L"[Worker] DLL paths:\n");
    wprintf(L"  32-bit: %s\n", dllPath32);
    wprintf(L"  64-bit: %s\n", dllPath64);

    /* Main monitoring loop */
    while (!g_exitFlag)
    {
        /* Clear window list */
        g_windowCount = 0;

        /* Find windows with display affinity */
        EnumWindows(EnumWindowsCallback, 0);

        if (g_windowCount > 0)
        {
            wprintf(L"[Worker] Found %lu protected window(s)\n",
                    g_windowCount);
        }

        /* Process each protected window */
        for (DWORD i = 0; i < g_windowCount; i++)
        {
            if (g_exitFlag)
                break;

            DWORD processId = 0;
            GetWindowThreadProcessId(g_hWnd[i], &processId);

            if (processId == 0 || processId == GetCurrentProcessId())
                continue;

            wchar_t processPath[MAX_PATH] = {0};

            /* Step 1: determine 32/64-bit using PROCESS_QUERY_INFORMATION only.
             * Failure here is non-fatal – default to 32-bit path. */
            BOOL isWow64 = FALSE;
            if (g_is64BitSystem)
            {
                HANDLE hQuery = OpenProcess(PROCESS_QUERY_INFORMATION,
                                            FALSE, processId);
                if (hQuery)
                {
                    IsWow64Process(hQuery, &isWow64);
                    CloseHandle(hQuery);
                }
                /* else: cannot determine bitness, assume 32-bit (same as fby) */
            }

            BOOL is64Target = (g_is64BitSystem && !isWow64);
            wprintf(L"[Worker] PID %lu (%s)\n",
                    processId, is64Target ? L"64-bit" : L"32-bit");

            /* Step 2: try to read process path for diagnostics (best-effort,
             * fby uses OpenProcess(0x410) here and skips path if it fails). */
            /* Falls back to QueryFullProcessImageNameW (needs only 0x1000). */
            {
                HANDLE hInfo = OpenProcess(
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                    FALSE, processId);
                if (hInfo)
                {
                    GetModuleFileNameExW(hInfo, NULL, processPath, MAX_PATH);
                    CloseHandle(hInfo);
                }
            }
            if (!processPath[0])
            {
                /* Fallback: QueryFullProcessImageNameW only needs 0x1000 */
                HANDLE hLim = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                          FALSE, processId);
                if (hLim)
                {
                    DWORD sz = MAX_PATH;
                    QueryFullProcessImageNameW(hLim, 0, processPath, &sz);
                    CloseHandle(hLim);
                }
            }
            if (processPath[0])
                wprintf(L"[Worker]   -> %s\n", processPath);

            /* Whitelist check: skip injection if this process is not listed */
            if (!IsWhitelisted(processPath))
            {
                wprintf(L"[Worker] PID %lu not in whitelist, skipping\n",
                        processId);
                continue;
            }

            /* Inject the matching DLL */
            int injectResult;
            if (is64Target)
            {
                injectResult = InjectDll64(processId, dllPath64);
            }
            else
            {
                injectResult = InjectDll32(processId, dllPath32);
            }

            if (injectResult == 1)
                wprintf(L"[Worker] Injection succeeded for PID %lu\n", processId);
            else
                wprintf(L"[Worker] Injection failed for PID %lu (code %d)\n",
                        processId, injectResult);
        }

        /* Sleep 10 seconds (0x2710 ms) before next scan */
        Sleep(10000);
    }

    /* Cleanup */
    if (g_hWow64Ext)
    {
        FreeLibrary(g_hWow64Ext);
        g_hWow64Ext = NULL;
    }

    wprintf(L"[Worker] Thread exiting\n");
    return 0;
}

/* ========================================================================
 * Hidden Window Procedure
 * ======================================================================== */

/**
 * Window procedure for the hidden test window.
 * On WM_CREATE, tests whether Set/GetWindowDisplayAffinity work.
 */
static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        DWORD affinity = 0;

        if (g_pGetWindowDisplayAffinity &&
            g_pGetWindowDisplayAffinity(hWnd, &affinity))
        {
            if (affinity != 0)
            {
                g_initResult = -1;
                PostQuitMessage(0);
                return 0;
            }

            if (g_pSetWindowDisplayAffinity(hWnd, WDA_EXCLUDEFROMCAPTURE))
            {
                affinity = 0;
                g_pGetWindowDisplayAffinity(hWnd, &affinity);
                g_initResult = -3;
                if (affinity == WDA_EXCLUDEFROMCAPTURE)
                    g_initResult = 0;
            }
            else
            {
                g_initResult = -2;
            }
        }
        else
        {
            g_initResult = -1;
        }

        PostQuitMessage(0);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}

/* ========================================================================
 * Main Entry Point
 * ======================================================================== */

int main(void)
{
    /* ---- 1. Initialize locale ---- */
    setlocale(LC_ALL, "");

    /* ---- 1b. Parse whitelist from command line arguments ---- */
    /* Usage: ScreenDominator.exe [process1.exe] [process2.exe] ...          */
    /* If no arguments are given, all processes are targeted (default).      */
    {
        int nArgs = 0;
        LPWSTR *argList = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (argList && nArgs >= 2 && argList[1] && argList[1][0] != L'\0')
        {
            wcsncpy_s(g_whitelistApp, MAX_PATH, argList[1], _TRUNCATE);
            wprintf(L"[Main] Whitelist app: %s\n", g_whitelistApp);
        }
        if (argList)
        {
            for (int i = 1; i < nArgs && g_whitelistCount < 64; i++)
            {
                wcscpy_s(g_whitelist[g_whitelistCount], MAX_PATH, argList[i]);
                g_whitelistCount++;
            }
            LocalFree(argList);
        }
    }
    if (g_whitelistCount > 0)
    {
        wprintf(L"Whitelist (%d entries):\n", g_whitelistCount);
        for (int i = 0; i < g_whitelistCount; i++)
            wprintf(L"  [%d] %s\n", i + 1, g_whitelist[i]);
    }
    else
    {
        wprintf(L"No whitelist — targeting all protected processes.\n");
    }

    /* ---- 2. Get Windows version via RtlGetNtVersionNumbers ---- */
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll)
    {
        fnRtlGetNtVersionNumbers pRtlGetNtVersionNumbers =
            (fnRtlGetNtVersionNumbers)GetProcAddress(
                hNtDll, "RtlGetNtVersionNumbers");
        if (pRtlGetNtVersionNumbers)
        {
            ULONG major = 0, minor = 0, build = 0;
            pRtlGetNtVersionNumbers(&major, &minor, &build);
            wprintf(L"Windows %lu.%lu build %lu\n",
                    major, minor, build & 0xFFFF);
        }
    }

    /* ---- 3. Set console title ---- */
    SetConsoleTitleW(L"ScreenDominator");

    /* ---- 4. Resolve WindowDisplayAffinity APIs ---- */
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32)
        hUser32 = LoadLibraryW(L"user32.dll");

    if (hUser32)
    {
        g_pGetWindowDisplayAffinity = (BOOL(WINAPI *)(HWND, DWORD *))
            GetProcAddress(hUser32, "GetWindowDisplayAffinity");
        g_pSetWindowDisplayAffinity = (BOOL(WINAPI *)(HWND, DWORD))
            GetProcAddress(hUser32, "SetWindowDisplayAffinity");
    }

    if (!g_pGetWindowDisplayAffinity || !g_pSetWindowDisplayAffinity)
    {
        wprintf(L"Error: WindowDisplayAffinity APIs not available.\n");
        wprintf(L"This program requires Windows 7 or later.\n");
        getchar();
        return 1;
    }

    /* ---- 5. Register hidden window class ---- */
    WNDCLASSEXW wc = { 0 };
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance      = GetModuleHandleW(NULL);
    wc.lpszClassName  = L"ShareBit ScreenDominator class";
    wc.hIcon          = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_APPLICATION));
    wc.hCursor        = LoadCursorW(NULL, MAKEINTRESOURCEW(IDC_ARROW));
    wc.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc))
    {
        wprintf(L"Error: Failed to register window class.\n");
        return 2;
    }

    /* ---- 6. Create hidden test window ---- */
    HWND hWnd = CreateWindowExW(
        0,
        L"ShareBit ScreenDominator class",
        L"ShareBit ScreenDominator window",
        0xCF0000u,
        0, 0, 0, 0,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    if (!hWnd)
    {
        wprintf(L"Error: Failed to create test window (GetLastError=%lu).\n",
                GetLastError());
        UnregisterClassW(L"ShareBit ScreenDominator class", GetModuleHandleW(NULL));
        return 3;
    }

    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    /* ---- 7. Message loop (exits when window is destroyed) ---- */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hWnd);
    UnregisterClassW(L"ShareBit ScreenDominator class", GetModuleHandleW(NULL));

    /* ---- 8. Check display affinity test result ---- */
    if (g_initResult == -1)
    {
        wprintf(L"Error: GetWindowDisplayAffinity failed after "
                L"SetWindowDisplayAffinity succeeded.\n");
        wprintf(L"This program requires Windows 10 version 1703 or later.\n");
        getchar();
        return 4;
    }
    else if (g_initResult == -2 || g_initResult == -3)
    {
        wprintf(L"Error: WindowDisplayAffinity APIs not working properly.\n");
        wprintf(L"This program requires Windows 10 version 1703 or later.\n");
        getchar();
        return 5;
    }

    wprintf(L"Display affinity APIs available. Initialization OK.\n");

    /* ---- 10. Determine system architecture ---- */
    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);
    g_is64BitSystem =
        (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
         sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64);

    wprintf(L"System architecture: %s\n",
            g_is64BitSystem ? L"64-bit" : L"32-bit");

    /* ---- 11. Enable debug privileges ---- */
    if (EnableDebugPrivileges())
        wprintf(L"Debug privileges enabled.\n");
    else
        wprintf(L"Warning: Failed to enable debug privileges.\n");

    /* ---- 12. Start worker thread ---- */
    HANDLE hThread = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    if (!hThread)
    {
        wprintf(L"Error: Failed to create worker thread.\n");
        return 6;
    }

    wprintf(L"Monitoring started. Scanning every 10 seconds.\n");
    wprintf(L"Press Enter to exit...\n");
    getchar();

    /* ---- 13. Signal exit and wait for worker ---- */
    g_exitFlag = 1;
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);

    wprintf(L"Exited cleanly.\n");
    return 0;
}