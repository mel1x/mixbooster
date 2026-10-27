// Updates from GitHub releases: a check a few seconds after the start and then every few hours. A newer release
// downloads by itself in the background (the setup for an installed copy, the exe for a portable one) and goes in
// on a click on the sidebar card, or by itself when the window is closed.
//
//   installed  the setup runs silently; it closes the app and the service (--shutdown) and starts the service again
//   portable   the exe is swapped in place (a running exe can be renamed); the service's own copy in Program Files
//              is brought up to date at the next start (upd_service_refresh)
//
// The dev build checks only with MB_PRETEND_VERSION set (say 0.9.0), and never applies what it downloads.

#define UPD_URL L"https://api.github.com/repos/mel1x/mixbooster/releases/latest"
#define UPD_SETUP_ASSET "MixBooster-Setup.exe"
#define UPD_PORTABLE_ASSET "MixBooster.exe"
#define UPD_EVERY_MS (6 * 3600 * 1000)
#define UPD_FIRST_MS 4000
#define TIMER_UPDATE 4

enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_DOWNLOADING, UPD_READY, UPD_ERROR };

static struct {
    volatile LONG state;
    volatile LONG progress;  // 0..1000 while downloading
    volatile LONG busy;
    WCHAR latest[32];        // the newer version, "1.1.0"
    WCHAR url[1024];
    WCHAR file[MAX_PATH];    // the download, once it is whole
    int cur[3];
    bool off, dry_run;
} UPD;

static void upd_notify(void)
{
    if (A.hwnd) PostMessageW(A.hwnd, WM_APP_UPDATE, 0, 0);
}

static void upd_set(LONG st)
{
    InterlockedExchange(&UPD.state, st);
    upd_notify();
}

static int upd_state(void) { return (int)InterlockedCompareExchange(&UPD.state, 0, 0); }

// Installed by the setup (its uninstaller is next to us), not a portable exe.
static bool upd_installed(void)
{
    WCHAR p[MAX_PATH];
    wcopy(p, MAX_PATH, g_exe_path);
    WCHAR *slash = wcsrchr(p, L'\\');
    if (!slash) return false;
    wcopy(slash + 1, (int)(MAX_PATH - (slash + 1 - p)), L"unins000.exe");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void upd_parse_ver(const char *s, int *v)
{
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3 && *s; i++) {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s != '.') break;
        s++;
    }
}

static bool upd_newer(const int *a, const int *b)
{
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static HINTERNET upd_session(void)
{
    HINTERNET s = WinHttpOpen(L"MixBooster/" MB_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"MixBooster/" MB_VER_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    }
    return s;
}

// GET an https url; the body goes to out (memory, the release JSON) or to the file (with progress). Follows redirects.
static bool upd_get(HINTERNET s, const WCHAR *url, Buf *out, HANDLE file)
{
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    WCHAR host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = countof(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (!c) return false;
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    const WCHAR *hdr = out ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (r && WinHttpSendRequest(r, hdr, out ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0;
        sz = sizeof total;
        if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &sz, WINHTTP_NO_HEADER_INDEX))
            total = 0;
        if (status == 200) {
            ok = true;
            u64 got = 0;
            static char chunk[64 * 1024];
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(r, chunk, sizeof chunk, &n)) {
                    ok = false;
                    break;
                }
                if (!n) break;
                got += n;
                if (out) {
                    if (got > 4 * 1024 * 1024) {
                        ok = false;
                        break;
                    }
                    buf_put(out, chunk, n);
                } else {
                    DWORD w = 0;
                    if (!WriteFile(file, chunk, n, &w, NULL) || w != n) {
                        ok = false;
                        break;
                    }
                    if (total) {
                        InterlockedExchange(&UPD.progress, (LONG)MIN(1000, got * 1000 / total));
                        upd_notify();
                    }
                }
            }
            if (ok && total && got != total) ok = false;
        } else {
            log_msg("update: HTTP %lu", status);
        }
    }
    if (r) WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    return ok;
}

// A "key":"value" field of the release JSON, looked for from the given point on; no escapes in the fields we read.
static bool upd_json_str(const char *from, const char *key, char *out, int cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    return *p == '"';
}

static bool upd_file_is_exe(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char mz[2] = { 0 };
    DWORD n = 0;
    LARGE_INTEGER sz;
    bool ok = ReadFile(f, mz, 2, &n, NULL) && n == 2 && mz[0] == 'M' && mz[1] == 'Z' && GetFileSizeEx(f, &sz) && sz.QuadPart > 64 * 1024;
    CloseHandle(f);
    return ok;
}

// The newest release; when it is newer than this one, its file for this kind of copy is downloaded right away.
static DWORD WINAPI upd_thread(void *arg)
{
    (void)arg;
    HINTERNET s = upd_session();
    Buf b = { 0 };
    bool ok = s && upd_get(s, UPD_URL, &b, NULL);
    LONG result = UPD_ERROR;
    if (ok) {
        buf_put(&b, "", 1);
        char tag[64], url[1024];
        const char *asset = upd_installed() ? UPD_SETUP_ASSET : UPD_PORTABLE_ASSET;
        bool found = false;
        if (upd_json_str(b.data, "tag_name", tag, sizeof tag)) {
            for (const char *p = b.data; (p = strstr(p, "\"browser_download_url\"")) != NULL; p++) {
                if (!upd_json_str(p, "browser_download_url", url, sizeof url)) continue;
                size_t n = strlen(url), m = strlen(asset);
                if (n > m && url[n - m - 1] == '/' && !_stricmp(url + n - m, asset) && !strncmp(url, "https://github.com/", 19)) {
                    found = true;
                    break;
                }
            }
            int v[3];
            upd_parse_ver(tag, v);
            result = UPD_LATEST;
            if (found && upd_newer(v, UPD.cur)) {
                const char *t = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
                MultiByteToWideChar(CP_UTF8, 0, t, -1, UPD.latest, countof(UPD.latest));
                MultiByteToWideChar(CP_UTF8, 0, url, -1, UPD.url, countof(UPD.url));
                result = UPD_DOWNLOADING;
            }
        }
    }
    free(b.data);

    if (result == UPD_DOWNLOADING) {
        InterlockedExchange(&UPD.progress, 0);
        upd_set(UPD_DOWNLOADING);
        WCHAR dir[MAX_PATH], file[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, dir);
        ok = n && n < MAX_PATH - 40;
        if (ok) {
            wcopy(dir + n, MAX_PATH - (int)n, L"MixBooster-update");
            CreateDirectoryW(dir, NULL);
            wfmt(file, MAX_PATH, L"%s\\%s", dir, upd_installed() ? L"" UPD_SETUP_ASSET : L"" UPD_PORTABLE_ASSET);
            HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            ok = f != INVALID_HANDLE_VALUE;
            if (ok) {
                ok = upd_get(s, UPD.url, NULL, f);
                CloseHandle(f);
                ok = ok && upd_file_is_exe(file);
                if (!ok) DeleteFileW(file);
            }
        }
        if (ok) wcopy(UPD.file, MAX_PATH, file);
        else log_msg("update: download failed");
        result = ok ? UPD_READY : UPD_ERROR;
    } else if (result == UPD_ERROR) {
        log_msg("update: check failed");
    }
    if (s) WinHttpCloseHandle(s);
    InterlockedExchange(&UPD.busy, 0);
    upd_set(result);
    return 0;
}

static void upd_check(void)
{
    int st = upd_state();
    if (UPD.off || st == UPD_DOWNLOADING || st == UPD_READY) return;
    if (InterlockedCompareExchange(&UPD.busy, 1, 0)) return;
    upd_set(UPD_CHECKING);
    HANDLE t = CreateThread(NULL, 0, upd_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else {
        InterlockedExchange(&UPD.busy, 0);
        upd_set(UPD_ERROR);
    }
}

// The download goes in. relaunch: the new version starts after it (a click on the card); otherwise the app is
// closing. Returns false when it couldn't: the app goes on as it was.
static bool upd_apply(bool relaunch)
{
    if (upd_state() != UPD_READY || !UPD.file[0]) return false;
    if (UPD.dry_run) {
        log_msg("update: dry run, %ls downloaded and not applied", UPD.file);
        return false;
    }
    if (upd_installed()) {
        // the setup closes this window and the service itself, before it puts the files in
        const WCHAR *args = relaunch ? L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /update=1" : L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /update=2";
        if ((INT_PTR)ShellExecuteW(NULL, L"open", UPD.file, args, NULL, SW_HIDE) <= 32) {
            log_msg("update: the setup didn't start: %lu", GetLastError());
            return false;
        }
        UPD.file[0] = 0;
        InterlockedExchange(&UPD.state, UPD_IDLE);
        return true;
    }
    WCHAR old[MAX_PATH + 8];
    wfmt(old, countof(old), L"%s.old", g_exe_path);
    DeleteFileW(old);
    if (!MoveFileExW(g_exe_path, old, MOVEFILE_REPLACE_EXISTING)) {
        log_msg("update: can't move the exe aside: %lu", GetLastError());
        return false;
    }
    if (!MoveFileExW(UPD.file, g_exe_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        log_msg("update: can't put the new exe in: %lu", GetLastError());
        MoveFileExW(old, g_exe_path, MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    UPD.file[0] = 0;
    InterlockedExchange(&UPD.state, UPD_IDLE);
    // the new one waits for this one's window to go (the single-window mutex), see upd_wait_old
    if (relaunch) {
        WCHAR args[32];
        wfmt(args, countof(args), L"--after-update %lu", GetCurrentProcessId());
        ShellExecuteW(NULL, L"open", g_exe_path, args, NULL, SW_SHOWNORMAL);
    }
    return true;
}

// The version of an exe on disk, "1.0.0", or empty.
static void upd_file_version(const WCHAR *path, WCHAR *out, int cap)
{
    out[0] = 0;
    DWORD h = 0, sz = GetFileVersionInfoSizeW(path, &h);
    if (!sz) return;
    void *data = malloc(sz);
    if (!data) return;
    VS_FIXEDFILEINFO *fi = NULL;
    UINT fl = 0;
    if (GetFileVersionInfoW(path, 0, sz, data) && VerQueryValueW(data, L"\\", (void **)&fi, &fl) && fi)
        wfmt(out, cap, L"%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    free(data);
}

static DWORD WINAPI upd_svc_thread(void *arg)
{
    (void)arg;
    svc_install();
    return 0;
}

// A portable copy was updated: the service still runs its own older copy from Program Files; put this one there.
static void upd_service_refresh(void)
{
    WCHAR target[MAX_PATH], ver[32];
    svc_target_path(target);
    if (!_wcsicmp(target, g_exe_path) || !svc_running()) return;
    upd_file_version(target, ver, countof(ver));
    if (!ver[0] || !wcscmp(ver, MB_VER_WSTR)) return;
    log_msg("update: the service runs %ls, bringing it to " MB_VER_STR, ver);
    HANDLE t = CreateThread(NULL, 0, upd_svc_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void upd_init(void)
{
    const char *cur = MB_VER_STR;
    char pretend[32] = "";
#ifdef MB_DEV_BUILD
    // the dev build: only with a made-up version, and only as far as the download
    UPD.off = !GetEnvironmentVariableA("MB_PRETEND_VERSION", pretend, sizeof pretend);
    UPD.dry_run = true;
    if (pretend[0]) cur = pretend;
#endif
    upd_parse_ver(cur, UPD.cur);
    // a portable update leaves the old exe behind
    WCHAR old[MAX_PATH + 8];
    wfmt(old, countof(old), L"%s.old", g_exe_path);
    DeleteFileW(old);
    if (!UPD.off) SetTimer(A.hwnd, TIMER_UPDATE, UPD_FIRST_MS, NULL);
}

static void upd_on_timer(void)
{
    SetTimer(A.hwnd, TIMER_UPDATE, UPD_EVERY_MS, NULL);
    upd_check();
}

// --after-update <pid>: the old copy is still closing; wait for it so the single-window check lets this one in.
static void upd_wait_old(int argc, WCHAR **argv)
{
    if (argc < 3 || _wcsicmp(argv[1], L"--after-update")) return;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[2], NULL, 10));
    if (p) {
        WaitForSingleObject(p, 15000);
        CloseHandle(p);
    }
}
