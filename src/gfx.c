// The "Graphics" page: the Windows switches for games and the GPU, the ones in Settings > Display > Graphics and a few
// it hides. Each one flips on a click and applies on the job thread (jobs.c), going back if the change didn't take;
// HAGS and MPO wait for a restart.
//
//   HAGS   HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers  HwSchMode = 2 on, 1 off; absent: the driver's default.
//          Whether the GPU can do it and runs it now comes from the kernel driver interface (D3DKMT, WDDM 2.7 caps).
//   MPO    HKLM\SOFTWARE\Microsoft\Windows\Dwm  OverlayTestMode = 5 turns it off; 24H2 and later read
//          ...\GraphicsDrivers  DisableOverlays = 1 instead. Off writes both.
//   VRR, windowed games
//          HKCU\Software\Microsoft\DirectX\UserGpuPreferences  DirectXUserGlobalSettings = "VRROptimizeEnable=1;
//          SwapEffectUpgradeEnable=1;": the string Settings writes, other keys in it kept.
//   FSO    HKCU\System\GameConfigStore  GameDVR_DXGIHonorFSEWindowsCompatible = 1 and GameDVR_HonorUserFSEBehaviorMode
//          = 1 (with GameDVR_FSEBehaviorMode and GameDVR_FSEBehavior = 2): fullscreen optimizations off for every game.
//   VBS    HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard  EnableVirtualizationBasedSecurity and
//          ...\Scenarios\HypervisorEnforcedCodeIntegrity  Enabled (memory integrity), both 0 or both 1; applies after a
//          restart. Whether it runs now comes from NtQuerySystemInformation. A group policy or a UEFI lock wins over these
//          values: the switch is off then.
//
// What MPO and FSO had before the first change is kept in HKLM\SOFTWARE\MixBooster\Gfx, so turning them back on
// restores it exactly.

enum { GX_HAGS, GX_MPO, GX_VRR, GX_WGO, GX_FSO, GX_VBS, GX__COUNT };

#define GX_DRV_KEY L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers"
#define GX_DWM_KEY L"SOFTWARE\\Microsoft\\Windows\\Dwm"
#define GX_DX_KEY L"Software\\Microsoft\\DirectX\\UserGpuPreferences"
#define GX_GCS_KEY L"System\\GameConfigStore"
#define GX_SAVE_KEY MB_REG_KEY L"\\Gfx"
#define GX_DG_KEY L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard"
#define GX_HVCI_KEY GX_DG_KEY L"\\Scenarios\\HypervisorEnforcedCodeIntegrity"
#define GX_DG_POLICY L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceGuard"
#define GX_ROW_H 90.f

static struct {
    bool ready;
    bool on[GX__COUNT];
    bool hags_sup, hags_run, hags_def;
    bool vbs_run;
    int vbs_lock;  // 0, or what keeps VBS as it is: 1 a group policy, 2 a UEFI lock
    f32 tog[GX__COUNT], togv[GX__COUNT];
    int seq[GX__COUNT], pend;  // clicks per switch, and jobs not done: Windows is read again once they are
    f32 scroll, scroll_target, scroll_v, content_h, sb_t;
    Box card, rows[GX__COUNT], sw[GX__COUNT];
} GX;

// --- the registry --------------------------------------------------------------------------------------------------

// -1 when absent.
static int gx_get(HKEY root, const WCHAR *key, const WCHAR *name)
{
    DWORD v = 0, sz = sizeof v;
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? (int)v : -1;
}

// v -1 deletes.
static void gx_put(HKEY root, const WCHAR *key, const WCHAR *name, int v)
{
    if (v < 0) {
        RegDeleteKeyValueW(root, key, name);
        return;
    }
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        log_msg("gfx: can't open %ls", key);
        return;
    }
    DWORD d = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof d);
    RegCloseKey(k);
}

// The value before MixBooster's first change, saved once: 0xFFFFFFFF stands for "absent".
static void gx_save_old(const WCHAR *as, int v)
{
    DWORD sz = sizeof(DWORD), d = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, GX_SAVE_KEY, as, RRF_RT_REG_DWORD, NULL, &d, &sz) == ERROR_SUCCESS) return;
    gx_put(HKEY_LOCAL_MACHINE, GX_SAVE_KEY, as, v < 0 ? (int)0xFFFFFFFF : v);
}

// The saved value (-1 absent), or def when nothing was saved; the saved one is used up.
static int gx_take_old(const WCHAR *as, int def)
{
    DWORD d = 0, sz = sizeof d;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, GX_SAVE_KEY, as, RRF_RT_REG_DWORD, NULL, &d, &sz) != ERROR_SUCCESS) return def;
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, GX_SAVE_KEY, as);
    return d == 0xFFFFFFFF ? -1 : (int)d;
}

// HAGS as the kernel sees it: supported by a GPU, running now, on by the driver's default. Through D3DKMT in gdi32.
typedef struct MbKmtAdapterInfo {
    UINT hAdapter;
    LUID luid;
    ULONG sources;
    BOOL precise;
} MbKmtAdapterInfo;
typedef struct MbKmtEnum2 {
    ULONG n;
    MbKmtAdapterInfo *adapters;
} MbKmtEnum2;
typedef struct MbKmtQuery {
    UINT hAdapter;
    UINT type;
    void *data;
    UINT size;
} MbKmtQuery;
typedef LONG(APIENTRY *PFN_MbKmtEnum2)(MbKmtEnum2 *);
typedef LONG(APIENTRY *PFN_MbKmtQuery)(MbKmtQuery *);
typedef LONG(APIENTRY *PFN_MbKmtClose)(UINT *);
#define MB_KMTQAITYPE_WDDM_2_7_CAPS 70

static void hags_caps(bool *sup, bool *run, bool *def)
{
    *sup = *run = *def = false;
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    PFN_MbKmtEnum2 en = g ? (PFN_MbKmtEnum2)(void *)GetProcAddress(g, "D3DKMTEnumAdapters2") : NULL;
    PFN_MbKmtQuery qu = g ? (PFN_MbKmtQuery)(void *)GetProcAddress(g, "D3DKMTQueryAdapterInfo") : NULL;
    PFN_MbKmtClose cl = g ? (PFN_MbKmtClose)(void *)GetProcAddress(g, "D3DKMTCloseAdapter") : NULL;
    if (!en || !qu || !cl) return;
    MbKmtAdapterInfo ad[16];
    MbKmtEnum2 e = { countof(ad), ad };
    if (en(&e) != 0) return;
    for (ULONG i = 0; i < e.n && i < countof(ad); i++) {
        UINT caps = 0;
        MbKmtQuery q = { ad[i].hAdapter, MB_KMTQAITYPE_WDDM_2_7_CAPS, &caps, sizeof caps };
        if (qu(&q) == 0) {
            *sup |= (caps & 1) != 0;
            *run |= (caps & 2) != 0;
            *def |= (caps & 4) != 0;
        }
        cl(&ad[i].hAdapter);
    }
}

// One key of the DirectX settings string: 1, 0, or -1 when it isn't there.
static int dx_get(const WCHAR *name)
{
    WCHAR s[512];
    DWORD sz = sizeof s;
    if (RegGetValueW(HKEY_CURRENT_USER, GX_DX_KEY, L"DirectXUserGlobalSettings", RRF_RT_REG_SZ, NULL, s, &sz) != ERROR_SUCCESS) return -1;
    int nl = wlen(name);
    for (WCHAR *p = s; *p;) {
        WCHAR *end = wcschr(p, L';');
        if (!_wcsnicmp(p, name, nl) && p[nl] == L'=') return _wtoi(p + nl + 1) != 0;
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

static void dx_put(const WCHAR *name, int v)
{
    WCHAR s[512] = L"", out[600] = L"";
    DWORD sz = sizeof s;
    RegGetValueW(HKEY_CURRENT_USER, GX_DX_KEY, L"DirectXUserGlobalSettings", RRF_RT_REG_SZ, NULL, s, &sz);
    int nl = wlen(name), len = 0;
    // every other key as it was, this one at the end
    for (WCHAR *p = s; *p;) {
        WCHAR *end = wcschr(p, L';');
        int n = end ? (int)(end - p) : wlen(p);
        if (n && !(!_wcsnicmp(p, name, nl) && p[nl] == L'=')) len += wfmt(out + len, countof(out) - len, L"%.*s;", n, p);
        if (!end) break;
        p = end + 1;
    }
    wfmt(out + len, countof(out) - len, L"%s=%d;", name, v);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, GX_DX_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"DirectXUserGlobalSettings", 0, REG_SZ, (const BYTE *)out, (DWORD)((wlen(out) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
}

static bool mpo_on(void)
{
    return gx_get(HKEY_LOCAL_MACHINE, GX_DWM_KEY, L"OverlayTestMode") != 5 && gx_get(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"DisableOverlays") != 1;
}

// VBS running now: the secure kernel, or memory integrity where the first query isn't there.
typedef LONG(NTAPI *PFN_MbNtQuerySysInfo)(ULONG, void *, ULONG, ULONG *);

static bool vbs_running(void)
{
    PFN_MbNtQuerySysInfo q = (PFN_MbNtQuerySysInfo)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!q) return false;
    u8 ium[16] = { 0 };
    // SystemIsolatedUserModeInformation: bit 0 the secure kernel runs, bit 1 memory integrity
    if (q(165, ium, sizeof ium, NULL) == 0) return (ium[0] & 3) != 0;
    // SystemCodeIntegrityInformation: CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED
    ULONG ci[2] = { sizeof ci, 0 };
    return q(103, ci, sizeof ci, NULL) == 0 && (ci[1] & 0x400);
}

// On as set: either value 1; with neither there, what runs now (Windows' own default).
static bool vbs_on(void)
{
    int v = gx_get(HKEY_LOCAL_MACHINE, GX_DG_KEY, L"EnableVirtualizationBasedSecurity"), h = gx_get(HKEY_LOCAL_MACHINE, GX_HVCI_KEY, L"Enabled");
    if (v < 0 && h < 0) return vbs_running();
    return v == 1 || h == 1;
}

static int vbs_lock(void)
{
    if (gx_get(HKEY_LOCAL_MACHINE, GX_DG_POLICY, L"EnableVirtualizationBasedSecurity") >= 0) return 1;
    if (gx_get(HKEY_LOCAL_MACHINE, GX_DG_KEY, L"Locked") == 1 || gx_get(HKEY_LOCAL_MACHINE, GX_HVCI_KEY, L"Locked") == 1) return 2;
    return 0;
}

static bool fso_on(void)
{
    return !(gx_get(HKEY_CURRENT_USER, GX_GCS_KEY, L"GameDVR_DXGIHonorFSEWindowsCompatible") == 1 &&
             gx_get(HKEY_CURRENT_USER, GX_GCS_KEY, L"GameDVR_HonorUserFSEBehaviorMode") == 1);
}

static void gx_read(void)
{
    hags_caps(&GX.hags_sup, &GX.hags_run, &GX.hags_def);
    int m = gx_get(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"HwSchMode");
    // no value: the driver's choice, which is what runs now
    GX.on[GX_HAGS] = m == 2 || (m < 0 && GX.hags_run);
    GX.on[GX_MPO] = mpo_on();
    GX.on[GX_VRR] = dx_get(L"VRROptimizeEnable") == 1;
    GX.on[GX_WGO] = dx_get(L"SwapEffectUpgradeEnable") == 1;
    GX.on[GX_FSO] = fso_on();
    GX.vbs_run = vbs_running();
    GX.vbs_lock = vbs_lock();
    GX.on[GX_VBS] = vbs_on();
    // HAGS knows what it runs on: a restart is due exactly while the two differ
    if (GX.hags_sup && GX.on[GX_HAGS] != GX.hags_run) reboot_mark(L"Hags");
    else reboot_clear(L"Hags");
    if (!GX.ready)
        for (int i = 0; i < GX__COUNT; i++) GX.tog[i] = GX.on[i] ? 1.f : 0.f;
    GX.ready = true;
}

static const WCHAR *k_fso_vals[] = { L"GameDVR_FSEBehaviorMode", L"GameDVR_HonorUserFSEBehaviorMode", L"GameDVR_FSEBehavior",
                                     L"GameDVR_DXGIHonorFSEWindowsCompatible" };

static void gx_set(int i, bool on)
{
    switch (i) {
    case GX_HAGS:
        gx_put(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"HwSchMode", on ? 2 : 1);
        break;
    case GX_MPO:
        if (on) {
            int old = gx_take_old(L"OverlayTestMode", -1);
            gx_put(HKEY_LOCAL_MACHINE, GX_DWM_KEY, L"OverlayTestMode", old == 5 ? -1 : old);
            gx_put(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"DisableOverlays", -1);
        } else {
            gx_save_old(L"OverlayTestMode", gx_get(HKEY_LOCAL_MACHINE, GX_DWM_KEY, L"OverlayTestMode"));
            gx_put(HKEY_LOCAL_MACHINE, GX_DWM_KEY, L"OverlayTestMode", 5);
            gx_put(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"DisableOverlays", 1);
        }
        reboot_change(L"Mpo", on ? 0 : 1, on ? 1 : 0);
        break;
    case GX_VRR: dx_put(L"VRROptimizeEnable", on); break;
    case GX_WGO: dx_put(L"SwapEffectUpgradeEnable", on); break;
    case GX_FSO:
        for (int k = 0; k < countof(k_fso_vals); k++) {
            WCHAR as[64];
            wfmt(as, countof(as), L"Fso.%s", k_fso_vals[k]);
            if (on) {
                // back to what was there, or Windows' own: the user's per-game choice not honored, DXGI's default
                static const int def[4] = { 0, 0, -1, 0 };
                gx_put(HKEY_CURRENT_USER, GX_GCS_KEY, k_fso_vals[k], gx_take_old(as, def[k]));
            } else {
                gx_save_old(as, gx_get(HKEY_CURRENT_USER, GX_GCS_KEY, k_fso_vals[k]));
                gx_put(HKEY_CURRENT_USER, GX_GCS_KEY, k_fso_vals[k], k == 0 || k == 2 ? 2 : 1);
            }
        }
        break;
    case GX_VBS:
        if (!on) {
            gx_save_old(L"Vbs.Enable", gx_get(HKEY_LOCAL_MACHINE, GX_DG_KEY, L"EnableVirtualizationBasedSecurity"));
            gx_save_old(L"Vbs.Hvci", gx_get(HKEY_LOCAL_MACHINE, GX_HVCI_KEY, L"Enabled"));
        } else {
            // on is on: what was there before goes, Windows' defaults could leave it off
            gx_take_old(L"Vbs.Enable", -1);
            gx_take_old(L"Vbs.Hvci", -1);
        }
        gx_put(HKEY_LOCAL_MACHINE, GX_DG_KEY, L"EnableVirtualizationBasedSecurity", on ? 1 : 0);
        gx_put(HKEY_LOCAL_MACHINE, GX_HVCI_KEY, L"Enabled", on ? 1 : 0);
        reboot_change(L"Vbs", on ? 0 : 1, on ? 1 : 0);
        break;
    }
    log_msg("gfx: %d -> %s", i, on ? "on" : "off");
}

// What the registry says now, for a job to check its change took.
static bool gx_is_on(int i)
{
    switch (i) {
    case GX_HAGS: return gx_get(HKEY_LOCAL_MACHINE, GX_DRV_KEY, L"HwSchMode") == 2;
    case GX_MPO: return mpo_on();
    case GX_VRR: return dx_get(L"VRROptimizeEnable") == 1;
    case GX_WGO: return dx_get(L"SwapEffectUpgradeEnable") == 1;
    case GX_FSO: return fso_on();
    case GX_VBS: return vbs_on();
    }
    return false;
}

static bool gx_job_run(Job *j)
{
    gx_set(j->idx, j->on);
    return gx_is_on(j->idx) == j->on;
}

static void gx_job_done(Job *j, bool ok)
{
    GX.pend--;
    if (!ok && j->seq == GX.seq[j->idx]) {
        GX.on[j->idx] = !j->on;
        toast_show(TR("Не удалось изменить настройку графики: Windows не дала записать её в реестр",
                      "Couldn't change the graphics setting: Windows didn't let it into the registry"));
    }
    // all done: what Windows has now, with the restart marks
    if (!GX.pend) {
        gx_read();
        refresh_reboot();
    }
}

// --- the page ------------------------------------------------------------------------------------------------------

static const struct {
    u32 glyph;
    const WCHAR *ru, *en, *dru, *den;
} k_gx[GX__COUNT] = {
    { 0xF211, L"Аппаратное планирование GPU (HAGS)", L"Hardware-accelerated GPU scheduling (HAGS)",
      L"Видеокарта сама ведёт очередь своих задач, а не процессор. Нужно для генерации кадров DLSS; на старых драйверах иногда даёт микрофризы.",
      L"The GPU runs its own work queue instead of the CPU. Needed for DLSS frame generation; on old drivers it can cause stutter." },
    { 0xE7F4, L"Multiplane Overlay (MPO)", L"Multiplane Overlay (MPO)",
      L"Даёт играм в окне без рамки выводить кадры в обход DWM, с задержкой как в полноэкранном режиме. Выключайте, только если мерцает экран или видео в браузере фризит поверх игры.",
      L"Lets borderless games skip DWM and present with fullscreen latency. Turn it off only if the screen flickers or browser video stutters over a game." },
    { 0xE7FC, L"Переменная частота обновления", L"Variable refresh rate",
      L"G-Sync и FreeSync для игр DirectX 11, которые сами их не включают. Нужен монитор с VRR.",
      L"G-Sync and FreeSync for DirectX 11 games that don't turn it on themselves. Needs a VRR display." },
    { 0xE737, L"Оптимизации для оконных игр", L"Optimizations for windowed games",
      L"Старые игры DirectX 10 и 11 в окне переходят на flip-модель: меньше задержка, работают VRR и Auto HDR.",
      L"Older DirectX 10 and 11 games in a window move to the flip model: lower latency, and VRR and Auto HDR work." },
    { 0xE740, L"Оптимизация во весь экран", L"Fullscreen optimizations",
      L"Windows подменяет эксклюзивный полноэкранный режим окном без рамки. Выключение возвращает настоящий эксклюзивный режим, в старых играх задержка бывает ниже.",
      L"Windows swaps exclusive fullscreen for a borderless window. Off brings real exclusive fullscreen back; in older games latency can drop." },
    { 0xEA18, L"Изоляция ядра (VBS и целостность памяти)", L"Core isolation (VBS and memory integrity)",
      L"Защищает ядро Windows от вредоносных драйверов, но в части игр стоит несколько процентов FPS. Выключение снижает защиту ПК.",
      L"Shields the Windows kernel from malicious drivers but costs a few percent of FPS in some games. Off lowers the PC's protection." },
};

static f32 gx_view_h(void) { return (f32)A.H - S(HW_TOP) - S(8); }
static f32 gx_max_scroll(void) { return MAX(0.f, GX.content_h - gx_view_h()); }

static void gx_layout(void)
{
    f32 x0 = content_x0(), cw = content_x1() - x0, y = S(HW_TOP) - floorf(GX.scroll + 0.5f);
    GX.card = (Box){ x0, y, cw, S(GX_ROW_H) * GX__COUNT + S(12) };
    for (int i = 0; i < GX__COUNT; i++) {
        GX.rows[i] = (Box){ x0, y + S(6) + S(GX_ROW_H) * (f32)i, cw, S(GX_ROW_H) };
        GX.sw[i] = (Box){ x0 + cw - S(24) - S(52), GX.rows[i].y + (S(GX_ROW_H) - S(32)) * 0.5f, S(52), S(32) };
    }
    GX.content_h = GX.card.h + S(16);
}

// A row of a settings card: the icon, the name, the explanation in up to two lines, and a note under it.
static void draw_set_row(const Box *r, f32 right, u32 glyph, const WCHAR *title, const WCHAR *desc, const WCHAR *note, u32 note_col)
{
    const Theme *t = &A.th;
    f32 isz = S(36), ix = r->x + S(20), tx = ix + isz + S(16), tw = right - tx - S(20);
    draw_icon_tile(ix, r->y + S(16), isz, glyph);
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(14), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(14), r->y + S(24)), tw, title, -1, t->text, false);
    f32 lh = S(17), dy = baseline_for(FONT_TEXT, SC(12.5f), r->y + S(45));
    int lines = text_wrap(FONT_TEXT, SC(12.5f), tx, dy, tw, lh, desc, t->dim, false);
    if (lines > 2) {
        text_draw_fit(FONT_TEXT, SC(12.5f), tx, dy, tw, desc, -1, t->dim, false);
        lines = 1;
    } else {
        text_wrap(FONT_TEXT, SC(12.5f), tx, dy, tw, lh, desc, t->dim, true);
    }
    if (note && note[0]) {
        f32 ny = dy + lh * (f32)lines + S(3);
        f32 d = S(6);
        r_rect(tx, floorf(ny - S(4) - d * 0.5f), d, d, note_col, d * 0.5f);
        text_draw_fit(FONT_TEXT, SC(12), tx + d + S(7), ny, tw - d - S(7), note, -1, note_col, false);
    }
}

static void draw_gfx_page(void)
{
    const Theme *t = &A.th;
    if (!GX.ready && !GX.pend) gx_read();
    gx_layout();
    f32 top = S(HW_TOP) - S(6);
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    Box *b = &GX.card;
    draw_card(b);
    for (int i = 0; i < GX__COUNT; i++) {
        Box *r = &GX.rows[i];
        WCHAR note[160] = L"";
        u32 nc = t->dim;
        bool enabled = true;
        if (i == GX_HAGS) {
            if (!GX.hags_sup) {
                enabled = false;
                wcopy(note, countof(note), TR("Видеокарта или драйвер не поддерживают", "The GPU or its driver doesn't support it"));
                nc = t->faint;
            } else if (GX.on[i] != GX.hags_run) {
                wcopy(note, countof(note), GX.on[i] ? TR("Включится после перезагрузки", "Turns on after a restart") : TR("Выключится после перезагрузки", "Turns off after a restart"));
                nc = t->warn;
            } else {
                wcopy(note, countof(note), GX.hags_run ? TR("Работает сейчас", "Running now") : TR("Сейчас выключено", "Off now"));
                nc = GX.hags_run ? t->good : t->dim;
            }
        } else if (i == GX_MPO) {
            u64 tm = 0;
            if (rb_get(L"Mpo", &tm) && same_boot(tm)) {
                wcopy(note, countof(note), TR("Изменится после перезагрузки", "Changes after a restart"));
                nc = t->warn;
            } else if (!GX.on[i]) {
                wcopy(note, countof(note), TR("Выключено: игры в окне без рамки выводят кадры через DWM", "Off: borderless games present through DWM"));
                nc = t->dim;
            }
        } else if (i == GX_VBS) {
            u64 tm = 0;
            if (GX.vbs_lock) {
                enabled = false;
                wcopy(note, countof(note), GX.vbs_lock == 1 ? TR("Задано групповой политикой", "Set by a group policy")
                                                            : TR("Заблокировано в UEFI: меняется только через BIOS", "Locked in UEFI: only the BIOS changes it"));
                nc = t->faint;
            } else if (rb_get(L"Vbs", &tm) && same_boot(tm)) {
                wcopy(note, countof(note), GX.on[i] ? TR("Включится после перезагрузки", "Turns on after a restart") : TR("Выключится после перезагрузки", "Turns off after a restart"));
                nc = t->warn;
            } else if (!GX.on[i] && GX.vbs_run) {
                wcopy(note, countof(note), TR("Всё ещё работает: его держат Hyper-V, WSL или Credential Guard", "Still running: Hyper-V, WSL or Credential Guard keeps it"));
                nc = t->warn;
            } else {
                wcopy(note, countof(note), GX.vbs_run ? TR("Работает сейчас", "Running now") : TR("Сейчас выключено", "Off now"));
                nc = GX.vbs_run ? t->dim : t->good;
            }
        } else {
            wcopy(note, countof(note), TR("Для игр, запущенных после изменения", "For games started after the change"));
            nc = t->faint;
        }
        draw_set_row(r, GX.sw[i].x - S(16), k_gx[i].glyph, g_lang_ru ? k_gx[i].ru : k_gx[i].en, g_lang_ru ? k_gx[i].dru : k_gx[i].den, note, nc);
        draw_switch(&GX.sw[i], GX.tog[i], enabled, enabled && A.hover.type == HT_GFXTOG && A.hover.idx == i);
        if (i + 1 < GX__COUNT) r_rect(r->x + S(20), r->y + r->h - 1, r->w - S(40), 1, color_alpha(t->sep, 0.7f), 0);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = gx_max_scroll();
    if (ms > 0 && GX.sb_t > 0) {
        f32 vh = gx_view_h(), bar = MAX(S(32), vh * vh / GX.content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(GX.scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, GX.sb_t), S(1.5f));
    }
}

static bool gx_animate(f32 dt)
{
    bool anim = false;
    for (int i = 0; i < GX__COUNT; i++) anim |= spring(&GX.tog[i], &GX.togv[i], GX.on[i] ? 1.f : 0.f, 0.34f, 0.7f, 0.002f, dt);
    GX.scroll_target = CLAMP(GX.scroll_target, 0.f, gx_max_scroll());
    anim |= spring_step(&GX.scroll, &GX.scroll_v, GX.scroll_target, 24.f, dt);
    bool sb = A.page == PG_GFX && gx_max_scroll() > 0 && A.mx >= content_x0();
    anim |= approach(&GX.sb_t, sb ? 1.f : 0.f, sb ? 20.f : 7.f, dt);
    return anim;
}

static Hit gx_hit(f32 x, f32 y)
{
    if (y < S(HW_TOP) - S(6)) return (Hit){ HT_NONE, -1 };
    for (int i = 0; i < GX__COUNT; i++)
        if (in_b(&GX.sw[i], x, y) && (i != GX_HAGS || GX.hags_sup) && (i != GX_VBS || !GX.vbs_lock)) return (Hit){ HT_GFXTOG, i };
    return (Hit){ HT_NONE, -1 };
}

static void gx_click(Hit h)
{
    if (h.type != HT_GFXTOG || h.idx < 0 || h.idx >= GX__COUNT) return;
    int i = h.idx;
    GX.on[i] = !GX.on[i];
    GX.pend++;
    Job j = { gx_job_run, gx_job_done, i, ++GX.seq[i], GX.on[i] };
    job_post(A.hwnd, &j);
}
