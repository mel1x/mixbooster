// MixBooster: one translation unit. With --service it is the GPU affinity service, otherwise the app.
#include "base.h"
#include "com_min.h"
#include "render.c"
#include "font.c"
#include "gpu.c"
#include "bench.c"
#include "service.c"
#include "app.c"

static void init_paths(void)
{
    GetModuleFileNameW(NULL, g_exe_path, MAX_PATH);
    WCHAR pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcopy(pd, MAX_PATH, L"C:\\ProgramData");
    wfmt(g_data_dir, MAX_PATH, L"%s\\MixBooster", pd);
    CreateDirectoryW(g_data_dir, NULL);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline_a, int show)
{
    (void)inst;
    (void)prev;
    (void)cmdline_a;
    (void)show;
    init_paths();
    time_init();
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1 && !_wcsicmp(argv[1], L"--service")) return svc_run();

    g_lang_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    typedef BOOL (WINAPI *PFN_SetDpiCtx)(HANDLE);
    PFN_SetDpiCtx set_dpi = (PFN_SetDpiCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (set_dpi) set_dpi((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/);

    // One window: a second start brings the first one up.
    CreateMutexW(NULL, TRUE, APP_CLASS);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(APP_CLASS, NULL);
        if (w) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    InitializeSRWLock(&B.lock);
    u64 busy = 0;
    if (reg_get_u64(L"BenchActive", &busy) && busy) {
        HANDLE t = CreateThread(NULL, 0, recover_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    cpu_info(&A.cpu);
    A.ngpu = gpu_list(A.gpu, MAX_GPUS);
    results_load();
    // Results from another CPU don't fit this table.
    if (B.ncpu != A.cpu.count) memset(B.r, 0, sizeof B.r), B.runs = B.ncpu = 0;
    A.runs = B.runs ? B.runs : 3;
    A.tab = 0;
    A.row_hov_idx = -1;
    refresh_best();
    refresh_binding();
    if (A.bound_cpu >= 0)
        for (int i = 0; i < A.nbest; i++)
            if (A.best[i] == A.bound_cpu) A.pick = i;

    if (!app_open()) {
        MessageBoxW(NULL, TR("Не удалось создать окно (нужен Direct3D 11).", "Couldn't create the window (Direct3D 11 is required)."), L"MixBooster",
                    MB_ICONERROR);
        return 1;
    }
    return app_loop();
}
