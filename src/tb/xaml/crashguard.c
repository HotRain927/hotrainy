/* hr/tb/xaml/crashguard.c - in-process crash reporting for GlassBoot.dll
 *
 * Why this exists
 * ---------------
 * explorer.exe runs at a higher integrity level than this session, so cdb
 * cannot attach to it (Win32 error 5, access denied), and system-level WER
 * dump configuration needs elevation we do not have.
 *
 * But the crash happens INSIDE explorer while our DLL is loaded, and we can
 * install an unhandled exception filter from there. That gives us the fault
 * address, the owning module, and a stack trace without any debugger.
 *
 * Module resolution walks the PEB loader list directly instead of calling
 * EnumProcessModules: this runs on a crashing thread, so it must not rely on
 * APIs that allocate, take locks, or call back into the loader.
 *
 * This is pure C and links nothing beyond ntdll/kernel32, so it can live in
 * the bootstrap layer without disturbing the COM/WinRT preconditions that
 * InitializeXamlDiagnosticsEx depends on.
 */
#include <windows.h>
#include <winternl.h>

#include "hr/tb/xaml/crashguard.h"

/* Not in winternl.h; we only need the fields we actually read. */
typedef struct _PEB_LDR_DATA_LOCAL {
    ULONG Length;
    BOOLEAN Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
} PEB_LDR_DATA_LOCAL;

typedef struct _LDR_DATA_TABLE_ENTRY_LOCAL {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY_LOCAL;

static void CrashLog(const char* msg) {
    wchar_t dir[MAX_PATH] = {0};
    wchar_t base[MAX_PATH] = {0};
    wchar_t path[MAX_PATH] = {0};
    HANDLE h;
    DWORD wr = 0;

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    wsprintfW(base, L"%ls\\hotrainy", dir);
    CreateDirectoryW(base, NULL);
    wsprintfW(path, L"%ls\\crash.log", base);

    h = CreateFileW(path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, msg, (DWORD)lstrlenA(msg), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
    CloseHandle(h);
}

/* Resolve an address to "module + offset" by walking the loader list. */
static void DescribeAddress(void* addr, char* out, DWORD outLen, int wantFull) {
    PPEB peb;
    PEB_LDR_DATA_LOCAL* ldr;
    LIST_ENTRY* head;
    LIST_ENTRY* cur;
    int guard = 0;

    (void)outLen;
    out[0] = 0;

#if defined(_M_X64) || defined(_M_AMD64)
    peb = (PPEB)__readgsqword(0x60);
#else
    peb = (PPEB)__readfsdword(0x30);
#endif
    if (!peb || !peb->Ldr) {
        wsprintfA(out, "%p  (no PEB)", addr);
        return;
    }

    ldr = (PEB_LDR_DATA_LOCAL*)peb->Ldr;
    head = &ldr->InLoadOrderModuleList;
    cur = head->Flink;

    while (cur && cur != head && guard++ < 512) {
        LDR_DATA_TABLE_ENTRY_LOCAL* e = (LDR_DATA_TABLE_ENTRY_LOCAL*)cur;
        BYTE* base = (BYTE*)e->DllBase;
        BYTE* end = base + e->SizeOfImage;

        if (base && (BYTE*)addr >= base && (BYTE*)addr < end) {
            char narrow[MAX_PATH * 2] = {0};
            if (e->FullDllName.Buffer && e->FullDllName.Length > 0) {
                int chars = e->FullDllName.Length / sizeof(wchar_t);
                if (chars > MAX_PATH * 2 - 1) chars = MAX_PATH * 2 - 1;
                WideCharToMultiByte(CP_ACP, 0, e->FullDllName.Buffer, chars,
                                    narrow, sizeof(narrow) - 1, NULL, NULL);
            }
            if (wantFull) {
                /* short name only for stack frames */
                char* slash = narrow;
                char* p = narrow;
                while (*p) { if (*p == '\\') slash = p + 1; ++p; }
                wsprintfA(out, "%p  %s + 0x%IX", addr, slash,
                          (ULONG_PTR)((BYTE*)addr - base));
            } else {
                wsprintfA(out, "%p  %s + 0x%IX", addr, narrow,
                          (ULONG_PTR)((BYTE*)addr - base));
            }
            return;
        }
        cur = cur->Flink;
    }

    wsprintfA(out, "%p  (unmapped or freed)", addr);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    char desc[1024];
    char line[1200];
    void* frames[48];
    USHORT n;
    USHORT i;

    if (!ep) return EXCEPTION_CONTINUE_SEARCH;

    CrashLog("");
    CrashLog("========== UNHANDLED EXCEPTION IN EXPLORER ==========");

    wsprintfA(line, "pid=%lu tid=%lu",
              (unsigned long)GetCurrentProcessId(),
              (unsigned long)GetCurrentThreadId());
    CrashLog(line);

    wsprintfA(line, "code    = 0x%08X", (unsigned)ep->ExceptionRecord->ExceptionCode);
    CrashLog(line);

    DescribeAddress(ep->ExceptionRecord->ExceptionAddress, desc, sizeof(desc), 0);
    wsprintfA(line, "address = %s", desc);
    CrashLog(line);

    /* Parameter 0/1 often identify the offending pointer for AVs. */
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        wsprintfA(line, "param0  = 0x%IX  param1 = 0x%IX",
                  (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[0],
                  (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1]);
        CrashLog(line);
    }

    CrashLog("--- stack ---");
    n = RtlCaptureStackBackTrace(0, 48, frames, NULL);
    for (i = 0; i < n; ++i) {
        DescribeAddress(frames[i], desc, sizeof(desc), 1);
        wsprintfA(line, "  [%02u] %s", (unsigned)i, desc);
        CrashLog(line);
    }

    CrashLog("====================================================");
    CrashLog("");

    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashGuard_Install(void) {
    CrashLog("crashguard: installed");
    SetUnhandledExceptionFilter(CrashFilter);
}