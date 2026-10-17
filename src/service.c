// The "MixBooster GPU Affinity" service: keeps the GPU on the chosen core. A driver install or update recreates the
// device's registry key and drops the policy, so the service checks at startup and every time a display adapter
// arrives, rewrites what is missing and restarts the GPU driver to apply it.
//
// It is this same exe, copied to Program Files and started with --service.

#define SVC_NAME L"MixBoosterGpuAffinity"
#define SVC_TITLE L"MixBooster GPU Affinity"

static SERVICE_STATUS_HANDLE g_svc_handle;
static SERVICE_STATUS g_svc_status;
static HANDLE g_svc_stop, g_svc_kick;

static void svc_report(DWORD state)
{
    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState = state;
    g_svc_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_svc_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(g_svc_handle, &g_svc_status);
}

static DWORD WINAPI svc_ctrl(DWORD ctrl, DWORD type, void *data, void *ctx)
{
    (void)data;
    (void)ctx;
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        svc_report(SERVICE_STOP_PENDING);
        SetEvent(g_svc_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_DEVICEEVENT:
        if (type == DBT_DEVICEARRIVAL) SetEvent(g_svc_kick);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void svc_check(void)
{
    u64 mask = 0;
    if (!reg_get_u64(L"GpuMask", &mask) || !mask) return;
    // A test in the app moves the GPU from core to core on purpose.
    u64 busy = 0;
    if (reg_get_u64(L"BenchActive", &busy) && busy) return;
    int n = gpus_apply(mask, true, false);
    if (n > 0) log_msg("service: restored the GPU affinity (%d GPU restarted)", n);
    else if (n < 0) log_msg("service: restored the GPU affinity, it applies after a reboot");
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    (void)argc;
    (void)argv;
    g_svc_handle = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_ctrl, NULL);
    if (!g_svc_handle) return;
    g_svc_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_svc_kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    svc_report(SERVICE_RUNNING);

    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    memset(&filter, 0, sizeof filter);
    filter.dbcc_size = sizeof filter;
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = k_iface_display;
    HDEVNOTIFY dn = RegisterDeviceNotificationW(g_svc_handle, &filter, DEVICE_NOTIFY_SERVICE_HANDLE);

    // At boot the policy is normally in place already; this only acts when an update removed it while Windows was off.
    if (WaitForSingleObject(g_svc_stop, 5000) == WAIT_TIMEOUT) svc_check();
    HANDLE ev[2] = { g_svc_stop, g_svc_kick };
    for (;;) {
        // Also look every 15 minutes, in case an arrival was missed.
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 15 * 60 * 1000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            // A driver install raises several arrivals and keeps working for a while: wait until it goes quiet.
            while (WaitForSingleObject(g_svc_kick, 15000) == WAIT_OBJECT_0)
                if (WaitForSingleObject(g_svc_stop, 0) == WAIT_OBJECT_0) break;
            if (WaitForSingleObject(g_svc_stop, 0) == WAIT_OBJECT_0) break;
        }
        svc_check();
    }
    if (dn) UnregisterDeviceNotification(dn);
    svc_report(SERVICE_STOPPED);
}

static int svc_run(void)
{
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVC_NAME, svc_main }, { NULL, NULL } };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

// --- install / remove, from the app --------------------------------------------------------------------------------

static void svc_target_path(WCHAR *out)
{
    WCHAR pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) wcopy(pf, MAX_PATH, L"C:\\Program Files");
    wfmt(out, MAX_PATH, L"%s\\MixBooster\\MixBooster.exe", pf);
}

static bool svc_stop_wait(SC_HANDLE s)
{
    SERVICE_STATUS st;
    if (!QueryServiceStatus(s, &st) || st.dwCurrentState == SERVICE_STOPPED) return true;
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 100; i++) {
        if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(100);
    }
    return false;
}

// true when the service is installed and running.
static bool svc_running(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!m) return false;
    SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_QUERY_STATUS);
    SERVICE_STATUS st;
    bool on = s && QueryServiceStatus(s, &st) && (st.dwCurrentState == SERVICE_RUNNING || st.dwCurrentState == SERVICE_START_PENDING);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(m);
    return on;
}

static bool svc_install(void)
{
    WCHAR target[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH + 16];
    svc_target_path(target);
    wcopy(dir, MAX_PATH, target);
    *wcsrchr(dir, L'\\') = 0;
    CreateDirectoryW(dir, NULL);

    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!m) {
        log_msg("OpenSCManager failed: %lu", GetLastError());
        return false;
    }
    SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s) svc_stop_wait(s);
    // The service runs its own copy, so the app can live anywhere (the desktop, Downloads) and be deleted.
    if (_wcsicmp(target, g_exe_path) && !CopyFileW(g_exe_path, target, FALSE)) log_msg("copy to %ls failed: %lu", target, GetLastError());
    wfmt(cmd, countof(cmd), L"\"%s\" --service", target);
    if (s) {
        ChangeServiceConfigW(s, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd, NULL, NULL, NULL, NULL, NULL, SVC_TITLE);
    } else {
        s = CreateServiceW(m, SVC_NAME, SVC_TITLE, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd,
                           NULL, NULL, NULL, NULL, NULL);
        if (!s) log_msg("CreateService failed: %lu", GetLastError());
    }
    bool ok = false;
    if (s) {
        SERVICE_DESCRIPTIONW d = { (LPWSTR)L"Keeps the GPU interrupts on the core chosen in MixBooster after driver updates." };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        SC_ACTION act[3] = { { SC_ACTION_RESTART, 10000 }, { SC_ACTION_RESTART, 60000 }, { SC_ACTION_NONE, 0 } };
        SERVICE_FAILURE_ACTIONSW fa = { 86400, NULL, NULL, 3, act };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        ok = StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        if (!ok) log_msg("StartService failed: %lu", GetLastError());
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
    return ok;
}

static void svc_remove(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE s = m ? OpenServiceW(m, SVC_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE) : NULL;
    if (s) {
        svc_stop_wait(s);
        DeleteService(s);
        CloseServiceHandle(s);
    }
    if (m) CloseServiceHandle(m);
    WCHAR target[MAX_PATH], dir[MAX_PATH];
    svc_target_path(target);
    if (_wcsicmp(target, g_exe_path)) {
        if (!DeleteFileW(target)) MoveFileExW(target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        wcopy(dir, MAX_PATH, target);
        *wcsrchr(dir, L'\\') = 0;
        RemoveDirectoryW(dir);
    }
}

// --- the bind and reset actions, on a worker thread ----------------------------------------------------------------

static volatile LONG g_task_busy;
static int g_task_cpu;
static HWND g_task_notify;

static DWORD WINAPI bind_thread(void *arg)
{
    bool bind = arg != NULL, ok = true;
    if (bind) {
        u64 mask = 1ull << g_task_cpu;
        reg_set_u64(L"GpuMask", mask);
        gpus_apply(mask, true, false);
        ok = svc_install();
        log_msg("bound the GPU to CPU %d, service %s", g_task_cpu, ok ? "running" : "failed");
    } else {
        svc_remove();
        reg_delete(L"GpuMask");
        bridges_clear();
        gpus_apply(0, false, false);
        log_msg("GPU affinity reset");
    }
    InterlockedExchange(&g_task_busy, 0);
    PostMessageW(g_task_notify, WM_APP_TASK_DONE, ok, 0);
    return 0;
}

static void task_start(HWND notify, bool bind, int cpu)
{
    if (g_task_busy) return;
    InterlockedExchange(&g_task_busy, 1);
    g_task_notify = notify;
    g_task_cpu = cpu;
    HANDLE t = CreateThread(NULL, 0, bind_thread, bind ? (void *)1 : NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_task_busy, 0);
}

// The app was closed (or crashed) in the middle of a test: put the GPUs back.
static DWORD WINAPI recover_thread(void *arg)
{
    (void)arg;
    gpus_apply(bound_mask(), false, true);
    bench_end_mark();
    log_msg("recovered from an unfinished test");
    return 0;
}
