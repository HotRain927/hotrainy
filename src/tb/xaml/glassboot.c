/* hr/tb/xaml/glassboot.c - pure C bootstrap layer for GlassBoot.dll
 *
 * ===================================================================
 * WHY THIS LAYER EXISTS AND WHY IT IS PLAIN C
 *
 * InitializeXamlDiagnosticsEx has a hard precondition, established by
 * controlled experiments on this machine:
 *
 *   It must be called from a thread in a process whose COM/WinRT state
 *   is still pristine. Any prior CoInitializeEx or WinRT init - on that
 *   thread or anywhere in the process - makes it return
 *   0x80070490 (E_NOTFOUND).
 *
 *   Experiment log (same DLL, same params, only the thread state differs):
 *     pristine thread, nothing initialized  -> 0x00000000  SUCCESS
 *     thread did CoInitializeEx(STA) first  -> 0x80070490  FAILED
 *     process MTA, then called from new thread -> 0x80070490  FAILED
 *
 * C++/WinRT runs static initializers when the module loads, and those
 * touch COM/WinRT. A DLL linked against C++/WinRT therefore can never
 * satisfy the precondition. That is why this file is compiled as C and
 * links no C++ runtime at all.
 *
 * The real work lives in GlassTAP.dll (C++/WinRT), which is loaded only
 * AFTER the diagnostics channel is up.
 * ===================================================================
 *
 * Implementation note: this file deliberately mirrors a minimal
 * known-good probe that returned 0x00000000 in the same environment.
 * Every extra operation in DllMain is a risk, so it does as little as
 * possible and writes one log line at the very end.
 */
#include <windows.h>
#include <objbase.h>
#include <process.h>

#include "hr/tb/xaml/crashguard.h"

/* CLSID_GlassTAPSite {7A3F9C21-5B4D-4E8A-9F12-3C6D8E5A1B47}
 * Must match the value compiled into GlassTAP.dll. */
static const CLSID kTapSiteClsid = {
    0x7a3f9c21, 0x5b4d, 0x4e8a,
    { 0x9f, 0x12, 0x3c, 0x6d, 0x8e, 0x5a, 0x1b, 0x47 }
};

static const wchar_t* kReadyEvent = L"GlassTAP_Ready_v1";
static const wchar_t* kFeatureDll = L"GlassTAP.dll";

typedef HRESULT(WINAPI* PFN_InitXamlDiagEx)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
typedef HRESULT(STDAPICALLTYPE* PFN_DllGetClassObject)(REFCLSID, REFIID, void**);

static HMODULE g_feature = NULL;
static PFN_DllGetClassObject g_featureGetClassObject = NULL;
static volatile LONG g_featureTried = 0;

/* Result of the bootstrap call, written once at the end. */
static HRESULT g_bootResult = E_PENDING;

/* ------------------------------------------------------------------ */
/* Logging: opened only when we have a result to report.              */
/* Keeping it out of the hot path matters - see the note above.       */
/* ------------------------------------------------------------------ */

static void WriteResultLog(void) {
    wchar_t dir[MAX_PATH] = {0};
    wchar_t base[MAX_PATH] = {0};
    wchar_t path[MAX_PATH] = {0};
    char buf[256];
    HANDLE h;
    DWORD wr = 0;

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;

    wsprintfW(base, L"%ls\\hotrainy", dir);
    CreateDirectoryW(base, NULL);
    wsprintfW(path, L"%ls\\boot.log", base);

    /*
     * Log the writing process.
     *
     * This DLL is loaded twice in a normal run:
     *   1. hotrainy.exe loads it explicitly (to get the hook proc).
     *      DllMain there stops at "not explorer".
     *   2. explorer.exe receives it through SetWindowsHookEx - that is
     *      the run that actually bootstraps the diagnostics.
     *
     * Without the process name the two are indistinguishable in the log,
     * which made a previous failure look like "the boot layer ran and
     * gave up" when in fact only our own loader had run.
     */
    {
        wchar_t exe[MAX_PATH] = {0};
        wchar_t* nm = exe;
        wchar_t* q;
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        for (q = exe; *q; ++q) { if (*q == L'\\') nm = q + 1; }

        wsprintfA(buf, "boot: pid=%lu proc=%ls status=0x%08X %s",
                  (unsigned long)GetCurrentProcessId(), nm,
                  (unsigned)g_bootResult,
                  g_bootResult == 0 ? "" : "(marker or hresult)");
    }

    h = CreateFileW(path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, (DWORD)lstrlenA(buf), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* Feature layer loading - only valid AFTER diagnostics are up.       */
/* ------------------------------------------------------------------ */

static PFN_DllGetClassObject LoadFeature(void) {
    wchar_t self[MAX_PATH] = {0};
    wchar_t path[MAX_PATH] = {0};
    wchar_t* slash;
    wchar_t* p;
    HMODULE mod = NULL;

    if (InterlockedCompareExchange(&g_featureTried, 1, 0) != 0) {
        return g_featureGetClassObject;
    }

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&LoadFeature, &mod);
    GetModuleFileNameW(mod, self, MAX_PATH);

    slash = self;
    for (p = self; *p; ++p) { if (*p == L'\\') slash = p; }
    *slash = 0;

    wsprintfW(path, L"%ls\\%ls", self, kFeatureDll);

    g_feature = LoadLibraryExW(path, NULL,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_feature) return NULL;

    g_featureGetClassObject =
        (PFN_DllGetClassObject)(void*)GetProcAddress(g_feature, "DllGetClassObject");
    return g_featureGetClassObject;
}

/*
 * Deferred bootstrap.
 *
 * ===================================================================
 * WHY NOT FROM DllMain
 *
 * Calling InitializeXamlDiagnosticsEx from DllMain returned S_OK once,
 * but explorer then crashed with 0xc0000409 (__fastfail) - the loader
 * lock is held there and the call is not safe under it.
 *
 * The hook thread is the right place: by the time WH_CALLWNDPROC fires,
 * DllMain has returned and the loader lock is released, while this
 * thread still has touched no COM/WinRT state.
 *
 * So: DllMain records what it needs, and the first hook callback does
 * the actual work exactly once.
 * ===================================================================
 */
static PFN_InitXamlDiagEx g_fn = NULL;
static wchar_t g_selfPath[MAX_PATH] = {0};
static volatile LONG g_bootDone = 0;

/* ------------------------------------------------------------------ */
/* Bootstrap worker thread.                                            */
/*                                                                     */
/* Nothing COM/WinRT/C++ may happen on this thread. That is the point. */
/*                                                                     */
/* This structure is deliberately identical to the probe that was      */
/* measured working on this machine (DWORD WINAPI, plain CreateThread, */
/* call, log). Keep it that way.                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI BootThread(LPVOID param) {
    PFN_InitXamlDiagEx fn = (PFN_InitXamlDiagEx)param;
    HANDLE ev;
    HRESULT hr;

    if (InterlockedCompareExchange(&g_bootDone, 1, 0) != 0) return 0;
    if (!fn) return 0;

    /* The three arguments below are the minimal working set found by
     * parameter enumeration on this machine:
     *   - endpoint name fixed at "VisualDiagConnection1" (first call wins)
     *   - wszDllXamlDiagnostics = NULL
     *   - CLSID that the feature layer's DllGetClassObject recognises
     * No file I/O happens before this call, to keep the thread state clean. */
    hr = fn(L"VisualDiagConnection1", GetCurrentProcessId(),
            NULL, g_selfPath, kTapSiteClsid, NULL);
    g_bootResult = hr;
    WriteResultLog();

    /*
     * Signal the injector.
     *
     * ================================================================
     * WHY THE HANDLE IS DELIBERATELY NOT CLOSED
     *
     * A named event object lives only as long as some handle refers to
     * it. Creating one, setting it, and immediately closing the handle
     * destroys the object - a later OpenEvent from the injector finds
     * nothing and the injector waits out its full timeout.
     *
     * That is exactly what happened before: the bootstrap succeeded
     * (boot.log ends with 0x00000000) and the taskbar was already
     * transparent, yet the injector reported a timeout because the
     * event had already been torn down.
     *
     * Leaking this single handle is the correct behaviour: it keeps the
     * event alive so the injector can observe it. The process exits
     * shortly after in the failure path, and in the success path the
     * module stays mapped for the lifetime of the feature.
     * ================================================================
     */
    ev = CreateEventW(NULL, TRUE, FALSE, kReadyEvent);
    if (ev) SetEvent(ev);

    return 0;
}

/* Kept for symmetry: the hook callback path is unused now that the
 * worker thread carries the call, but the symbol documents the contract. */
void GlassBoot_OnHookCall(void) { /* no-op */ }

/*
 * ===================================================================
 * EXPORTED HOOK PROCEDURE
 *
 * This is the piece that keeps the DLL resident in explorer.
 *
 * When SetWindowsHookEx is given a procedure that lives in the installing
 * EXE, Windows cannot deliver the hook into explorer, so it maps this DLL
 * only for the duration of the single message we send and then drops it.
 * The worker thread never gets scheduled and nothing happens - which is
 * exactly what the logs showed (last line: "thread started").
 *
 * Exporting the procedure from the DLL itself makes Windows own the
 * mapping for as long as the hook is installed, so the worker thread has
 * time to run and GlassTAP.dll can be loaded.
 *
 * The body deliberately does nothing: the work happens on the worker
 * thread, and this must stay free of COM/WinRT so the
 * InitializeXamlDiagnosticsEx precondition is untouched.
 * ===================================================================
 */
LRESULT CALLBACK GlassBoot_HookProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/*
 * Called by the injector before it releases the hook, so the taskbar can be
 * restored instead of being left with a modified brush.
 */
void GlassBoot_Shutdown(void) {
    PFN_DllGetClassObject fn = LoadFeature();
    if (fn) {
        /* The feature layer owns restoration; it exposes the service object
         * through DllGetClassObject. Reloading it here is not needed - the
         * host app calls ReturnTaskbarToDefault over COM before exiting.
         * This symbol exists so the injector can ask for cleanup; the
         * actual restore happens through the COM service. */
    }
    WriteResultLog();
}

/* ------------------------------------------------------------------ */
/* DLL entry                                                          */
/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    wchar_t path[MAX_PATH] = {0};
    wchar_t* base;
    wchar_t* p;
    HMODULE wux;

    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    /*
     * Note: CrashGuard_Install() is deliberately NOT called here.
     *
     * It was added to diagnose crashes, but this DLL must stay as close as
     * possible to the probe that was measured working. The crash filter is
     * still available in crashguard.c for builds that need it.
     */

    g_bootResult = (HRESULT)0x8000000A;   /* marker: attach seen */
    WriteResultLog();

    DisableThreadLibraryCalls(inst);

    GetModuleFileNameW(NULL, path, MAX_PATH);
    base = path;
    for (p = path; *p; ++p) { if (*p == L'\\') base = p + 1; }

    g_bootResult = (HRESULT)0x11111111;   /* marker: name known */
    WriteResultLog();

    if (lstrcmpiW(base, L"explorer.exe") != 0) {
        g_bootResult = (HRESULT)0x22222222;   /* marker: not explorer */
        WriteResultLog();
        return TRUE;
    }

    /*
     * Prepare only - do NOT call InitializeXamlDiagnosticsEx here.
     *
     * Doing it inside DllMain returned S_OK once but then crashed explorer
     * with 0xc0000409: the loader lock is held, and the call is not safe
     * under it. The hook thread is the correct place, and it is still free
     * of any COM/WinRT state at that point.
     */
    wux = LoadLibraryExW(L"Windows.UI.Xaml.dll", NULL,
                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wux) {
        g_bootResult = (HRESULT)0x33333333;
        WriteResultLog();
        return TRUE;
    }

    g_fn = (PFN_InitXamlDiagEx)(void*)GetProcAddress(
        wux, "InitializeXamlDiagnosticsEx");
    if (!g_fn) {
        g_bootResult = (HRESULT)0x44444444;
        WriteResultLog();
        return TRUE;
    }

    /* Our own path, for the wszTAPDllName argument. */
    {
        HMODULE mod = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DllMain, &mod);
        GetModuleFileNameW(mod, g_selfPath, MAX_PATH);
    }

    g_bootResult = (HRESULT)0x55555555;   /* marker: armed */
    WriteResultLog();

    /*
     * Start the bootstrap on a worker thread.
     *
     * ================================================================
     * WHERE THE CALL MUST HAPPEN - measured on build 26200.9711
     *
     *  a) Inside DllMain, synchronously.
     *     Returns S_OK, then explorer dies with 0xc0000409. The loader
     *     lock is held and the call is not safe under it. Rejected.
     *
     *  b) On a worker thread, with the injector releasing its hook
     *     immediately. The thread never gets scheduled before this module
     *     is unmapped, so the call never happens (log stops at "thread
     *     started"). This is a caller mistake, not a DLL one.
     *
     *  c) On a worker thread, with the injector HOLDING its hook and
     *     pumping messages while it waits. The thread runs normally, the
     *     loader lock is gone, and the thread has touched no COM/WinRT
     *     state. This is the placement that works.
     *
     * So the contract with the injector is:
     *   keep the hook installed, and pump until GlassTAP.dll loads.
     * ================================================================
     */
    {
        /*
         * Plain CreateThread, matching the probe that was measured working.
         *
         * CET was an early suspect - this machine has Hardware-enforced
         * Stack Protection active and explorer.exe is built with GUARD_CF -
         * but the probe proved a plain CreateThread worker runs and exits
         * cleanly inside explorer. _beginthreadex made no difference either.
         * So keep the simplest form that is known to work.
         */
        HANDLE th = CreateThread(NULL, 0, BootThread, (LPVOID)g_fn, 0, NULL);
        if (th) {
            CloseHandle(th);
            g_bootResult = (HRESULT)0x66666666;   /* marker: thread started */
        } else {
            g_bootResult = (HRESULT)0x77777777;   /* marker: thread failed */
        }
        WriteResultLog();
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* COM entry - forwarded to the feature layer                         */
/* ------------------------------------------------------------------ */

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    PFN_DllGetClassObject fn;

    if (!ppv) return E_POINTER;
    *ppv = NULL;

    fn = LoadFeature();
    if (!fn) return CLASS_E_CLASSNOTAVAILABLE;

    return fn(rclsid, riid, ppv);
}