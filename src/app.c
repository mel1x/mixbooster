// The window: a sidebar with the pages (only GPU Affinity for now), the test controls, the best cores and the results
// table with a tab per run. Everything is drawn by render.c, laid out in DIPs and scaled by the window's DPI.

#ifdef MB_DEV_BUILD
#define APP_CLASS L"MixBoosterDevWindow"  // runs next to an installed copy
#else
#define APP_CLASS L"MixBoosterWindow"
#endif
#define TIMER_TICK 1
#define HOTKEY_STOP 1

typedef struct Theme {
    bool dark;
    u32 accent, on_accent, text, dim, faint, sep, danger, good, warn;
} Theme;

typedef struct Colors {
    u32 bg, panel, panel_border, card, card_border, strip, nav_sel, nav_sel_border, nav_hover, ctl, ctl_hover, ctl_border, shadow, knob, overlay, pop_border;
} Colors;

enum {
    HT_NONE,
    HT_WINMIN,
    HT_WINCLOSE,
    HT_NAV,
    HT_SLIDER,
    HT_START,
    HT_PICK,      // idx: 0 or 1
    HT_BIND,
    HT_RESET,
    HT_TAB,       // idx: 0 = AVG, 1.. = run
    HT_ROW,       // idx: the CPU
    HT_MOK,       // the dialog before the test
    HT_MCANCEL,
    HT_MPANEL,
    HT_MOUT,
    HT_DONATE,    // the sidebar button
    HT_DON,       // the donation panel; idx: a wallet or DON_*
};

// Hover animation slots.
enum { HV_MIN, HV_CLOSE, HV_NAV, HV_SLIDER, HV_START, HV_PICK0, HV_PICK1, HV_BIND, HV_RESET, HV_MOK, HV_MCANCEL, HV_DONATE, HV_TAB0, HV__COUNT = HV_TAB0 + 1 + BENCH_MAX_RUNS };

typedef struct Hit {
    int type, idx;
} Hit;

typedef struct Box {
    f32 x, y, w, h;
} Box;

static struct {
    HWND hwnd;
    int W, H;
    UINT dpi;
    f32 s;
    bool mica, active, dirty, animating, close_pending, ready;
    f64 last_frame, device_retry;
    Theme th;
    Colors c;

    CpuInfo cpu;
    GpuDev gpu[MAX_GPUS];
    int ngpu;

    int runs;                 // the slider, 1..8
    bool dragging;
    int tab;                  // 0 = AVG, n = run n
    int best[2], nbest, pick;
    int bound_cpu;            // -1 when not bound
    bool svc_on;
    WCHAR note[160];          // a line under the bind button after binding or resetting
    bool note_bad;

    Hit hover, press;
    int mx, my;
    f32 hv[HV__COUNT];
    f32 slider_v;             // the knob, animated
    f32 tab_x, tab_w, tab_v;  // the selected tab, sliding
    f32 pick_t;               // 0..1 between the two pick tiles
    f32 prog;                 // the progress bar, eased
    f32 page_t;
    bool modal;               // the dialog before the test
    f32 modal_t;
    Box m_panel, m_ok, m_cancel;
    f32 row_hov;
    int row_hov_idx;
    f32 scroll, scroll_target, scroll_v, sb_t;

    // layout, rebuilt every frame
    Box card, slider, start, pick_box[2], bind, reset, strip, body, rows;
    Box tabs[1 + BENCH_MAX_RUNS];
    int ntabs;
    f32 left_w, right_x, content_h;
} A;

static void start_or_stop(void);

static f32 S(f32 v) { return floorf(v * A.s + 0.5f); }
static f32 SC(f32 v) { return v * A.s; }

static bool in_b(const Box *b, f32 x, f32 y) { return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h; }

static void invalidate(void) { A.dirty = true; }

// --- theme ---------------------------------------------------------------------------------------------------------

static bool reg_dword_hkcu(const WCHAR *key, const WCHAR *name, DWORD *out)
{
    DWORD sz = sizeof *out;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, out, &sz) == ERROR_SUCCESS;
}

static void theme_update(void)
{
    DWORD light = 0;
    bool dark = !(reg_dword_hkcu(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", &light) && light);
    Theme *t = &A.th;
    t->dark = dark;
    u32 acc = RGBA(0, 120, 212, 255);
    BYTE pal[32];
    DWORD sz = sizeof pal, c;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette", RRF_RT_REG_BINARY,
                     NULL, pal, &sz) == ERROR_SUCCESS && sz >= 32) {
        int i = dark ? 1 : 4;
        acc = RGBA(pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2], 255);
    } else if (reg_dword_hkcu(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &c)) {
        acc = RGBA(c & 255, (c >> 8) & 255, (c >> 16) & 255, 255);
    }
    t->accent = acc;
    f32 lum = (0.299f * (f32)(acc & 255) + 0.587f * (f32)((acc >> 8) & 255) + 0.114f * (f32)((acc >> 16) & 255)) / 255.f;
    t->on_accent = lum > 0.6f ? RGBA(0, 0, 0, 230) : RGBA(255, 255, 255, 255);

    Colors *k = &A.c;
    if (dark) {
        t->text = RGBA(255, 255, 255, 236);
        t->dim = RGBA(255, 255, 255, 150);
        t->faint = RGBA(255, 255, 255, 92);
        t->sep = RGBA(255, 255, 255, 18);
        t->danger = RGBA(255, 107, 107, 255);
        t->good = RGBA(108, 203, 95, 255);
        t->warn = RGBA(252, 225, 0, 255);
        k->bg = RGBA(30, 30, 32, 255);
        k->panel = RGBA(255, 255, 255, 9);
        k->panel_border = RGBA(255, 255, 255, 15);
        k->card = RGBA(255, 255, 255, 12);
        k->card_border = RGBA(255, 255, 255, 8);
        k->strip = RGBA(0, 0, 0, 34);
        k->nav_sel = RGBA(255, 255, 255, 20);
        k->nav_sel_border = RGBA(255, 255, 255, 12);
        k->nav_hover = RGBA(255, 255, 255, 11);
        k->ctl = RGBA(255, 255, 255, 16);
        k->ctl_hover = RGBA(255, 255, 255, 26);
        k->ctl_border = RGBA(255, 255, 255, 14);
        k->shadow = RGBA(0, 0, 0, 120);
        k->overlay = RGBA(0, 0, 0, 120);
        k->pop_border = RGBA(255, 255, 255, 22);
        k->knob = RGBA(69, 69, 72, 255);
    } else {
        t->text = RGBA(0, 0, 0, 228);
        t->dim = RGBA(0, 0, 0, 140);
        t->faint = RGBA(0, 0, 0, 90);
        t->sep = RGBA(0, 0, 0, 16);
        t->danger = RGBA(196, 43, 28, 255);
        t->good = RGBA(15, 123, 15, 255);
        t->warn = RGBA(157, 93, 0, 255);
        k->bg = RGBA(243, 243, 245, 255);
        k->panel = RGBA(255, 255, 255, 120);
        k->panel_border = RGBA(0, 0, 0, 14);
        k->card = RGBA(255, 255, 255, 200);
        k->card_border = RGBA(0, 0, 0, 12);
        k->strip = RGBA(0, 0, 0, 10);
        k->nav_sel = RGBA(0, 0, 0, 13);
        k->nav_sel_border = RGBA(0, 0, 0, 8);
        k->nav_hover = RGBA(0, 0, 0, 8);
        k->ctl = RGBA(0, 0, 0, 10);
        k->ctl_hover = RGBA(0, 0, 0, 18);
        k->ctl_border = RGBA(0, 0, 0, 20);
        k->shadow = RGBA(0, 0, 0, 60);
        k->overlay = RGBA(0, 0, 0, 60);
        k->pop_border = RGBA(0, 0, 0, 22);
        k->knob = RGBA(255, 255, 255, 255);
    }
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    g_reduced_motion = !anim;
    if (A.hwnd) dwm_set(A.hwnd, DWA_DARK, dark);
}

// --- state ---------------------------------------------------------------------------------------------------------

static void refresh_binding(void)
{
    u64 m = 0;
    A.bound_cpu = reg_get_u64(L"GpuMask", &m) ? mask_cpu(m) : -1;
    A.svc_on = A.bound_cpu >= 0 && svc_running();
}

static void refresh_best(void)
{
    AcquireSRWLockShared(&B.lock);
    A.nbest = bench_best(A.best, 2);
    ReleaseSRWLockShared(&B.lock);
    if (A.pick >= A.nbest) A.pick = 0;
}

static bool has_results(void)
{
    for (int r = 0; r < B.runs; r++)
        for (int c = 0; c < B.ncpu; c++)
            if (B.r[r][c].ok) return true;
    return false;
}

// --- small drawing helpers -----------------------------------------------------------------------------------------

static f32 baseline_for(int font, f32 fs, f32 cy) { return floorf(cy + font_cap_height(font, fs) * 0.5f + 0.5f); }

static void text_right(int font, f32 fs, f32 right, f32 cy, const WCHAR *s, u32 col)
{
    text_draw(font, fs, floorf(right - text_width(font, fs, s, -1) + 0.5f), baseline_for(font, fs, cy), s, -1, col);
}

static void text_center(int font, f32 fs, f32 cx, f32 cy, const WCHAR *s, u32 col)
{
    text_draw(font, fs, floorf(cx - text_width(font, fs, s, -1) * 0.5f + 0.5f), baseline_for(font, fs, cy), s, -1, col);
}

static void fmt_duration(WCHAR *out, int cap, f64 sec)
{
    int m = (int)ceil(sec / 60.0);
    if (m < 1) wcopy(out, cap, TR("< 1 мин", "< 1 min"));
    else if (m < 60) wfmt(out, cap, TR("≈ %d мин", "≈ %d min"), m);
    else wfmt(out, cap, TR("≈ %d ч %02d мин", "≈ %d h %02d min"), m / 60, m % 60);
}

enum { BTN_ACCENT, BTN_NEUTRAL };

static void draw_button(const Box *b, const WCHAR *label, u32 glyph, int style, f32 hov, bool down, bool disabled)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    u32 bg, fg;
    if (disabled) {
        bg = c->ctl;
        fg = t->faint;
    } else if (style == BTN_ACCENT) {
        u32 lift = t->dark ? RGBA(0, 0, 0, 255) : RGBA(255, 255, 255, 255);
        bg = color_mix(t->accent, lift, down ? 0.2f : 0.1f * hov);
        fg = t->on_accent;
    } else {
        bg = color_mix(c->ctl, c->ctl_hover, down ? 1.f : hov);
        fg = t->text;
    }
    r_rect(b->x, b->y, b->w, b->h, bg, S(6));
    if (style == BTN_NEUTRAL || disabled) r_rect_ex(b->x, b->y, b->w, b->h, c->ctl_border, S(6), 1.f, 0);
    f32 fs = SC(13.5f), mid = b->y + b->h * 0.5f;
    f32 lw = text_width(FONT_TEXT, fs, label, -1), gw = glyph ? S(14) + S(8) : 0;
    f32 x = floorf(b->x + (b->w - lw - gw) * 0.5f + 0.5f);
    if (glyph) text_draw_icon(glyph, SC(13), x + S(7), mid, fg);
    text_draw(FONT_TEXT, fs, x + gw, baseline_for(FONT_TEXT, fs, mid), label, -1, fg);
}

// The logo, as in tools/make-icon.py: a chip with one core lit, on a dark tile like MixLauncher's and MixWidgets'.
// Art units 0..100 cover the inner 72% of the tile. The exe and the taskbar use the same chip without the tile.
static void draw_logo(f32 x, f32 y, f32 sz)
{
    x = floorf(x);
    y = floorf(y);
    f32 rad = floorf(sz * 0.225f + 0.5f), u = sz * 0.72f / 100.f, ox = x + sz * 0.14f, oy = y + sz * 0.14f;
    r_rect(x, y, sz, sz, RGBA(0x1C, 0x23, 0x1E, 255), rad);
    r_rect_ex(x, y, sz, sz, RGBA(255, 255, 255, 26), rad, 1.f, 0);
    u32 pin = RGBA(0x2C, 0x5E, 0x3A, 255);
    for (int i = 0; i < 3; i++) {
        f32 o = 32.f + 18.f * (f32)i;
        r_rect(ox + (o - 4) * u, oy + 2 * u, 8 * u, 12 * u, pin, 3 * u);
        r_rect(ox + (o - 4) * u, oy + 86 * u, 8 * u, 12 * u, pin, 3 * u);
        r_rect(ox + 2 * u, oy + (o - 4) * u, 12 * u, 8 * u, pin, 3 * u);
        r_rect(ox + 86 * u, oy + (o - 4) * u, 12 * u, 8 * u, pin, 3 * u);
    }
    r_rect(ox + 14 * u, oy + 14 * u, 72 * u, 72 * u, pin, 14 * u);
    static const f32 cells[4][2] = { { 26, 26 }, { 52, 26 }, { 26, 52 }, { 52, 52 } };
    // The four cores as one group, scaled up from the chip's center.
    const f32 cs = 1.06f;
    for (int i = 0; i < 4; i++)
        r_rect(ox + (50 + (cells[i][0] - 50) * cs) * u, oy + (50 + (cells[i][1] - 50) * cs) * u, 22 * cs * u, 22 * cs * u,
               i == 1 ? RGBA(0xA6, 0xF0, 0x8E, 255) : RGBA(0x46, 0x9C, 0x5C, 255), 6 * cs * u);
}

// --- layout --------------------------------------------------------------------------------------------------------

static f32 sb_x(void) { return S(10); }
static f32 sb_w(void) { return S(228); }
static f32 content_x0(void) { return sb_x() + sb_w() + S(26); }
static f32 content_x1(void) { return (f32)A.W - S(26); }

static void winbtn_box(int i, Box *b)
{
    b->w = S(46);
    b->h = S(34);
    b->x = (f32)A.W - b->w * (f32)(2 - i);
    b->y = 0;
}

static void donate_box(Box *b)
{
    b->x = sb_x() + S(8);
    b->w = sb_w() - S(16);
    b->h = S(36);
    b->y = (f32)A.H - sb_x() - S(8) - b->h;
}

static void nav_box(Box *b)
{
    b->x = sb_x() + S(8);
    b->y = sb_x() + S(78);
    b->w = sb_w() - S(16);
    b->h = S(36);
}

static const WCHAR *tab_label(int i, WCHAR *buf, int cap)
{
    if (!i) return L"AVG";
    wfmt(buf, cap, TR("Прогон %d", "Run %d"), i);
    return buf;
}

static void layout(void)
{
    f32 x0 = content_x0(), x1 = content_x1(), cw = x1 - x0;
    A.card = (Box){ x0, S(88), cw, S(158) };
    A.left_w = floorf(CLAMP(cw * 0.4f, S(290), S(420)));
    f32 lx = x0 + S(22);
    A.slider = (Box){ lx + S(2), A.card.y + S(60), A.left_w - S(4), S(1) };
    A.start = (Box){ lx, A.card.y + S(106), S(176), S(34) };
    A.right_x = lx + A.left_w + S(44);
    f32 rw = x1 - S(22) - A.right_x, tw = floorf((rw - S(10)) * 0.5f);
    for (int i = 0; i < 2; i++) A.pick_box[i] = (Box){ A.right_x + (tw + S(10)) * (f32)i, A.card.y + S(44), tw, S(54) };
    A.bind = (Box){ A.right_x, A.card.y + S(106), S(214), S(34) };
    A.reset = (Box){ A.bind.x + A.bind.w + S(8), A.bind.y, S(112), S(34) };

    f32 ty = A.card.y + A.card.h + S(18);
    A.strip = (Box){ x0, ty, cw, S(44) };
    A.body = (Box){ x0, ty + A.strip.h, cw, MAX(S(60), (f32)A.H - S(16) - ty - A.strip.h) };
    A.ntabs = B.runs > 0 ? 1 + B.runs : 0;
    f32 x = x0 + S(8), fs = SC(13);
    for (int i = 0; i < A.ntabs; i++) {
        WCHAR buf[32];
        f32 w = MAX(S(84), floorf(text_width(FONT_TEXT_SEMIBOLD, fs, tab_label(i, buf, countof(buf)), -1) + (f32)S(i ? 40.f : 52.f)));
        A.tabs[i] = (Box){ x, ty + S(8), w, A.strip.h - S(8) };
        x += w + S(2);
    }
    f32 hh = S(40);
    A.rows = (Box){ x0, A.body.y + hh, cw, A.body.h - hh - S(6) };
    A.content_h = (f32)B.ncpu * S(38);
}

static f32 max_scroll(void) { return MAX(0.f, A.content_h - A.rows.h); }

// --- drawing -------------------------------------------------------------------------------------------------------

static void draw_sidebar(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    f32 x = sb_x(), y = sb_x(), w = sb_w(), h = (f32)A.H - sb_x() * 2;
    r_rect(x, y, w, h, c->panel, S(12));
    r_rect_ex(x, y, w, h, c->panel_border, S(12), 1.f, 0);

    f32 tile = S(36), tx = x + S(16), ty = y + S(18);
    draw_logo(tx, ty, tile);
    f32 nx = tx + tile + S(12);
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(15), nx, floorf(ty + S(16)), x + w - nx - S(10), L"MixBooster", -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(12.5f), nx, floorf(ty + S(33)), x + w - nx - S(10), TR("Оптимизация ПК", "PC tuning"), -1, t->dim, false);

    Box n;
    nav_box(&n);
    r_rect(n.x, n.y, n.w, n.h, c->nav_sel, S(8));
    r_rect_ex(n.x, n.y, n.w, n.h, c->nav_sel_border, S(8), 1.f, 0);
    r_rect(n.x + S(4), floorf(n.y + (n.h - S(16)) * 0.5f), S(3), S(16), t->accent, S(1.5f));
    f32 cy = n.y + n.h * 0.5f, ix = n.x + S(10), isz = S(20), fs = SC(14);
    text_draw_icon(0xE950, SC(15), ix + isz * 0.5f, cy, t->text);
    text_draw_fit(FONT_TEXT_SEMIBOLD, fs, ix + isz + S(12), baseline_for(FONT_TEXT, fs, cy), n.x + n.w - ix - isz - S(18), L"GPU Affinity", -1,
                  t->text, false);

    Box d;
    donate_box(&d);
    f32 hv = A.hv[HV_DONATE], dcy = d.y + d.h * 0.5f;
    if (hv > 0) r_rect(d.x, d.y, d.w, d.h, color_alpha(c->nav_hover, hv), S(8));
    text_draw_icon(0xEB51, SC(15), d.x + S(10) + isz * 0.5f, dcy, color_mix(t->faint, t->dim, hv));
    text_draw_fit(FONT_TEXT, fs, d.x + S(10) + isz + S(12), baseline_for(FONT_TEXT, fs, dcy), d.w - isz - S(32), TR("Поддержать автора", "Support the author"),
                  -1, color_mix(t->dim, t->text, hv), false);
    text_draw(FONT_TEXT, SC(12), x + S(18), floorf(d.y - S(12)), TR("Версия " MB_VER_STR, "Version " MB_VER_STR), -1, t->faint);
}

static void draw_slider(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *b = &A.slider;
    bool off = B.running;
    f32 cy = b->y, fr = (A.slider_v - 1.f) / (BENCH_MAX_RUNS - 1), kx = floorf(b->x + b->w * fr + 0.5f);
    f32 th = S(4);
    r_rect(b->x, cy - th * 0.5f, b->w, th, t->dark ? RGBA(255, 255, 255, 60) : RGBA(0, 0, 0, 70), th * 0.5f);
    r_rect(b->x, cy - th * 0.5f, kx - b->x, th, off ? t->faint : t->accent, th * 0.5f);
    for (int i = 1; i <= BENCH_MAX_RUNS; i++) {
        f32 tx = floorf(b->x + b->w * (f32)(i - 1) / (BENCH_MAX_RUNS - 1) + 0.5f);
        WCHAR num[4];
        wfmt(num, countof(num), L"%d", i);
        bool cur = i == A.runs;
        text_center(cur ? FONT_TEXT_SEMIBOLD : FONT_TEXT, SC(11.5f), tx, cy + S(20), num, cur ? t->text : t->faint);
    }
    // The Windows 11 knob: a ring with an accent dot that grows under the mouse and shrinks while held.
    f32 hv = A.hv[HV_SLIDER], k = S(20);
    r_rect_ex(kx - k * 0.5f, cy - k * 0.5f + S(1), k, k, RGBA(0, 0, 0, t->dark ? 90 : 40), k * 0.5f, 0, S(2));
    r_rect(kx - k * 0.5f, cy - k * 0.5f, k, k, c->knob, k * 0.5f);
    r_rect_ex(kx - k * 0.5f, cy - k * 0.5f, k, k, c->ctl_border, k * 0.5f, 1.f, 0);
    f32 d = A.dragging ? S(10) : S(12) + S(2) * hv;
    if (off) d = S(10);
    r_rect(kx - d * 0.5f, cy - d * 0.5f, d, d, off ? t->faint : t->accent, d * 0.5f);
}

static void draw_controls(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *cd = &A.card;
    r_rect(cd->x, cd->y, cd->w, cd->h, c->card, S(10));
    r_rect_ex(cd->x, cd->y, cd->w, cd->h, c->card_border, S(10), 1.f, 0);
    f32 lx = cd->x + S(22), fs = SC(14), top = cd->y + S(30);

    // left: the number of runs and the start button
    text_draw(FONT_TEXT, fs, lx, baseline_for(FONT_TEXT, fs, top), TR("Количество прогонов", "Number of runs"), -1, t->text);
    WCHAR v[32];
    wfmt(v, countof(v), L"%d", A.runs);
    text_right(FONT_TEXT_SEMIBOLD, fs, lx + A.left_w, top, v, t->text);
    draw_slider();
    bool run = B.running != 0, stopping = run && B.cancel;
    Hit *h = &A.hover;
    draw_button(&A.start, run ? (stopping ? TR("Останавливаю…", "Stopping…") : TR("Остановить · F4", "Stop · F4")) : TR("Начать тест", "Start test"),
                run ? 0xE71A : 0xE768, run ? BTN_NEUTRAL : BTN_ACCENT, A.hv[HV_START], A.press.type == HT_START && h->type == HT_START,
                stopping || g_task_busy);
    if (!run) {
        f64 step = BENCH_SETTLE_SEC + BENCH_WARMUP_SEC + BENCH_MEASURE_SEC;
        WCHAR d[48], s2[96];
        fmt_duration(d, countof(d), step * A.cpu.count * A.runs);
        wfmt(s2, countof(s2), TR("%s · %d потоков", "%s · %d threads"), d, A.cpu.count);
        f32 ex = A.start.x + A.start.w + S(14);
        text_draw_fit(FONT_TEXT, SC(12.5f), ex, baseline_for(FONT_TEXT, SC(12.5f), A.start.y + A.start.h * 0.5f), lx + A.left_w - ex, s2, -1, t->dim,
                      false);
    }

    r_rect(A.right_x - S(22), cd->y + S(18), 1, cd->h - S(36), t->sep, 0);
    f32 rx = A.right_x, rr = cd->x + cd->w - S(22);

    if (run) {
        // right while testing: where it is and how long is left
        WCHAR line[128], ph[96];
        AcquireSRWLockShared(&B.lock);
        int phase = B.phase, r = B.run, cpu = B.cpu;
        f64 el = time_now() - B.phase_t0, len = B.phase_len;
        ReleaseSRWLockShared(&B.lock);
        wfmt(line, countof(line), TR("Прогон %d из %d · CPU %d (%d из %d)", "Run %d of %d · CPU %d (%d of %d)"), r + 1, B.runs, cpu, cpu + 1, B.ncpu);
        if (phase == PH_PREPARE) wcopy(line, countof(line), TR("Подготовка…", "Preparing…"));
        if (phase == PH_RESTORE) wcopy(line, countof(line), TR("Возвращаю настройки видеокарты…", "Restoring the GPU settings…"));
        text_draw_fit(FONT_TEXT_SEMIBOLD, fs, rx, baseline_for(FONT_TEXT, fs, top), rr - rx, line, -1, t->text, false);
        int left = (int)MAX(0.0, ceil(len - el));
        switch (phase) {
        case PH_RESTART: wcopy(ph, countof(ph), TR("Перезапуск драйвера видеокарты", "Restarting the GPU driver")); break;
        case PH_WARMUP: wfmt(ph, countof(ph), TR("Прогрев · %d с", "Warm-up · %d s"), left); break;
        case PH_MEASURE: wfmt(ph, countof(ph), TR("Замер кадров · %d с", "Recording frames · %d s"), left); break;
        default: ph[0] = 0;
        }
        text_draw_fit(FONT_TEXT, SC(12.5f), rx, baseline_for(FONT_TEXT, SC(12.5f), cd->y + S(54)), rr - rx, ph, -1, t->dim, false);
        f32 py = cd->y + S(76), pw = rr - rx;
        r_rect(rx, py, pw, S(6), c->ctl, S(3));
        r_rect(rx, py, MAX(S(6), floorf(pw * A.prog + 0.5f)), S(6), t->accent, S(3));
        WCHAR eta[48], tail[128];
        fmt_duration(eta, countof(eta), bench_eta());
        wfmt(tail, countof(tail), TR("Осталось %s", "%s left"), eta);
        f32 my = A.bind.y + A.bind.h * 0.5f;
        text_draw(FONT_TEXT, SC(12.5f), rx, baseline_for(FONT_TEXT, SC(12.5f), my - S(9)), tail, -1, t->text);
        text_draw_fit(FONT_TEXT, SC(12), rx, baseline_for(FONT_TEXT, SC(12), my + S(10)), rr - rx,
                      TR("Экран будет мигать — не трогайте ПК. F4 остановит тест", "The screen will flicker — leave the PC alone. F4 stops the test"),
                      -1, t->faint, false);
        return;
    }

    // right: the two best cores and the bind button
    text_draw(FONT_TEXT, fs, rx, baseline_for(FONT_TEXT, fs, top), TR("Лучшие ядра", "Best cores"), -1, t->text);
    if (A.bound_cpu >= 0) {
        WCHAR s2[96];
        wfmt(s2, countof(s2), A.svc_on ? TR("Привязано к CPU %d", "Bound to CPU %d") : TR("CPU %d · служба не запущена", "CPU %d · service stopped"),
             A.bound_cpu);
        f32 sfs = SC(12.5f), sw = text_width(FONT_TEXT, sfs, s2, -1), dot = S(8);
        text_right(FONT_TEXT, sfs, rr, top, s2, t->dim);
        r_rect(rr - sw - S(8) - dot, floorf(top - dot * 0.5f), dot, dot, A.svc_on ? t->good : t->warn, dot * 0.5f);
    }
    if (B.error[0]) {
        text_draw_fit(FONT_TEXT, SC(12.5f), rx, baseline_for(FONT_TEXT, SC(12.5f), cd->y + S(56)), rr - rx, B.error, -1, t->danger, false);
    }
    if (!A.nbest) {
        if (!B.error[0])
            text_draw_fit(FONT_TEXT, SC(12.5f), rx, baseline_for(FONT_TEXT, SC(12.5f), cd->y + S(56)), rr - rx,
                          TR("После теста здесь появятся два лучших ядра для видеокарты", "The two best cores for the GPU show up here after a test"),
                          -1, t->dim, false);
    } else {
        CpuResult avg;
        for (int i = 0; i < A.nbest; i++) {
            Box *b = &A.pick_box[i];
            f32 sel = i == 0 ? 1.f - A.pick_t : A.pick_t, hv = A.hv[HV_PICK0 + i];
            r_rect(b->x, b->y, b->w, b->h, color_mix(color_mix(c->ctl, c->ctl_hover, hv), color_alpha(t->accent, 0.16f), sel), S(8));
            r_rect_ex(b->x, b->y, b->w, b->h, color_mix(c->ctl_border, t->accent, sel), S(8), 1.f + sel * 0.5f, 0);
            f32 rcx = b->x + S(22), rcy = b->y + b->h * 0.5f, ro = S(18);
            r_rect_ex(rcx - ro * 0.5f, rcy - ro * 0.5f, ro, ro, color_mix(t->dim, t->accent, sel), ro * 0.5f, S(1.5f), 0);
            f32 ri = S(8) * sel;
            if (ri > 0.5f) r_rect(rcx - ri * 0.5f, rcy - ri * 0.5f, ri, ri, t->accent, ri * 0.5f);
            WCHAR nm[24], st[96];
            int cpu = A.best[i];
            wfmt(nm, countof(nm), L"CPU %d", cpu);
            f32 tx = b->x + S(42), tmax = b->x + b->w - tx - S(10);
            text_draw(FONT_TEXT_SEMIBOLD, SC(14.5f), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(14.5f), b->y + S(19)), nm, -1, t->text);
            if (i == 0) {
                f32 nw = text_width(FONT_TEXT_SEMIBOLD, SC(14.5f), nm, -1), bx = tx + nw + S(8), bfs = SC(11);
                const WCHAR *lbl = TR("лучшее", "best");
                f32 bw = text_width(FONT_TEXT_SEMIBOLD, bfs, lbl, -1) + S(12);
                if (bx + bw < b->x + b->w - S(8)) {
                    r_rect(bx, b->y + S(10), bw, S(18), color_alpha(t->accent, 0.22f), S(9));
                    text_center(FONT_TEXT_SEMIBOLD, bfs, bx + bw * 0.5f, b->y + S(19), lbl, t->text);
                }
            }
            AcquireSRWLockShared(&B.lock);
            bool ok = bench_avg(cpu, &avg);
            ReleaseSRWLockShared(&B.lock);
            if (ok) {
                wfmt(st, countof(st), TR("1%% low %.0f · avg %.0f FPS", "1%% low %.0f · avg %.0f FPS"), avg.v[M_LOW1], avg.v[M_AVG]);
                text_draw_fit(FONT_TEXT, SC(12), tx, baseline_for(FONT_TEXT, SC(12), b->y + S(37)), tmax, st, -1, t->dim, false);
            }
        }
        int cpu = A.best[A.pick];
        bool same = A.bound_cpu == cpu && A.svc_on, busy = g_task_busy != 0;
        WCHAR lbl[64];
        if (busy) wcopy(lbl, countof(lbl), TR("Применяю…", "Applying…"));
        else if (same) wfmt(lbl, countof(lbl), TR("Привязано к CPU %d", "Bound to CPU %d"), cpu);
        else wcopy(lbl, countof(lbl), TR("Привязать видеокарту", "Bind the GPU"));
        draw_button(&A.bind, lbl, same ? 0xE73E : 0xE71B, BTN_ACCENT, A.hv[HV_BIND], A.press.type == HT_BIND && h->type == HT_BIND, busy || same);
    }
    f32 nx = A.bind.x;
    if (A.bound_cpu >= 0) {
        draw_button(&A.reset, TR("Сбросить", "Reset"), 0xE777, BTN_NEUTRAL, A.hv[HV_RESET], A.press.type == HT_RESET && h->type == HT_RESET,
                    g_task_busy != 0);
        nx = A.reset.x + A.reset.w;
    } else if (A.nbest) {
        nx = A.bind.x + A.bind.w;
    }
    if (A.note[0]) {
        f32 my = A.bind.y + A.bind.h * 0.5f, x = nx + (nx > A.bind.x ? S(14) : 0);
        text_draw_fit(FONT_TEXT, SC(12), x, baseline_for(FONT_TEXT, SC(12), my), rr - x, A.note, -1, A.note_bad ? t->danger : t->dim, false);
    }
}

// Columns of the table: the CPU, then the metrics in AutoGpuAffinity's order of importance.
static const int k_cols[] = { M_AVG, M_MAX, M_MIN, M_LOW1, M_LOW01, M_LOW001, M_P1, M_P01, M_STDEV };

static const WCHAR *col_name(int m)
{
    switch (m) {
    case M_AVG: return TR("Средний", "Average");
    case M_MAX: return TR("Макс", "Max");
    case M_MIN: return TR("Мин", "Min");
    case M_LOW1: return L"1% Low";
    case M_LOW01: return L"0.1% Low";
    case M_LOW001: return L"0.01% Low";
    case M_P1: return L"1% Pct";
    case M_P01: return L"0.1% Pct";
    case M_STDEV: return TR("STDEV, мс", "STDEV, ms");
    }
    return L"";
}

static void draw_tabs(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *s = &A.strip, *b = &A.body;
    f32 rad = S(10);
    // The strip and the body fill different rows, so their translucent fills never overlap.
    r_set_clip(s->x, s->y, s->x + s->w, s->y + s->h);
    r_rect(s->x, s->y, s->w, s->h + rad * 2, c->strip, rad);
    if (A.ntabs) r_rect(floorf(A.tab_x + 0.5f), A.tabs[0].y, floorf(A.tab_w + 0.5f), A.tabs[0].h + S(12), c->card, S(8));
    r_set_clip(b->x, b->y, b->x + b->w, b->y + b->h);
    r_rect(b->x, b->y - rad * 2, b->w, b->h + rad * 2, c->card, rad);
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    r_rect_ex(s->x, s->y, s->w, s->h + b->h, c->card_border, rad, 1.f, 0);

    bool run = B.running != 0;
    for (int i = 0; i < A.ntabs; i++) {
        Box *tb = &A.tabs[i];
        bool sel = i == A.tab;
        f32 hv = A.hv[HV_TAB0 + i];
        if (!sel && hv > 0) r_rect(tb->x + S(2), tb->y + S(3), tb->w - S(4), tb->h - S(9), color_alpha(c->nav_hover, hv), S(6));
        // Chrome's separators between the tabs that are not selected
        if (i > 0 && !sel && A.tab != i - 1) r_rect(tb->x - S(1), tb->y + S(11), 1, tb->h - S(22), t->sep, 0);
        WCHAR buf[32];
        const WCHAR *lbl = tab_label(i, buf, countof(buf));
        f32 fs = SC(13), cy = tb->y + (tb->h - S(3)) * 0.5f, x = tb->x + S(16);
        u32 col = sel ? t->text : color_mix(t->dim, t->text, hv);
        if (i == 0) {
            text_draw_icon(0xE9D2, SC(13), x + S(6), cy, sel ? t->accent : col);
            x += S(20);
        }
        text_draw(sel ? FONT_TEXT_SEMIBOLD : FONT_TEXT, fs, x, baseline_for(FONT_TEXT, fs, cy), lbl, -1, col);
        // A still dot, not a pulsing one: nothing animates while the test runs, so the GPU only draws the demo.
        if (run && i == B.run + 1) {
            f32 d = S(6);
            r_rect(tb->x + tb->w - S(14) - d * 0.5f, floorf(cy - d * 0.5f), d, d, t->accent, d * 0.5f);
        }
    }
}

static void fmt_metric(WCHAR *out, int cap, int m, f32 v)
{
    if (m == M_STDEV) wfmt(out, cap, L"%.3f", v);
    else wfmt(out, cap, L"%.2f", v);
}

static void draw_empty(void)
{
    const Theme *t = &A.th;
    Box *b = &A.body;
    f32 cx = b->x + b->w * 0.5f, cy = A.strip.y + (A.strip.h + b->h) * 0.5f;
    text_draw_icon(0xE9D9, SC(34), cx, cy - S(34), t->faint);
    text_center(FONT_TEXT_SEMIBOLD, SC(15), cx, cy + S(10), TR("Результатов пока нет", "No results yet"), t->text);
    text_center(FONT_TEXT, SC(13), cx, cy + S(34),
                TR("Выберите количество прогонов и нажмите «Начать тест»", "Pick the number of runs and press Start test"), t->dim);
}

static void draw_table(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    if (!A.ntabs) {
        r_rect(A.strip.x, A.strip.y, A.strip.w, A.strip.h + A.body.h, c->card, S(10));
        r_rect_ex(A.strip.x, A.strip.y, A.strip.w, A.strip.h + A.body.h, c->card_border, S(10), 1.f, 0);
        draw_empty();
        return;
    }
    draw_tabs();
    Box *b = &A.body;
    int ncol = countof(k_cols);
    f32 cpu_w = S(150), pad = S(20), colw = (b->w - cpu_w - pad * 2) / (f32)ncol;
    f32 hy = b->y + S(20), hfs = SC(12.5f);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, b->x + pad, baseline_for(FONT_TEXT, hfs, hy), L"CPU", -1, t->dim);
    for (int k = 0; k < ncol; k++)
        text_draw_fit(FONT_TEXT_SEMIBOLD, hfs,
                      MAX(b->x + pad + cpu_w + colw * (f32)k, b->x + pad + cpu_w + colw * (f32)(k + 1) - text_width(FONT_TEXT_SEMIBOLD, hfs, col_name(k_cols[k]), -1)),
                      baseline_for(FONT_TEXT, hfs, hy), colw - S(6), col_name(k_cols[k]), -1, t->dim, false);
    r_rect(b->x + S(12), b->y + S(39), b->w - S(24), 1, t->sep, 0);

    AcquireSRWLockShared(&B.lock);
    CpuResult rows[MAX_CPUS];
    int run = A.tab - 1;
    for (int i = 0; i < B.ncpu; i++) {
        if (run < 0) bench_avg(i, &rows[i]);
        else rows[i] = B.r[run][i];
    }
    bool live = B.running && run == B.run;
    int live_cpu = B.cpu;
    ReleaseSRWLockShared(&B.lock);

    // the best value of each column
    f32 top[M__COUNT];
    bool any = false;
    for (int m = 0; m < M__COUNT; m++) top[m] = m == M_STDEV ? 1e9f : -1.f;
    for (int i = 0; i < B.ncpu; i++) {
        if (!rows[i].ok) continue;
        any = true;
        for (int m = 0; m < M__COUNT; m++) top[m] = m == M_STDEV ? MIN(top[m], rows[i].v[m]) : MAX(top[m], rows[i].v[m]);
    }

    f32 off = A.rows.y - floorf(A.scroll + 0.5f), rh = S(38), fs = SC(13);
    r_set_clip(b->x, A.rows.y, b->x + b->w, A.rows.y + A.rows.h);
    for (int i = 0; i < B.ncpu; i++) {
        f32 y = off + rh * (f32)i, cy = y + rh * 0.5f;
        if (y + rh < A.rows.y || y > A.rows.y + A.rows.h) continue;
        if (A.row_hov_idx == i && A.row_hov > 0) r_rect(b->x + S(6), y + S(2), b->w - S(12), rh - S(4), color_alpha(c->nav_hover, A.row_hov), S(6));
        bool cur = live && live_cpu == i;
        if (cur) {
            r_rect(b->x + S(6), y + S(2), b->w - S(12), rh - S(4), color_alpha(t->accent, 0.10f), S(6));
            r_rect(b->x + S(8), floorf(cy - S(8)), S(3), S(16), t->accent, S(1.5f));
        }
        WCHAR nm[16];
        wfmt(nm, countof(nm), L"CPU %d", i);
        f32 x = b->x + pad;
        x = text_draw(FONT_TEXT_SEMIBOLD, fs, x, baseline_for(FONT_TEXT, fs, cy), nm, -1, t->text) + S(8);
        // badges: the rank among the best cores, an efficiency core, the bound core
        for (int k = 0; k < A.nbest && run < 0; k++)
            if (A.best[k] == i) {
                WCHAR rk[4];
                wfmt(rk, countof(rk), L"%d", k + 1);
                f32 d = S(18);
                r_rect(x, floorf(cy - d * 0.5f), d, d, k == 0 ? t->accent : color_alpha(t->accent, 0.25f), d * 0.5f);
                text_center(FONT_TEXT_SEMIBOLD, SC(11), x + d * 0.5f, cy, rk, k == 0 ? t->on_accent : t->text);
                x += d + S(6);
            }
        if (A.cpu.hybrid && A.cpu.eff[i] == 0) {
            f32 bw = S(20), bh = S(18);
            r_rect(x, floorf(cy - bh * 0.5f), bw, bh, c->ctl, S(5));
            text_center(FONT_TEXT_SEMIBOLD, SC(10.5f), x + bw * 0.5f, cy, L"E", t->dim);
            x += bw + S(6);
        }
        if (A.bound_cpu == i) text_draw_icon(0xE71B, SC(13), x + S(7), cy, A.svc_on ? t->good : t->warn);

        for (int k = 0; k < ncol; k++) {
            int m = k_cols[k];
            f32 rx = b->x + pad + cpu_w + colw * (f32)(k + 1);
            WCHAR v[32];
            if (!rows[i].ok) {
                text_right(FONT_TEXT, fs, rx, cy, cur ? L"…" : L"—", t->faint);
                continue;
            }
            fmt_metric(v, countof(v), m, rows[i].v[m]);
            bool best = any && rows[i].v[m] == top[m];
            if (best) {
                f32 vw = text_width(FONT_TEXT_SEMIBOLD, fs, v, -1);
                r_rect(rx - vw - S(8), floorf(cy - S(11)), vw + S(16), S(22), color_alpha(t->good, 0.14f), S(6));
            }
            text_right(best ? FONT_TEXT_SEMIBOLD : FONT_TEXT, fs, rx, cy, v, best ? t->good : t->text);
        }
        if (i + 1 < B.ncpu && !(A.row_hov_idx == i && A.row_hov > 0.5f)) r_rect(b->x + S(20), y + rh - 1, b->w - S(40), 1, color_alpha(t->sep, 0.6f), 0);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);

    f32 ms = max_scroll();
    if (ms > 0 && A.sb_t > 0) {
        f32 vh = A.rows.h, bar = MAX(S(32), vh * vh / A.content_h), by = A.rows.y + (vh - bar) * CLAMP(A.scroll / ms, 0.f, 1.f);
        r_rect(b->x + b->w - S(7), floorf(by), S(3), floorf(bar), color_alpha(t->faint, A.sb_t), S(1.5f));
    }
}

// Greedy word wrap. Draws when draw is set; returns the number of lines either way.
static int text_wrap(int font, f32 fs, f32 x, f32 y, f32 max_w, f32 line_h, const WCHAR *s, u32 col, bool draw)
{
    int n = wlen(s), lines = 0;
    for (int a = 0; a < n; lines++) {
        int end = a, fit = a;
        while (end < n) {
            int e = end;
            while (e < n && s[e] != ' ') e++;
            if (fit > a && text_width(font, fs, s + a, e - a) > max_w) break;
            fit = e;
            end = e + 1;
        }
        if (fit == a) fit = MIN(n, a + 1);
        if (draw) text_draw(font, fs, x, floorf(y + line_h * (f32)lines + 0.5f), s + a, fit - a, col);
        a = fit;
        while (a < n && s[a] == ' ') a++;
    }
    return lines;
}

typedef struct ModalItem {
    u32 glyph;          // 0: the F4 keycap
    const WCHAR *title, *text;
} ModalItem;

static int modal_items(ModalItem *it, WCHAR *eta, int cap)
{
    f64 step = BENCH_SETTLE_SEC + BENCH_WARMUP_SEC + BENCH_MEASURE_SEC;
    WCHAR d[48];
    fmt_duration(d, countof(d), step * A.cpu.count * A.runs);
    wfmt(eta, cap,
         TR("Тест займёт %s. Экран будет гаснуть и уходить в полноэкранный режим — это перезапуск драйвера видеокарты на каждом ядре.",
            "The test takes %s. The screen goes black and fullscreen now and then: the GPU driver restarts for every core."),
         d);
    it[0] = (ModalItem){ 0xE7BA, TR("Не трогайте ПК во время теста", "Leave the PC alone during the test"), eta };
    it[1] = (ModalItem){ 0xE9D9, TR("Закройте всё, что нагружает ПК", "Close everything that loads the PC"),
                         TR("Игры, браузер, лаунчеры, оверлеи, запись экрана, торренты — любая фоновая нагрузка на процессор или видеокарту исказит результаты.",
                            "Games, the browser, launchers, overlays, screen recording, torrents: any background load on the CPU or GPU skews the results.") };
    it[2] = (ModalItem){ 0, TR("Нужен ПК — нажмите F4", "Need the PC? Press F4"),
                         TR("Тест остановится в любой момент, даже поверх полноэкранного окна, а видеокарта вернётся к прежней привязке.",
                            "The test stops at any moment, even over the fullscreen window, and the GPU goes back to its previous binding.") };
    return 3;
}

// Lays the dialog out; with draw set it also draws it.
static void modal_pass(bool draw)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    ModalItem it[3];
    WCHAR eta[256];
    int n = modal_items(it, eta, countof(eta));
    f32 w = MIN(S(520), (f32)A.W - S(40)), pad = S(28), tx = pad + S(48), tw = w - tx - pad;
    f32 tfs = SC(14), dfs = SC(13), lh = S(19);
    f32 h = pad + S(40) + S(20);
    f32 item_h[3];
    for (int i = 0; i < n; i++) {
        int lines = text_wrap(FONT_TEXT, dfs, 0, 0, tw, lh, it[i].text, 0, false);
        item_h[i] = S(22) + lh * (f32)lines;
        h += item_h[i] + S(18);
    }
    h += S(12) + S(34) + pad;
    f32 k = ease_out_cubic(A.modal_t);
    f32 x = floorf(((f32)A.W - w) * 0.5f), y = floorf(((f32)A.H - h) * 0.5f + (1.f - k) * S(12));
    A.m_panel = (Box){ x, y, w, h };
    A.m_ok = (Box){ x + w - pad - S(150), y + h - pad - S(34), S(150), S(34) };
    A.m_cancel = (Box){ A.m_ok.x - S(8) - S(110), A.m_ok.y, S(110), S(34) };
    if (!draw) return;

    R.opacity = k;
    r_rect(0, 0, (f32)A.W, (f32)A.H, t->dark ? RGBA(0, 0, 0, 120) : RGBA(0, 0, 0, 70), 0);
    r_rect_ex(x, y + S(8), w, h, c->shadow, S(12), 0, S(24));
    r_rect(x, y, w, h, t->dark ? RGBA(43, 43, 46, 255) : RGBA(251, 251, 252, 255), S(12));
    r_rect_ex(x, y, w, h, t->dark ? RGBA(255, 255, 255, 22) : RGBA(0, 0, 0, 22), S(12), 1.f, 0);

    f32 cy = y + pad + S(18), ic = S(36);
    r_rect(x + pad, cy - ic * 0.5f, ic, ic, color_alpha(t->accent, 0.18f), S(9));
    text_draw_icon(0xE768, SC(16), x + pad + ic * 0.5f, cy, t->text);
    text_draw(FONT_TEXT_SEMIBOLD, SC(19), x + pad + ic + S(14), baseline_for(FONT_TEXT_SEMIBOLD, SC(19), cy), TR("Перед началом теста", "Before the test"), -1,
              t->text);

    f32 iy = y + pad + S(40) + S(20);
    for (int i = 0; i < n; i++) {
        f32 gx = x + pad + S(18), gy = iy + S(10);
        if (it[i].glyph) {
            text_draw_icon(it[i].glyph, SC(18), gx, gy, t->dim);
        } else {
            f32 kw = S(34), kh = S(26);
            r_rect(gx - kw * 0.5f, gy - kh * 0.5f, kw, kh, c->ctl, S(6));
            r_rect_ex(gx - kw * 0.5f, gy - kh * 0.5f, kw, kh, c->ctl_border, S(6), 1.f, 0);
            r_rect(gx - kw * 0.5f + S(1), gy + kh * 0.5f - S(3), kw - S(2), S(2), c->ctl_border, S(1));
            text_center(FONT_TEXT_SEMIBOLD, SC(12), gx, gy - S(1), L"F4", t->text);
        }
        text_draw(FONT_TEXT_SEMIBOLD, tfs, x + tx, baseline_for(FONT_TEXT_SEMIBOLD, tfs, gy), it[i].title, -1, t->text);
        text_wrap(FONT_TEXT, dfs, x + tx, baseline_for(FONT_TEXT, dfs, gy + S(22)), tw, lh, it[i].text, t->dim, true);
        iy += item_h[i] + S(18);
    }
    r_rect(x + S(1), A.m_ok.y - S(18), w - S(2), 1, t->sep, 0);
    Hit *hv = &A.hover;
    draw_button(&A.m_cancel, TR("Отмена", "Cancel"), 0, BTN_NEUTRAL, A.hv[HV_MCANCEL], A.press.type == HT_MCANCEL && hv->type == HT_MCANCEL, false);
    draw_button(&A.m_ok, TR("Начать тест", "Start test"), 0xE768, BTN_ACCENT, A.hv[HV_MOK], A.press.type == HT_MOK && hv->type == HT_MOK, false);
    R.opacity = 1.f;
}

static void modal_open(void)
{
    A.modal = true;
    A.modal_t = 0;
    A.hover.type = HT_NONE;
    invalidate();
}

static void modal_close(bool start)
{
    A.modal = false;
    if (start) {
        // Gone at once: no fade-out drawn over the start of the test.
        A.modal_t = 0;
        start_or_stop();
    }
    invalidate();
}

#include "donate.c"

static void draw_all(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    if (!A.mica) r_rect(0, 0, (f32)A.W, (f32)A.H, c->bg, 0);
    draw_sidebar();

    f32 pe = ease_out_cubic(A.page_t), rise = floorf((1.f - pe) * S(10) + 0.5f);
    R.opacity = pe;
    f32 x0 = content_x0();
    text_draw(FONT_TEXT_SEMIBOLD, SC(20), x0 + S(2), S(42) + rise, L"GPU Affinity", -1, t->text);
    WCHAR sub[300];
    const WCHAR *gpu = A.ngpu ? A.gpu[0].name : TR("Видеокарта не найдена", "No GPU found");
    wfmt(sub, countof(sub), TR("%s · %s, %d потоков", "%s · %s, %d threads"), gpu, A.cpu.name[0] ? A.cpu.name : L"CPU", A.cpu.count);
    text_draw_fit(FONT_TEXT, SC(13), x0 + S(2), S(66) + rise, content_x1() - x0 - S(100), sub, -1, t->dim, false);
    draw_controls();
    draw_table();
    R.opacity = 1.f;
    if (A.modal || A.modal_t > 0) modal_pass(true);
    if (D.open) draw_donate();
    confetti_draw();

    for (int i = 0; i < 2; i++) {
        Box r;
        winbtn_box(i, &r);
        f32 h = A.hv[i ? HV_CLOSE : HV_MIN];
        bool down = A.press.type == A.hover.type && A.hover.type == (i ? HT_WINCLOSE : HT_WINMIN);
        u32 fg = A.active ? t->text : t->faint;
        if (i == 1) {
            r_rect(r.x, r.y, r.w, r.h, color_alpha(down ? RGBA(200, 64, 50, 255) : RGBA(196, 43, 28, 255), h), 0);
            fg = color_mix(fg, RGBA(255, 255, 255, 255), h);
        } else {
            r_rect(r.x, r.y, r.w, r.h, color_alpha(down ? c->ctl_hover : c->ctl, h), 0);
        }
        text_draw_icon(i ? 0xE8BB : 0xE921, SC(10), r.x + r.w * 0.5f, r.y + r.h * 0.5f, fg);
    }
}

// --- animation -----------------------------------------------------------------------------------------------------

static int row_at(f32 y)
{
    if (!in_b(&A.rows, (f32)A.mx, y)) return -1;
    int i = (int)floorf((y - A.rows.y + A.scroll) / S(38));
    return i >= 0 && i < B.ncpu ? i : -1;
}

static bool animate(f32 dt)
{
    bool anim = false;
    int hv_of[] = { [HT_WINMIN] = HV_MIN, [HT_WINCLOSE] = HV_CLOSE, [HT_NAV] = HV_NAV, [HT_SLIDER] = HV_SLIDER, [HT_START] = HV_START,
                    [HT_BIND] = HV_BIND, [HT_RESET] = HV_RESET };
    for (int i = 0; i < HV__COUNT; i++) {
        bool on = false;
        if (i >= HV_TAB0) on = A.hover.type == HT_TAB && A.hover.idx == i - HV_TAB0;
        else if (i == HV_PICK0 || i == HV_PICK1) on = A.hover.type == HT_PICK && A.hover.idx == i - HV_PICK0;
        else if (i == HV_MOK || i == HV_MCANCEL) on = A.hover.type == (i == HV_MOK ? HT_MOK : HT_MCANCEL);
        else if (i == HV_DONATE) on = A.hover.type == HT_DONATE;
        else if (A.hover.type < countof(hv_of) && A.hover.type != HT_NONE) on = hv_of[A.hover.type] == i && A.hover.type != HT_PICK;
        if (i == HV_SLIDER && A.dragging) on = true;
        anim |= approach(&A.hv[i], on ? 1.f : 0.f, 22.f, dt);
    }
    anim |= approach(&A.slider_v, (f32)A.runs, 26.f, dt);
    if (A.ntabs) {
        Box *tb = &A.tabs[CLAMP(A.tab, 0, A.ntabs - 1)];
        static f32 wv;
        anim |= spring_step(&A.tab_x, &A.tab_v, tb->x, 30.f, dt);
        anim |= spring_step(&A.tab_w, &wv, tb->w, 30.f, dt);
    }
    anim |= approach(&A.pick_t, (f32)A.pick, 20.f, dt);
    A.prog = bench_progress();
    if (A.page_t < 1.f) {
        A.page_t = g_reduced_motion ? 1.f : MIN(1.f, A.page_t + dt / 0.28f);
        anim = true;
    }
    int rh = row_at((f32)A.my);
    if (rh >= 0 && rh != A.row_hov_idx) {
        A.row_hov_idx = rh;
        A.row_hov = 0;
    }
    anim |= approach(&A.row_hov, rh >= 0 ? 1.f : 0.f, 22.f, dt);
    A.scroll_target = CLAMP(A.scroll_target, 0.f, max_scroll());
    anim |= spring_step(&A.scroll, &A.scroll_v, A.scroll_target, 24.f, dt);
    bool sb_on = max_scroll() > 0 && in_b(&A.body, (f32)A.mx, (f32)A.my);
    anim |= approach(&A.sb_t, sb_on ? 1.f : 0.f, sb_on ? 20.f : 7.f, dt);
    anim |= approach(&A.modal_t, A.modal ? 1.f : 0.f, A.modal ? 14.f : 18.f, dt);
    anim |= don_animate(dt);
    anim |= confetti_animate(dt);
    return anim;
}

static void render(void)
{
    if (!A.hwnd || !A.ready) return;
    if (!R.ok) {
        // A driver restart (the test does many) removes the D3D device: build it again.
        f64 now = time_now();
        if (now < A.device_retry) return;
        A.device_retry = now + 0.5;
        r_shutdown();
        if (!r_init(A.hwnd, A.W, A.H)) return;
    }
    r_wait_frame();
    f64 now = time_now();
    f32 dt = (f32)(now - A.last_frame);
    if (dt > 0.1f || dt <= 0) dt = 1.f / 60.f;
    A.last_frame = now;
    layout();
    A.animating = animate(dt);
    R.scale = 1.f;
    r_begin();
    draw_all();
    if (F.overflow) {
        font_reset_atlas();
        r_begin();
        draw_all();
        F.overflow = false;
    }
    r_end_and_present();
    A.dirty = false;
}

// --- input ---------------------------------------------------------------------------------------------------------

static Hit hit_test(int mx, int my)
{
    Hit h = { HT_NONE, -1 };
    f32 x = (f32)mx, y = (f32)my;
    for (int i = 0; i < 2; i++) {
        Box r;
        winbtn_box(i, &r);
        if (in_b(&r, x, y)) {
            h.type = i ? HT_WINCLOSE : HT_WINMIN;
            return h;
        }
    }
    if (don_active()) {
        h.type = HT_DON;
        h.idx = in_box(D.panel, x, y) ? DON_PANEL : DON_OUT;
        if (h.idx == DON_PANEL) {
            if (in_box(D.close, x, y)) h.idx = DON_CLOSE;
            else if (in_box(D.link, x, y)) h.idx = DON_LINK;
            else
                for (int i = 0; i < 8; i++)
                    if (in_box(D.copy[i], x, y)) h.idx = i;
        }
        return h;
    }
    if (A.modal) {
        modal_pass(false);
        h.type = in_b(&A.m_ok, x, y) ? HT_MOK : in_b(&A.m_cancel, x, y) ? HT_MCANCEL : in_b(&A.m_panel, x, y) ? HT_MPANEL : HT_MOUT;
        return h;
    }
    Box n;
    donate_box(&n);
    if (in_b(&n, x, y)) {
        h.type = HT_DONATE;
        return h;
    }
    nav_box(&n);
    if (in_b(&n, x, y)) {
        h.type = HT_NAV;
        return h;
    }
    Box sl = { A.slider.x - S(12), A.slider.y - S(14), A.slider.w + S(24), S(28) };
    if (!B.running && in_b(&sl, x, y)) h.type = HT_SLIDER;
    else if (in_b(&A.start, x, y)) h.type = HT_START;
    else if (!B.running && A.nbest && in_b(&A.bind, x, y)) h.type = HT_BIND;
    else if (!B.running && A.bound_cpu >= 0 && in_b(&A.reset, x, y)) h.type = HT_RESET;
    if (h.type) return h;
    for (int i = 0; !B.running && i < A.nbest; i++)
        if (in_b(&A.pick_box[i], x, y)) return (Hit){ HT_PICK, i };
    for (int i = 0; i < A.ntabs; i++)
        if (in_b(&A.tabs[i], x, y)) return (Hit){ HT_TAB, i };
    int r = row_at(y);
    if (r >= 0) return (Hit){ HT_ROW, r };
    return h;
}

static void slider_set(int mx)
{
    f32 fr = CLAMP(((f32)mx - A.slider.x) / A.slider.w, 0.f, 1.f);
    int v = (int)floorf(fr * (BENCH_MAX_RUNS - 1) + 0.5f) + 1;
    if (v != A.runs) {
        A.runs = v;
        invalidate();
    }
}

static void set_tab(int i)
{
    i = CLAMP(i, 0, MAX(0, A.ntabs - 1));
    if (i == A.tab) return;
    A.tab = i;
    invalidate();
}

static void start_or_stop(void)
{
    if (B.running) {
        InterlockedExchange(&B.cancel, 1);
    } else if (!g_task_busy) {
        A.note[0] = 0;
        bench_start(A.hwnd, A.runs, A.cpu.count);
        A.tab = 1;
        A.nbest = 0;
        A.scroll = A.scroll_target = 0;
        // Once a second for the countdown, and only while the window is in front (see app_loop).
        SetTimer(A.hwnd, TIMER_TICK, 1000, NULL);
        if (!RegisterHotKey(A.hwnd, HOTKEY_STOP, MOD_NOREPEAT, VK_F4)) log_msg("F4 is taken by another app: %lu", GetLastError());
    }
    invalidate();
}

static void mouse_down(int mx, int my)
{
    Hit h = hit_test(mx, my);
    A.press = h;
    if (D.open) {
        if (D.closing) return;
        if (h.idx == DON_OUT || h.idx == DON_CLOSE) {
            don_close();
        } else if (h.idx == DON_LINK) {
            open_url(DON_LINK_URL);
            confetti_burst();
        } else if (h.idx >= 0) {
            don_copy(h.idx);
            confetti_burst();
        }
        return;
    }
    switch (h.type) {
    case HT_DONATE: don_open(); break;
    case HT_SLIDER:
        A.dragging = true;
        slider_set(mx);
        break;
    case HT_PICK: A.pick = h.idx; break;
    case HT_TAB: set_tab(h.idx); break;
    }
    invalidate();
}

static void mouse_up(int mx, int my)
{
    Hit h = hit_test(mx, my);
    if (A.press.type == h.type && A.press.idx == h.idx) {
        switch (h.type) {
        case HT_WINCLOSE: PostMessageW(A.hwnd, WM_CLOSE, 0, 0); break;
        case HT_WINMIN: ShowWindow(A.hwnd, SW_MINIMIZE); break;
        case HT_START:
            if (B.running) {
                if (!B.cancel) start_or_stop();
            } else if (!g_task_busy) {
                modal_open();
            }
            break;
        case HT_MOK: modal_close(true); break;
        case HT_MCANCEL:
        case HT_MOUT: modal_close(false); break;
        case HT_BIND:
            if (!g_task_busy && A.nbest && !(A.bound_cpu == A.best[A.pick] && A.svc_on)) {
                A.note[0] = 0;
                task_start(A.hwnd, true, A.best[A.pick]);
            }
            break;
        case HT_RESET:
            if (!g_task_busy) {
                A.note[0] = 0;
                task_start(A.hwnd, false, 0);
            }
            break;
        }
    }
    A.dragging = false;
    A.press.type = HT_NONE;
    invalidate();
}

static void key_down(UINT vk)
{
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (D.open) {
        if (vk == VK_ESCAPE) don_close();
        else if (vk == VK_DOWN || vk == VK_NEXT) D.scroll_target += S(vk == VK_NEXT ? 240.f : 48.f);
        else if (vk == VK_UP || vk == VK_PRIOR) D.scroll_target -= S(vk == VK_PRIOR ? 240.f : 48.f);
        invalidate();
        return;
    }
    if (A.modal) {
        if (vk == VK_ESCAPE) modal_close(false);
        else if (vk == VK_RETURN) modal_close(true);
        return;
    }
    if (ctrl && vk == VK_TAB && A.ntabs) {
        set_tab((A.tab + (shift ? A.ntabs - 1 : 1)) % A.ntabs);
        return;
    }
    switch (vk) {
    case VK_LEFT:
    case VK_RIGHT:
        if (!B.running) {
            A.runs = CLAMP(A.runs + (vk == VK_LEFT ? -1 : 1), 1, BENCH_MAX_RUNS);
            invalidate();
        }
        break;
    case VK_UP:
    case VK_DOWN:
        A.scroll_target += vk == VK_UP ? -S(38) : S(38);
        invalidate();
        break;
    }
}

static LRESULT nc_hit(HWND h, LPARAM lp)
{
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(h, &p);
    f32 x = (f32)p.x, y = (f32)p.y;
    if (!IsZoomed(h)) {
        f32 b = S(6);
        bool L = x < b, Rr = x >= A.W - b, T = y < b, Bm = y >= A.H - b;
        if (T && L) return HTTOPLEFT;
        if (T && Rr) return HTTOPRIGHT;
        if (Bm && L) return HTBOTTOMLEFT;
        if (Bm && Rr) return HTBOTTOMRIGHT;
        if (T) return HTTOP;
        if (Bm) return HTBOTTOM;
        if (L) return HTLEFT;
        if (Rr) return HTRIGHT;
    }
    for (int i = 0; i < 2; i++) {
        Box r;
        winbtn_box(i, &r);
        if (in_b(&r, x, y)) return HTCLIENT;
    }
    if (A.modal || D.open) return HTCLIENT;
    if (y < A.card.y - S(8) && x >= sb_x() + sb_w()) return HTCAPTION;
    if (x < sb_x() + sb_w() && y < sb_x() + S(72)) return HTCAPTION;
    return HTCLIENT;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            if (IsZoomed(h)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                int f = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                InflateRect(&p->rgrc[0], -f, -f);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return nc_hit(h, lp);
    case WM_NCACTIVATE:
        return DefWindowProcW(h, msg, wp, -1);
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        if (A.s > 0) {
            mm->ptMinTrackSize.x = (LONG)S(1000);
            mm->ptMinTrackSize.y = (LONG)S(600);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        invalidate();
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        A.W = MAX(1, rc.right - rc.left);
        A.H = MAX(1, rc.bottom - rc.top);
        r_resize(A.W, A.H);
        invalidate();
        render();
        return 0;
    }
    case WM_DPICHANGED: {
        A.dpi = LOWORD(wp);
        A.s = (f32)A.dpi / 96.f;
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        invalidate();
        return 0;
    }
    case WM_MOUSEMOVE: {
        A.mx = GET_X_LPARAM(lp);
        A.my = GET_Y_LPARAM(lp);
        if (A.dragging) slider_set(A.mx);
        Hit hit = hit_test(A.mx, A.my);
        if (hit.type != A.hover.type || hit.idx != A.hover.idx) A.hover = hit;
        invalidate();
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        A.hover.type = HT_NONE;
        A.mx = A.my = -1;
        invalidate();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            int t = A.hover.type;
            bool hand = t == HT_SLIDER || t == HT_TAB || t == HT_PICK || (t == HT_START && !B.cancel) || t == HT_BIND || t == HT_RESET || t == HT_MOK ||
                        t == HT_MCANCEL || t == HT_DONATE || (t == HT_DON && (A.hover.idx >= 0 || A.hover.idx == DON_LINK || A.hover.idx == DON_CLOSE));
            SetCursor(LoadCursorW(NULL, hand ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        SetCapture(h);
        mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        mouse_up(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (D.open) {
            D.scroll_target = CLAMP(D.scroll_target - (f32)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * S(64), 0.f, MAX(0.f, D.content_h - D.view_h));
            invalidate();
            return 0;
        }
        A.scroll_target = CLAMP(A.scroll_target - (f32)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * S(76), 0.f, max_scroll());
        invalidate();
        return 0;
    case WM_HOTKEY:
        // F4 works over the fullscreen demo too: the test stops and the GPU goes back.
        if (wp == HOTKEY_STOP && B.running) {
            InterlockedExchange(&B.cancel, 1);
            invalidate();
        }
        return 0;
    case WM_KEYDOWN:
        key_down((UINT)wp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_FLASH) KillTimer(h, TIMER_FLASH);
        else if (!B.running) KillTimer(h, TIMER_TICK);
        invalidate();
        return 0;
    case WM_APP_BENCH:
        invalidate();
        return 0;
    case WM_APP_BENCH_DONE:
        KillTimer(h, TIMER_TICK);
        UnregisterHotKey(h, HOTKEY_STOP);
        if (A.close_pending) {
            DestroyWindow(h);
            return 0;
        }
        refresh_best();
        refresh_binding();
        A.tab = has_results() ? 0 : A.tab;
        // The test ran fullscreen; bring the results up.
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        FlashWindow(h, TRUE);
        invalidate();
        return 0;
    case WM_APP_TASK_DONE: {
        bool was_bound = A.bound_cpu >= 0;
        refresh_binding();
        if (A.bound_cpu >= 0) {
            A.note_bad = !wp;
            wfmt(A.note, countof(A.note),
                 wp ? TR("Шина видеокарты переключится после перезагрузки", "The GPU's PCI bridge switches after a restart")
                    : TR("Не удалось запустить службу, подробности в log.txt", "Couldn't start the service, see log.txt"));
        } else {
            A.note_bad = false;
            wcopy(A.note, countof(A.note), was_bound ? TR("Привязка сброшена, служба удалена", "Binding reset, service removed") : L"");
        }
        invalidate();
        return 0;
    }
    case WM_ACTIVATE:
        A.active = LOWORD(wp) != WA_INACTIVE;
        if (A.active) {
            theme_update();
            if (!g_task_busy) refresh_binding();
        }
        invalidate();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const WCHAR *)lp, L"ImmersiveColorSet")) {
            theme_update();
            invalidate();
        }
        return 0;
    case WM_CLOSE:
        if (B.running) {
            // Put the GPU back before leaving; the window goes when the test thread is done.
            A.close_pending = true;
            InterlockedExchange(&B.cancel, 1);
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (g_task_busy) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static UINT window_dpi(HWND h)
{
    typedef UINT (WINAPI *PFN)(HWND);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = f ? f(h) : 96;
    return d ? d : 96;
}

static bool app_open(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    theme_update();
    if (!font_init()) return false;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    // WS_CAPTION: the system open/minimize animations; no WS_SYSMENU, so DWM draws no caption buttons.
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW;
    A.hwnd = CreateWindowExW(ex, APP_CLASS, L"MixBooster", style, mi.rcWork.left, mi.rcWork.top, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!A.hwnd) return false;
    A.dpi = window_dpi(A.hwnd);
    A.s = (f32)A.dpi / 96.f;
    RECT wa = mi.rcWork;
    int w = MIN((int)S(1180), (int)(wa.right - wa.left) - 40), hgt = MIN((int)S(780), (int)(wa.bottom - wa.top) - 40);
    SetWindowPos(A.hwnd, NULL, wa.left + ((wa.right - wa.left) - w) / 2, wa.top + ((wa.bottom - wa.top) - hgt) / 2, w, hgt,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    dwm_set(A.hwnd, DWA_DARK, A.th.dark);
    dwm_set(A.hwnd, DWA_CORNER, DWC_ROUND);
    A.mica = dwm_backdrop(A.hwnd, DWB_MICA);
    RECT cr;
    GetClientRect(A.hwnd, &cr);
    A.W = MAX(1, cr.right);
    A.H = MAX(1, cr.bottom);
    if (!r_init(A.hwnd, A.W, A.H)) {
        DestroyWindow(A.hwnd);
        return false;
    }
    layout();
    A.slider_v = (f32)A.runs;
    if (A.ntabs) {
        A.tab_x = A.tabs[A.tab].x;
        A.tab_w = A.tabs[A.tab].w;
    }
    A.pick_t = 0;
    A.page_t = 0;
    A.last_frame = time_now();
    A.dirty = true;
    A.ready = true;
    render();
    ShowWindow(A.hwnd, SW_SHOW);
    SetForegroundWindow(A.hwnd);
    return true;
}

static int app_loop(void)
{
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // While the test runs behind the fullscreen demo the window stays still, so it takes nothing from the measurement.
        bool idle = B.running && !A.active;
        if (A.hwnd && IsWindowVisible(A.hwnd) && !IsIconic(A.hwnd) && !idle && (A.dirty || A.animating || !R.ok)) {
            render();
            if (!R.ok) MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        MsgWaitForMultipleObjectsEx(0, NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
