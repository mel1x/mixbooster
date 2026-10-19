// The GPU affinity benchmark, the same method as AutoGpuAffinity: for every logical processor the GPU interrupts are
// pinned to it, the driver is restarted, liblava's triangle demo runs fullscreen, and PresentMon records its frame
// times after a warm-up. Repeating the whole pass several times averages out noise.

#define BENCH_WARMUP_SEC 30
#define BENCH_MEASURE_SEC 90
#define BENCH_MAX_RUNS 8
#define BENCH_SETTLE_SEC 6   // driver restart and launching the demo, for the time estimate

enum { M_AVG, M_MAX, M_MIN, M_LOW1, M_LOW01, M_LOW001, M_P1, M_P01, M_STDEV, M__COUNT };

typedef struct CpuResult {
    bool ok;
    f32 v[M__COUNT];
} CpuResult;

enum { PH_IDLE, PH_PREPARE, PH_RESTART, PH_WARMUP, PH_MEASURE, PH_RESTORE };

static struct {
    SRWLOCK lock;
    // results, read by the UI under the lock
    int runs, ncpu;
    CpuResult r[BENCH_MAX_RUNS][MAX_CPUS];
    i64 when;                   // FILETIME of the last result
    // progress
    volatile LONG running, cancel;
    int run, cpu, phase;
    f64 phase_t0, phase_len;
    int done_steps, total_steps;
    WCHAR error[200];
    HANDLE thread;
    HWND notify;
    // which runs this test measures: all of them, one again ("rerun"), or one more at the end ("+")
    int first_run, last_run;
    bool append;
} B;

// --- tools ---------------------------------------------------------------------------------------------------------

static WCHAR g_bin_dir[MAX_PATH];

// The demo and PresentMon are packed into the exe and written to %ProgramData%\MixBooster\bin on first use.
static bool extract_resource(int id, const WCHAR *name)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *p = g ? LockResource(g) : NULL;
    DWORD size = r ? SizeofResource(NULL, r) : 0;
    if (!p || !size) return false;
    WCHAR path[MAX_PATH];
    wfmt(path, MAX_PATH, L"%s\\%s", g_bin_dir, name);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow == size) return true;
    return write_file(path, p, size);
}

static bool tools_ready(void)
{
    wfmt(g_bin_dir, MAX_PATH, L"%s\\bin", g_data_dir);
    CreateDirectoryW(g_bin_dir, NULL);
    return extract_resource(101, L"lava-triangle.exe") && extract_resource(102, L"res.zip") && extract_resource(103, L"PresentMon.exe");
}

// liblava keeps its window state in %APPDATA%\liblava\lava triangle; fullscreen at the desktop resolution, no v-sync.
static void lava_config(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) return;
    wfmt(path, MAX_PATH, L"%s\\liblava", dir);
    CreateDirectoryW(path, NULL);
    wfmt(dir, MAX_PATH, L"%s\\lava triangle", path);
    CreateDirectoryW(dir, NULL);
    char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "{\n    \"default\": {\n        \"decorated\": true,\n        \"floating\": false,\n        \"fullscreen\": true,\n"
                     "        \"height\": %d,\n        \"maximized\": false,\n        \"monitor\": 0,\n        \"resizable\": true,\n"
                     "        \"width\": %d,\n        \"x\": 0,\n        \"y\": 0\n    }\n}\n",
                     GetSystemMetrics(SM_CYSCREEN), GetSystemMetrics(SM_CXSCREEN));
    wfmt(path, MAX_PATH, L"%s\\window.json", dir);
    write_file(path, buf, (DWORD)n);
    // its FPS window stays on, as in AutoGpuAffinity; on a laptop with an iGPU too, the demo runs on the discrete GPU
    char cfg[512];
    n = snprintf(cfg, sizeof cfg,
                 "{\n    \"auto load\": true,\n    \"auto save\": false,\n    \"delta\": 20,\n    \"fixed delta\": false,\n"
                 "    \"gui\": true,\n    \"paused\": false,\n    \"physical device\": %d,\n    \"save interval\": 300,\n"
                 "    \"speed\": 1.0,\n    \"v-sync\": false\n}\n",
                 vk_test_device());
    wfmt(path, MAX_PATH, L"%s\\config.json", dir);
    write_file(path, cfg, (DWORD)n);
}

static HANDLE spawn(const WCHAR *exe, const WCHAR *args, bool hidden)
{
    WCHAR cmd[1024];
    wfmt(cmd, countof(cmd), L"\"%s\\%s\" %s", g_bin_dir, exe, args);
    STARTUPINFOW si = { sizeof si };
    if (hidden) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, hidden ? CREATE_NO_WINDOW : 0, NULL, g_bin_dir, &si, &pi)) {
        log_msg("can't start %ls: %lu", exe, GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void kill_proc(HANDLE *p)
{
    if (!*p) return;
    if (WaitForSingleObject(*p, 0) == WAIT_TIMEOUT) {
        TerminateProcess(*p, 1);
        WaitForSingleObject(*p, 5000);
    }
    CloseHandle(*p);
    *p = NULL;
}

// --- metrics -------------------------------------------------------------------------------------------------------

static int cmp_f64(const void *a, const void *b)
{
    f64 x = *(const f64 *)a, y = *(const f64 *)b;
    return x < y ? -1 : x > y;
}

// AutoGpuAffinity's numbers: FPS from the frame times, "x% low" is the mean of the worst x% of frames and
// "x% percentile" the frame time at the (100 - x)th percentile.
static bool metrics_from_csv(const WCHAR *path, CpuResult *out)
{
    char *text = read_file(path, NULL);
    if (!text) return false;
    char *cur = text, *line = next_line(&cur);
    int col = -1;
    if (line) {
        int c = 0;
        for (char *s = line;; c++) {
            char *e = strchr(s, ',');
            if (e) *e = 0;
            if (!strcmp(s, "msBetweenPresents")) col = c;
            if (!e) break;
            s = e + 1;
        }
    }
    int n = 0, cap = 0;
    f64 *ft = NULL;
    while (col >= 0 && (line = next_line(&cur)) != NULL) {
        char *s = line;
        for (int c = 0; c < col && s; c++) {
            s = strchr(s, ',');
            if (s) s++;
        }
        if (!s) continue;
        f64 v = strtod(s, NULL);
        if (v <= 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            f64 *nf = (f64 *)realloc(ft, sizeof(f64) * (size_t)cap);
            if (!nf) break;
            ft = nf;
        }
        ft[n++] = v;
    }
    free(text);
    if (n < 100) {
        free(ft);
        return false;
    }
    // PresentMon's first interval spans the start of the capture.
    memmove(ft, ft + 1, sizeof(f64) * (size_t)(n - 1));
    n--;
    f64 sum = 0;
    for (int i = 0; i < n; i++) sum += ft[i];
    f64 mean = sum / n, var = 0;
    for (int i = 0; i < n; i++) var += (ft[i] - mean) * (ft[i] - mean);
    qsort(ft, (size_t)n, sizeof(f64), cmp_f64);

    memset(out, 0, sizeof *out);
    out->ok = true;
    out->v[M_AVG] = (f32)(1000.0 / mean);
    out->v[M_MAX] = (f32)(1000.0 / ft[0]);
    out->v[M_MIN] = (f32)(1000.0 / ft[n - 1]);
    out->v[M_STDEV] = (f32)sqrt(var / (n - 1));
    static const f64 pct[3] = { 1.0, 0.1, 0.01 };
    for (int k = 0; k < 3; k++) {
        int worst = MAX(1, (int)(n * pct[k] / 100.0));
        f64 s = 0;
        for (int i = n - worst; i < n; i++) s += ft[i];
        out->v[M_LOW1 + k] = (f32)(1000.0 / (s / worst));
        if (k < 2) {
            int idx = CLAMP((int)ceil(n * (1.0 - pct[k] / 100.0)) - 1, 0, n - 1);
            out->v[M_P1 + k] = (f32)(1000.0 / ft[idx]);
        }
    }
    free(ft);
    return true;
}

// The mean over the runs that have this CPU measured.
static bool bench_avg(int cpu, CpuResult *out)
{
    memset(out, 0, sizeof *out);
    int n = 0;
    for (int r = 0; r < B.runs; r++) {
        if (!B.r[r][cpu].ok) continue;
        for (int m = 0; m < M__COUNT; m++) out->v[m] += B.r[r][cpu].v[m];
        n++;
    }
    if (!n) return false;
    for (int m = 0; m < M__COUNT; m++) out->v[m] /= (f32)n;
    out->ok = true;
    return true;
}

// How much each metric counts in the ranking, in percent. Smoothness first: the 1% and 0.1% lows and the matching
// percentiles are what a stutter looks like. The 0.01% low is a handful of frames, so it counts less; the average
// still matters; the STDEV says how even the frame times are; min and max are single frames, mostly noise.
static const f32 k_weight[M__COUNT] = {
    [M_LOW1] = 22, [M_LOW01] = 18, [M_LOW001] = 10, [M_P1] = 12, [M_P01] = 10, [M_AVG] = 12, [M_STDEV] = 10, [M_MIN] = 4, [M_MAX] = 2,
};

static int cmp_f32(const void *a, const void *b)
{
    f32 x = *(const f32 *)a, y = *(const f32 *)b;
    return x < y ? -1 : x > y;
}

// How a core did in one run: each metric as its distance from that run's median core, in parts (+0.03 is 3% better;
// for the STDEV lower is better), capped at 15% either way so one lucky or unlucky frame in the 0.01% low or the max
// can't carry a core alone, then weighted. Against the same run's cores, so a run that went slower as a whole (a
// warmer room, something in the background) doesn't count against the cores it measured.
static f32 run_score(int r, int c, const f32 *med)
{
    f32 sc = 0;
    for (int m = 0; m < M__COUNT; m++) {
        f32 v = B.r[r][c].v[m], d = 0;
        if (v > 0 && med[m] > 0) d = m == M_STDEV ? med[m] / v - 1.f : v / med[m] - 1.f;
        sc += k_weight[m] * CLAMP(d, -0.15f, 0.15f);
    }
    return sc;
}

static f32 median_f32(f32 *v, int n)
{
    qsort(v, (size_t)n, sizeof(f32), cmp_f32);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

// Per core, filled by bench_best: in how many runs it was in the better half, out of how many counted.
static int g_core_top[MAX_CPUS], g_core_runs[MAX_CPUS];

// Ranks the cores. A core is scored in every run on its own (run_score), and its final score is the median of those
// scores minus their spread (the standard deviation): a core that was good in every run beats one that was the best
// once and poor the next time, since the GPU stays on it for good. Only runs that measured at least 3/4 of the cores
// count, a cut-off run's median isn't comparable; with none like that, every run does.
// CPU 0 is measured but never suggested: Windows sends most of its own interrupts and DPCs there, which a quiet test
// doesn't show, while a game does.
static int bench_best(int *best, int want)
{
    static f32 sc[BENCH_MAX_RUNS][MAX_CPUS];
    bool use[BENCH_MAX_RUNS] = { 0 };
    int full = 0;
    for (int r = 0; r < B.runs; r++) {
        int k = 0;
        for (int c = 0; c < B.ncpu; c++) k += B.r[r][c].ok;
        use[r] = k * 4 >= B.ncpu * 3 && k >= 2;
        full += use[r];
    }
    for (int r = 0; r < B.runs; r++)
        if (!full) use[r] = true;
    memset(g_core_top, 0, sizeof g_core_top);
    memset(g_core_runs, 0, sizeof g_core_runs);
    f32 col[MAX_CPUS];
    for (int r = 0; r < B.runs; r++) {
        if (!use[r]) continue;
        f32 med[M__COUNT];
        int k = 0;
        for (int m = 0; m < M__COUNT; m++) {
            k = 0;
            for (int c = 0; c < B.ncpu; c++)
                if (B.r[r][c].ok) col[k++] = B.r[r][c].v[m];
            med[m] = k ? median_f32(col, k) : 0;
        }
        if (!k) {
            use[r] = false;
            continue;
        }
        for (int c = 0; c < B.ncpu; c++)
            if (B.r[r][c].ok) sc[r][c] = run_score(r, c, med);
        // the better half of this run's cores
        k = 0;
        for (int c = 0; c < B.ncpu; c++)
            if (B.r[r][c].ok) col[k++] = sc[r][c];
        f32 mid = median_f32(col, k);
        for (int c = 0; c < B.ncpu; c++)
            if (B.r[r][c].ok) {
                g_core_runs[c]++;
                g_core_top[c] += sc[r][c] >= mid;
            }
    }
    f32 score[MAX_CPUS];
    int have = 0;
    for (int c = 0; c < B.ncpu; c++) {
        score[c] = -1e9f;
        int n = 0;
        for (int r = 0; r < B.runs; r++)
            if (use[r] && B.r[r][c].ok) col[n++] = sc[r][c];
        if (!n || c == 0) continue;
        f32 mean = 0, var = 0;
        for (int i = 0; i < n; i++) mean += col[i];
        mean /= (f32)n;
        for (int i = 0; i < n; i++) var += (col[i] - mean) * (col[i] - mean);
        f32 spread = n > 1 ? sqrtf(var / (f32)(n - 1)) : 0;
        score[c] = median_f32(col, n) - spread;
        have++;
    }
    int n = 0;
    for (; n < want && n < have; n++) {
        int b = -1;
        for (int c = 0; c < B.ncpu; c++)
            if (score[c] > -1e8f && (b < 0 || score[c] > score[b])) b = c;
        if (b < 0) break;
        best[n] = b;
        score[b] = -1e9f;
    }
    return n;
}

// --- results file --------------------------------------------------------------------------------------------------

static void results_path(WCHAR *out) { data_path(out, L"gpu-affinity.txt"); }

static void results_save(void)
{
    Buf b = { 0 };
    AcquireSRWLockShared(&B.lock);
    buf_printf(&b, "mixbooster gpu-affinity 1\nruns %d cpus %d when %lld\n", B.runs, B.ncpu, (long long)B.when);
    for (int r = 0; r < B.runs; r++)
        for (int c = 0; c < B.ncpu; c++) {
            if (!B.r[r][c].ok) continue;
            buf_printf(&b, "%d %d", r, c);
            for (int m = 0; m < M__COUNT; m++) buf_printf(&b, " %.4f", B.r[r][c].v[m]);
            buf_printf(&b, "\n");
        }
    ReleaseSRWLockShared(&B.lock);
    WCHAR path[MAX_PATH];
    results_path(path);
    if (b.data) write_file_atomic(path, b.data, (DWORD)b.len);
    free(b.data);
}

static void results_load(void)
{
    WCHAR path[MAX_PATH];
    results_path(path);
    char *text = read_file(path, NULL), *cur = text, *line;
    if (!text) return;
    line = next_line(&cur);
    long long when = 0;
    if (line && !strcmp(line, "mixbooster gpu-affinity 1") && (line = next_line(&cur)) != NULL &&
        sscanf(line, "runs %d cpus %d when %lld", &B.runs, &B.ncpu, &when) == 3 && B.runs >= 1 && B.runs <= BENCH_MAX_RUNS && B.ncpu >= 1 &&
        B.ncpu <= MAX_CPUS) {
        B.when = when;
        while ((line = next_line(&cur)) != NULL) {
            int r, c;
            f32 v[M__COUNT];
            if (sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f", &r, &c, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) != 11 ||
                r < 0 || r >= B.runs || c < 0 || c >= B.ncpu)
                continue;
            B.r[r][c].ok = true;
            memcpy(B.r[r][c].v, v, sizeof v);
        }
    } else {
        B.runs = B.ncpu = 0;
    }
    free(text);
}

// --- the run -------------------------------------------------------------------------------------------------------

static void set_phase(int phase, f64 len)
{
    AcquireSRWLockExclusive(&B.lock);
    B.phase = phase;
    B.phase_t0 = time_now();
    B.phase_len = len;
    ReleaseSRWLockExclusive(&B.lock);
    PostMessageW(B.notify, WM_APP_BENCH, 0, 0);
}

// What the GPUs go back to after a test: the core the service keeps them on, otherwise what they had before the test
// (set by hand or another tool), saved in BenchRestore when it started. 0 is the Windows default.
static u64 bound_mask(void)
{
    u64 m = 0;
    if (reg_get_u64(L"GpuMask", &m) && m) return m;
    return reg_get_u64(L"BenchRestore", &m) ? m : 0;
}

static void bench_end_mark(void)
{
    reg_delete(L"BenchActive");
    reg_delete(L"BenchRestore");
}

static bool measure_cpu(int run, int cpu)
{
    WCHAR csv[MAX_PATH], args[600];
    wfmt(csv, MAX_PATH, L"%s\\run%d-cpu%d.csv", g_bin_dir, run + 1, cpu);
    DeleteFileW(csv);

    set_phase(PH_RESTART, BENCH_SETTLE_SEC);
    if (gpus_apply(1ull << cpu, false, true) <= 0) {
        // Without a restart the GPU stays where it was and every core would measure the same.
        wcopy(B.error, countof(B.error),
              TR("Драйвер видеокарты не перезапускается без перезагрузки - тест невозможен", "The GPU driver can't restart without a reboot, so the test can't run"));
        B.cancel = 1;
        return false;
    }
    if (B.cancel) return false;

    HANDLE lava = spawn(L"lava-triangle.exe", L"", false), pm = NULL;
    if (!lava) {
        wcopy(B.error, countof(B.error), TR("Не удалось запустить liblava", "Couldn't start liblava"));
        B.cancel = 1;
        return false;
    }
    set_phase(PH_WARMUP, BENCH_WARMUP_SEC);
    bool ok = sleep_or_cancel(&B.cancel, BENCH_WARMUP_SEC * 1000) && WaitForSingleObject(lava, 0) == WAIT_TIMEOUT;
    if (ok) {
        set_phase(PH_MEASURE, BENCH_MEASURE_SEC);
        wfmt(args, countof(args),
             L"-process_name lava-triangle.exe -output_file \"%s\" -timed %d -terminate_after_timed -no_top -stop_existing_session "
             L"-session_name MixBooster",
             csv, BENCH_MEASURE_SEC);
        pm = spawn(L"PresentMon.exe", args, true);
        ok = pm != NULL;
        for (int t = 0; ok && t < (BENCH_MEASURE_SEC + 30) * 20; t++) {
            if (B.cancel) {
                ok = false;
                break;
            }
            if (WaitForSingleObject(pm, 50) == WAIT_OBJECT_0) break;
        }
    }
    kill_proc(&pm);
    kill_proc(&lava);
    if (!ok) return false;

    CpuResult res;
    if (!metrics_from_csv(csv, &res)) {
        log_msg("run %d cpu %d: no frames in the capture", run + 1, cpu);
        return true;  // the cell stays empty, the test goes on
    }
    DeleteFileW(csv);
    AcquireSRWLockExclusive(&B.lock);
    B.r[run][cpu] = res;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    B.when = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ReleaseSRWLockExclusive(&B.lock);
    results_save();
    log_msg("run %d cpu %d: avg %.1f, 1%% low %.1f", run + 1, cpu, res.v[M_AVG], res.v[M_LOW1]);
    return true;
}

static DWORD WINAPI bench_thread(void *arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    set_phase(PH_PREPARE, 0);
    GpuDev g[MAX_GPUS];
    if (!gpu_list(g, MAX_GPUS)) {
        wcopy(B.error, countof(B.error), TR("Видеокарта не найдена", "No GPU found"));
    } else if (!tools_ready()) {
        wcopy(B.error, countof(B.error), TR("Не удалось распаковать liblava и PresentMon", "Couldn't unpack liblava and PresentMon"));
    } else {
        lava_config();
        // A test that was cut off (a crash, a power loss) is put right on the next start, see recover_thread.
        reg_set_u64(L"BenchRestore", affinity_get(g[0].id));
        reg_set_u64(L"BenchActive", 1);
        for (int r = B.first_run; r <= B.last_run && !B.cancel; r++)
            for (int c = 0; c < B.ncpu && !B.cancel; c++) {
                AcquireSRWLockExclusive(&B.lock);
                B.run = r;
                B.cpu = c;
                ReleaseSRWLockExclusive(&B.lock);
                measure_cpu(r, c);
                if (!B.cancel) InterlockedIncrement((volatile LONG *)&B.done_steps);
            }
        if (B.append) {
            // a run added with "+" becomes a tab when it went through; one stopped half way leaves nothing behind
            AcquireSRWLockExclusive(&B.lock);
            if (!B.cancel) B.runs = B.first_run + 1;
            else memset(B.r[B.first_run], 0, sizeof B.r[0]);
            ReleaseSRWLockExclusive(&B.lock);
            results_save();
        }
        set_phase(PH_RESTORE, 0);
        gpus_apply(bound_mask(), false, true);
        bench_end_mark();
    }
    AcquireSRWLockExclusive(&B.lock);
    B.phase = PH_IDLE;
    ReleaseSRWLockExclusive(&B.lock);
    InterlockedExchange(&B.running, 0);
    PostMessageW(B.notify, WM_APP_BENCH_DONE, 0, 0);
    return 0;
}

static void bench_start(HWND notify, int runs, int ncpu)
{
    if (B.running) return;
    AcquireSRWLockExclusive(&B.lock);
    memset(B.r, 0, sizeof B.r);
    B.runs = CLAMP(runs, 1, BENCH_MAX_RUNS);
    B.ncpu = CLAMP(ncpu, 1, MAX_CPUS);
    B.first_run = 0;
    B.last_run = B.runs - 1;
    B.append = false;
    B.run = B.cpu = 0;
    B.done_steps = 0;
    B.total_steps = B.runs * B.ncpu;
    B.error[0] = 0;
    B.cancel = 0;
    B.notify = notify;
    ReleaseSRWLockExclusive(&B.lock);
    InterlockedExchange(&B.running, 1);
    if (B.thread) CloseHandle(B.thread);
    B.thread = CreateThread(NULL, 0, bench_thread, NULL, 0, NULL);
    if (!B.thread) InterlockedExchange(&B.running, 0);
}

// One run on its own: run (an existing one) is measured again from scratch, or with append one more run goes after
// the last. The other runs stay as they are.
static void bench_start_one(HWND notify, int run, bool append)
{
    if (B.running || B.ncpu <= 0 || run < 0 || run >= BENCH_MAX_RUNS) return;
    AcquireSRWLockExclusive(&B.lock);
    memset(B.r[run], 0, sizeof B.r[run]);
    B.first_run = B.last_run = run;
    B.append = append;
    B.run = run;
    B.cpu = 0;
    B.done_steps = 0;
    B.total_steps = B.ncpu;
    B.error[0] = 0;
    B.cancel = 0;
    B.notify = notify;
    ReleaseSRWLockExclusive(&B.lock);
    InterlockedExchange(&B.running, 1);
    if (B.thread) CloseHandle(B.thread);
    B.thread = CreateThread(NULL, 0, bench_thread, NULL, 0, NULL);
    if (!B.thread) InterlockedExchange(&B.running, 0);
}

// The number of runs to show, counting one being added.
static int bench_runs_shown(void) { return B.runs + (B.running && B.append ? 1 : 0); }

// Seconds left, from the steps still to go and the time left in the current one.
static f64 bench_eta(void)
{
    f64 step = BENCH_SETTLE_SEC + BENCH_WARMUP_SEC + BENCH_MEASURE_SEC;
    int left = B.total_steps - B.done_steps;
    f64 in_phase = 0;
    if (B.phase == PH_WARMUP) in_phase = BENCH_SETTLE_SEC + MIN(time_now() - B.phase_t0, (f64)BENCH_WARMUP_SEC);
    else if (B.phase == PH_MEASURE) in_phase = BENCH_SETTLE_SEC + BENCH_WARMUP_SEC + MIN(time_now() - B.phase_t0, (f64)BENCH_MEASURE_SEC);
    return MAX(0.0, left * step - in_phase);
}

// The share of the whole test that is done, 0..1.
static f32 bench_progress(void)
{
    if (B.total_steps <= 0) return 0;
    f64 step = BENCH_SETTLE_SEC + BENCH_WARMUP_SEC + BENCH_MEASURE_SEC;
    return (f32)CLAMP(1.0 - bench_eta() / (B.total_steps * step), 0.0, 1.0);
}
