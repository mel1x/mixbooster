// A small panel at the top of the screen while a core warms up: the step out of all (5 of 36), the run, the core,
// and that F4 stops the test. It hides before PresentMon records frames, so it can't touch the measurement.
// Plain GDI: no Direct3D work on top of the benchmark.

#define OVL_CLASS L"MixBoosterOverlay"

static struct {
    HWND hwnd;
    HFONT bold, text;
    UINT dpi;
} OVL;

static LRESULT CALLBACK ovl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int s100 = (int)OVL.dpi * 100 / 96;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(30, 33, 31));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        RECT bar = { 0, 0, MAX(3, 4 * s100 / 100), rc.bottom };
        HBRUSH acc = CreateSolidBrush(RGB(0x6C, 0xCB, 0x7E));
        FillRect(mem, &bar, acc);
        DeleteObject(acc);

        AcquireSRWLockShared(&B.lock);
        int phase = B.phase, run = B.run, cpu = B.cpu, runs = bench_runs_shown(), step = B.done_steps + 1, total = B.total_steps;
        f64 left = B.phase_len - (time_now() - B.phase_t0);
        ReleaseSRWLockShared(&B.lock);
        WCHAR l1[96], l2[200], ph[64];
        wfmt(l1, countof(l1), TR("MixBooster  ·  шаг %d из %d", "MixBooster  ·  step %d of %d"), MIN(step, total), total);
        if (phase == PH_WARMUP) wfmt(ph, countof(ph), TR("прогрев %d с", "warm-up %d s"), (int)MAX(0.0, ceil(left)));
        else wcopy(ph, countof(ph), TR("перезапуск драйвера", "restarting the driver"));
        wfmt(l2, countof(l2), TR("Прогон %d из %d  ·  CPU %d  ·  %s  ·  F4 - остановить тест", "Run %d of %d  ·  CPU %d  ·  %s  ·  F4 stops the test"),
             run + 1, runs, cpu, ph);

        SetBkMode(mem, TRANSPARENT);
        int x = 18 * s100 / 100;
        RECT r1 = { x, 10 * s100 / 100, rc.right - 12, rc.bottom / 2 + 2 };
        SelectObject(mem, OVL.bold);
        SetTextColor(mem, RGB(240, 242, 241));
        DrawTextW(mem, l1, -1, &r1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        RECT r2 = { x, rc.bottom / 2 + 1, rc.right - 12, rc.bottom - 6 };
        SelectObject(mem, OVL.text);
        SetTextColor(mem, RGB(170, 178, 173));
        DrawTextW(mem, l2, -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ovl_fonts(UINT dpi)
{
    if (OVL.dpi == dpi && OVL.bold) return;
    if (OVL.bold) DeleteObject(OVL.bold);
    if (OVL.text) DeleteObject(OVL.text);
    OVL.dpi = dpi;
    OVL.bold = CreateFontW(-(int)(15 * dpi / 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    OVL.text = CreateFontW(-(int)(13 * dpi / 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
}

// Shown while a core's driver restarts and while it warms up; hidden otherwise.
static void ovl_update(void)
{
    AcquireSRWLockShared(&B.lock);
    int phase = B.phase;
    ReleaseSRWLockShared(&B.lock);
    bool show = B.running && !B.cancel && (phase == PH_RESTART || phase == PH_WARMUP);
    if (!show) {
        if (OVL.hwnd && IsWindowVisible(OVL.hwnd)) ShowWindow(OVL.hwnd, SW_HIDE);
        return;
    }
    if (!OVL.hwnd) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = ovl_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = OVL_CLASS;
        RegisterClassExW(&wc);
        OVL.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, OVL_CLASS, L"", WS_POPUP, 0, 0,
                                   10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!OVL.hwnd) return;
        SetLayeredWindowAttributes(OVL.hwnd, 0, 238, LWA_ALPHA);
        dwm_set(OVL.hwnd, DWA_CORNER, DWC_ROUND);
    }
    // the top middle of the main display, where the fullscreen demo runs
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    UINT dpi = window_dpi(OVL.hwnd);
    ovl_fonts(dpi);
    int w = (int)(520 * dpi / 96), h = (int)(62 * dpi / 96), mw = mi.rcMonitor.right - mi.rcMonitor.left;
    SetWindowPos(OVL.hwnd, HWND_TOPMOST, mi.rcMonitor.left + (mw - w) / 2, mi.rcMonitor.top + (int)(24 * dpi / 96), w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(OVL.hwnd, NULL, FALSE);
}
