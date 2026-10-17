#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef CINTERFACE
#define CINTERFACE
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006

#include <winsock2.h>  // before windows.h, for the network adapters in hw.c
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "uuid")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "cfgmgr32")
#endif

#include "../res/version.h"
#define MB_WIDEN2(x) L##x
#define MB_WIDEN(x) MB_WIDEN2(x)
#define MB_VER_WSTR MB_WIDEN(MB_VER_STR)

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

#define countof(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define SAFE_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static bool g_lang_ru;
#define TR(ru, en) (g_lang_ru ? L##ru : L##en)

// HKCU\Software\MixBooster\Language: 0 Russian, 1 English. Unset on the first start: Russian for a Russian
// Windows, English for any other, and that choice is saved.
#define MB_USER_KEY L"Software\\MixBooster"

static void lang_save(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MB_USER_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    DWORD v = g_lang_ru ? 0 : 1;
    RegSetValueExW(k, L"Language", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

static void lang_load(void)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, MB_USER_KEY, L"Language", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) {
        g_lang_ru = v == 0;
        return;
    }
    g_lang_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    lang_save();
}

// Follows "Animation effects" in the Windows settings; render.c reads it.
static bool g_reduced_motion;

static int wlen(const WCHAR *s) { return s ? (int)wcslen(s) : 0; }

static void wcopy(WCHAR *dst, int cap, const WCHAR *src)
{
    if (cap <= 0) return;
    int n = src ? wlen(src) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n) memcpy(dst, src, (size_t)n * sizeof(WCHAR));
    dst[n] = 0;
}

static bool wstarts_with_i(const WCHAR *s, const WCHAR *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (!*s) return false;
        if (towlower(*s) != towlower(*prefix)) return false;
    }
    return true;
}

// DWM attributes and values by name: older SDKs lack them.
enum { DWA_NO_TRANSITIONS = 3, DWA_DARK = 20, DWA_CORNER = 33, DWA_BORDER_COLOR = 34, DWA_BACKDROP = 38 };
enum { DWC_SQUARE = 1, DWC_ROUND = 2, DWC_ROUND_SMALL = 3 };
enum { DWB_NONE = 1, DWB_MICA = 2, DWB_ACRYLIC = 3 };

static bool dwm_set(HWND h, DWORD attr, DWORD v) { return SUCCEEDED(DwmSetWindowAttribute(h, attr, &v, sizeof v)); }

// A system backdrop under the whole window (the frame extended over the client area).
static bool dwm_backdrop(HWND h, DWORD type)
{
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(h, &m);
    return dwm_set(h, DWA_BACKDROP, type);
}

// swprintf into a fixed buffer, always terminated. Returns the length, or -1 when the text was cut.
static int wfmt(WCHAR *buf, int cap, const WCHAR *fmt, ...)
{
    if (cap <= 0) return -1;
    va_list ap;
    va_start(ap, fmt);
#ifdef _MSC_VER
    int n = _vsnwprintf_s(buf, (size_t)cap, _TRUNCATE, fmt, ap);
#else
    int n = _vsnwprintf(buf, (size_t)cap, fmt, ap);
    if (n < 0 || n >= cap) {
        buf[cap - 1] = 0;
        n = -1;
    }
#endif
    va_end(ap);
    return n;
}

static u64 hash_u64(u64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static f64 g_qpc_inv;

static void time_init(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_inv = 1.0 / (f64)f.QuadPart;
}

static f64 time_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (f64)c.QuadPart * g_qpc_inv;
}

// %ProgramData%\MixBooster: results, the log and the unpacked benchmark tools. Shared by the app and the service.
static WCHAR g_data_dir[MAX_PATH];
static WCHAR g_exe_path[MAX_PATH];

static void data_path(WCHAR *out, const WCHAR *file)
{
    wfmt(out, MAX_PATH, L"%s\\%s", g_data_dir, file);
}

static SRWLOCK g_log_lock = SRWLOCK_INIT;

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';

    WCHAR path[MAX_PATH];
    data_path(path, L"log.txt");
    AcquireSRWLockExclusive(&g_log_lock);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > 256 * 1024) {
            CloseHandle(f);
            f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        }
        if (f != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char ts[64];
            int tn = snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            DWORD w;
            WriteFile(f, ts, (DWORD)tn, &w, NULL);
            WriteFile(f, buf, (DWORD)n, &w, NULL);
            CloseHandle(f);
        }
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

static char *read_file(const WCHAR *path, DWORD *out_size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(f);
        return NULL;
    }
    DWORD n = (DWORD)sz.QuadPart, got = 0;
    char *buf = (char *)malloc(n + 2);
    if (buf && !ReadFile(f, buf, n, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!buf) return NULL;
    buf[got] = 0;
    buf[got + 1] = 0;
    if (out_size) *out_size = got;
    return buf;
}

static bool write_file(const WCHAR *path, const void *data, DWORD size)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, size, &w, NULL) && w == size;
    CloseHandle(f);
    return ok != 0;
}

static bool write_file_atomic(const WCHAR *path, const void *data, DWORD size)
{
    WCHAR tmp[MAX_PATH + 8];
    wfmt(tmp, countof(tmp), L"%s.tmp", path);
    if (!write_file(tmp, data, size)) {
        DeleteFileW(tmp);
        return false;
    }
    return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

typedef struct Buf {
    char *data;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        char *nd = (char *)realloc(b->data, nc);
        if (!nd) return;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_put(b, tmp, (size_t)MIN(n, (int)sizeof(tmp) - 1));
}

static char *next_line(char **cursor)
{
    char *s = *cursor;
    if (!s || !*s) return NULL;
    char *e = s;
    while (*e && *e != '\n') e++;
    char *line = s;
    *cursor = *e == '\n' ? e + 1 : e;
    if (e > s && e[-1] == '\r') e[-1] = 0;
    *e = 0;
    return line;
}

// HKLM\SOFTWARE\MixBooster: the bound core for the service, and the "test in progress" mark.
#define MB_REG_KEY L"SOFTWARE\\MixBooster"

static bool reg_get_u64(const WCHAR *name, u64 *out)
{
    u64 v = 0;
    DWORD sz = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, MB_REG_KEY, name, RRF_RT_REG_QWORD | RRF_RT_REG_DWORD | RRF_ZEROONFAILURE, NULL, &v, &sz) != ERROR_SUCCESS)
        return false;
    *out = v;
    return true;
}

static bool reg_set_u64(const WCHAR *name, u64 v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, MB_REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_QWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

static void reg_delete(const WCHAR *name) { RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, MB_REG_KEY, name); }

static bool sleep_or_cancel(volatile LONG *cancel, DWORD ms)
{
    for (DWORD t = 0; t < ms; t += 50) {
        if (cancel && *cancel) return false;
        Sleep(MIN(50, ms - t));
    }
    return !(cancel && *cancel);
}

enum {
    WM_APP_BENCH = WM_APP + 1,  // progress or a new result from the benchmark thread
    WM_APP_BENCH_DONE,
    WM_APP_TASK_DONE,           // binding or resetting finished; wp = 1 on success
    WM_APP_HW,                  // the PC components are read
};
