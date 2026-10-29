// "A restart is due": the changes that only a restart applies, shown in the sidebar on every page.
//
// Each change leaves a value under HKLM\SOFTWARE\MixBooster\Reboot named after it, holding the boot it was made in. A
// value from an earlier boot has been applied and goes. A change that can be undone in the same boot (a switch on the
// Graphics page turned back) also keeps "<name>.was", the state the PC booted with: back on it, the mark goes too.
// Windows Update waiting for a restart counts as well.

#define RB_KEY MB_REG_KEY L"\\Reboot"

// When this boot began, in 100 ns units. GetTickCount64 runs through sleep and hibernation (fast startup too), and
// those don't apply what waits for a restart either.
static u64 boot_time(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - GetTickCount64() * 10000ull;
}

// The clock may have been set since the boot: a minute either way is the same boot.
static bool same_boot(u64 t)
{
    u64 b = boot_time();
    return (t > b ? t - b : b - t) < 60ull * 10000000ull;
}

static bool rb_get(const WCHAR *name, u64 *t)
{
    DWORD sz = sizeof *t;
    return RegGetValueW(HKEY_LOCAL_MACHINE, RB_KEY, name, RRF_RT_REG_QWORD, NULL, t, &sz) == ERROR_SUCCESS;
}

static void rb_drop(const WCHAR *name)
{
    WCHAR was[64];
    wfmt(was, countof(was), L"%s.was", name);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, RB_KEY, name);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, RB_KEY, was);
}

// A change made now that applies after a restart. was: the state before it, -1 when it can't be undone in place.
static void reboot_change(const WCHAR *name, int was, int now)
{
    u64 t = 0;
    WCHAR wn[64];
    wfmt(wn, countof(wn), L"%s.was", name);
    if (rb_get(name, &t) && same_boot(t)) {
        DWORD w = 0, sz = sizeof w;
        if (now >= 0 && RegGetValueW(HKEY_LOCAL_MACHINE, RB_KEY, wn, RRF_RT_REG_DWORD, NULL, &w, &sz) == ERROR_SUCCESS && (int)w == now) rb_drop(name);
        return;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RB_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    t = boot_time();
    RegSetValueExW(k, name, 0, REG_QWORD, (const BYTE *)&t, sizeof t);
    if (was >= 0) {
        DWORD w = (DWORD)was;
        RegSetValueExW(k, wn, 0, REG_DWORD, (const BYTE *)&w, sizeof w);
    } else {
        RegDeleteValueW(k, wn);
    }
    RegCloseKey(k);
}

static void reboot_mark(const WCHAR *name) { reboot_change(name, -1, -1); }

// Drops a mark whatever it holds: the thing it was for is known to be in place (HAGS reads its own state).
static void reboot_clear(const WCHAR *name) { rb_drop(name); }

static const WCHAR *rb_title(const WCHAR *name)
{
    if (!wcscmp(name, L"Msi")) return L"MSI Mode";
    if (!wcscmp(name, L"Gpu")) return L"GPU Affinity";
    if (!wcscmp(name, L"Hags") || !wcscmp(name, L"Mpo") || !wcscmp(name, L"Vbs")) return TR("Графика", "Graphics");
    return name;
}

static bool key_exists(const WCHAR *path)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

// What waits for a restart, as "MSI Mode, Graphics"; the number of things. Stale marks are removed on the way.
static int reboot_list(WCHAR *out, int cap)
{
    out[0] = 0;
    WCHAR names[16][64];
    int n = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RB_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0; n < countof(names); i++) {
            DWORD len = countof(names[0]);
            if (RegEnumValueW(k, i, names[n], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (!wcschr(names[n], L'.')) n++;
        }
        RegCloseKey(k);
    }
    int shown = 0, len = 0;
    const WCHAR *seen[16];
    for (int i = 0; i < n; i++) {
        u64 t = 0;
        if (!rb_get(names[i], &t) || !same_boot(t)) {
            rb_drop(names[i]);
            continue;
        }
        const WCHAR *title = rb_title(names[i]);
        bool dup = false;
        for (int j = 0; j < shown; j++) dup |= seen[j] == title || !wcscmp(seen[j], title);
        if (dup) continue;
        seen[shown++] = title;
        len += wfmt(out + len, cap - len, L"%s%s", len ? L", " : L"", title);
    }
    if (key_exists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
        key_exists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending")) {
        wfmt(out + len, cap - len, L"%s%s", len ? L", " : L"", L"Windows Update");
        shown++;
    }
    return shown;
}

static bool reboot_now(void)
{
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp = { 1 };
        LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    if (ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED)) return true;
    log_msg("reboot failed: %lu", GetLastError());
    return false;
}
