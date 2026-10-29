// The "Game fixes" page: a card about the fixes, then the games found on this PC (games.c) with their own icons and
// the fixes each has on, and "Add a game" for any other exe. A click on a game opens a dialog with a switch per fix
// (only the ones this CPU gets); each switch applies on a click, and the service or Process Lasso keeps it for later
// starts.

#define GM_ROW_H 68.f
#define GM_ICON_SLOT 96   // px, an icon's cell in the renderer's icon atlas, a cell per row of a page
#define GM_PAGE 24        // games on a page of the list
#define GM_SLOTS ((ICON_ATLAS / GM_ICON_SLOT) * (ICON_ATLAS / GM_ICON_SLOT))

enum { GMP_PREV = -1, GMP_NEXT = -2 };

enum { LS_NONE, LS_IDLE, LS_RUNNING };

static struct {
    bool ready;
    Game list[GM_MAX];
    int n;
    int lasso;             // LS_*
    int seq[GM_MAX], pend;  // clicks per game, and jobs not done: the list is read again only once they are
    bool rescan;            // a game was added or removed: read the list when the jobs are done
    int ico[GM_MAX];       // the icon's size in the atlas: 0 not read yet, -1 none
    int slot_of[GM_SLOTS]; // which game's icon an atlas cell holds now, + 1 (0 none)
    int page;              // the page of the list shown
    int npg;               // how many page buttons there are now
    Box pg_prev, pg_next, pg[16];
    f32 pg_t;              // 0..1, the rows of a newly opened page coming in
    int ico_gen;           // the renderer device the icons were uploaded to
    WCHAR note[160];
    bool note_bad;
    f32 scroll, scroll_target, scroll_v, content_h, sb_t, hero_h;
    Box hero, list_card, add, rows[GM_MAX], del[GM_MAX];
    f32 sec_y;

    // the game's dialog: which game (kept while it closes), its fixes top to bottom, the switches
    int dlg;
    bool dlg_open;
    WCHAR dlg_exe[64];     // a game just added: its dialog opens once the list is read again
    f32 dlg_t, dlg_v;
    int dlg_fx[8], ndlg;
    f32 ftog[8], ftogv[8];
    Box dlg_panel, dlg_done, dlg_rows[8], dlg_sw[8];
} GM = { .dlg = -1 };

static void gm_dlg_show(int i);
static void gm_set_page(int p);
static int gm_fix_text(int fix, WCHAR *out, int cap);

static void gm_read(void)
{
    if (GM.pend) return;
    WCHAR ini[MAX_PATH];
    GM.lasso = lasso_running() ? LS_RUNNING : lasso_ini(ini) ? LS_IDLE : LS_NONE;
    // Lasso put in or removed since: the service is needed or not any more
    if (lasso_sync()) games_svc_update(A.hwnd);
    GM.n = games_scan(GM.list);
    for (int i = 0; i < GM.n; i++) GM.ico[i] = 0;
    GM.ready = true;
    // the game whose dialog is up, or was just added, may sit elsewhere in the new list
    if (GM.dlg_exe[0]) {
        int at = -1;
        for (int i = 0; i < GM.n; i++)
            if (!_wcsicmp(GM.list[i].exe, GM.dlg_exe)) at = i;
        if (at < 0) {
            GM.dlg_open = false;
            GM.dlg = -1;
        } else if (GM.dlg_open) {
            GM.dlg = at;
        } else {
            GM.page = at / GM_PAGE;
            gm_dlg_show(at);
        }
    }
}

// The jobs: a switch, a game added, a game removed. Once none is left the service is put in or removed as the games
// now need it.
static bool gm_set_run(Job *j)
{
    WCHAR exe[64];
    exe_lower(exe, countof(exe), j->u.game.exe);
    int was = gm_fix(exe);
    if (game_set(&j->u.game, j->u.game.fix)) return true;
    // half made: back to how it was, as the switch goes back
    j->u.game.fix = was;
    game_set(&j->u.game, was);
    return false;
}

static bool gm_forget_run(Job *j) { return game_forget(&j->u.game); }

static void gm_job_done(Job *j, bool ok)
{
    GM.pend--;
    if (!ok) {
        if (j->idx >= 0 && j->idx < GM.n && j->seq == GM.seq[j->idx] && !_wcsicmp(GM.list[j->idx].exe, j->u.game.exe)) GM.list[j->idx].fix = j->u.game.fix;
        toast_show(TR("Не удалось применить фикс к игре: подробности в log.txt", "Couldn't apply the fix to the game: details in log.txt"));
    }
    if (GM.pend) return;
    games_svc_update(A.hwnd);
    if (GM.rescan) {
        GM.rescan = false;
        GM.ready = false;
    }
}

static void gm_post(int i, const Game *g, bool (*run)(Job *))
{
    Job j = { run, gm_job_done, i, i >= 0 ? ++GM.seq[i] : 0, false };
    j.u.game = *g;
    GM.pend++;
    job_post(A.hwnd, &j);
}

// One fix of the dialog's game, on or off.
static void gm_fix_toggle(int k)
{
    if (GM.dlg < 0 || GM.dlg >= GM.n || k < 0 || k >= GM.ndlg) return;
    Game *g = &GM.list[GM.dlg];
    g->fix ^= GM.dlg_fx[k];
    GM.note[0] = 0;
    gm_post(GM.dlg, g, gm_set_run);
}

// "Add a game": any exe; a known one keeps its name. Its dialog opens to pick the fixes.
static void gm_add(void)
{
    WCHAR path[MAX_PATH] = L"";
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = A.hwnd;
    of.lpstrFilter = g_lang_ru ? L"Программы (*.exe)\0*.exe\0" : L"Programs (*.exe)\0*.exe\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = TR("Выберите exe игры", "Pick the game's exe");
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    const WCHAR *exe = path_file(path);
    for (int i = 0; i < GM.n; i++)
        if (!_wcsicmp(GM.list[i].exe, exe)) {
            wcopy(GM.list[i].path, MAX_PATH, path);
            GM.ico[i] = 0;
            gm_set_page(i / GM_PAGE);
            gm_dlg_show(i);
            return;
        }
    Game g;
    memset(&g, 0, sizeof g);
    wcopy(g.exe, countof(g.exe), exe);
    wcopy(g.path, MAX_PATH, path);
    wcopy(g.name, countof(g.name), exe);
    WCHAR *dot = wcsrchr(g.name, L'.');
    if (dot && !_wcsicmp(dot, L".exe")) *dot = 0;
    g.custom = true;
    for (int k = 0; k < countof(k_games); k++)
        if (!_wcsicmp(k_games[k].exe, exe)) {
            wcopy(g.name, countof(g.name), k_games[k].name);
            g.custom = false;
        }
    GM.rescan = true;
    wcopy(GM.dlg_exe, countof(GM.dlg_exe), g.exe);
    gm_post(-1, &g, gm_set_run);
}

static void gm_remove(int i)
{
    GM.rescan = true;
    gm_post(-1, &GM.list[i], gm_forget_run);
    // gone from the list at once; the list is read again when the job is done
    memmove(&GM.list[i], &GM.list[i + 1], sizeof(Game) * (size_t)(GM.n - i - 1));
    memmove(&GM.seq[i], &GM.seq[i + 1], sizeof(int) * (size_t)(GM.n - i - 1));
    GM.n--;
    // an icon's atlas cell goes by its index: the ones that moved up are read again
    for (int k = i; k < GM.n; k++) GM.ico[k] = 0;
}

static void gm_done(bool ok)
{
    if (ok) {
        GM.note[0] = 0;
        // a toggle made while the service was being put in or removed
        games_svc_update(A.hwnd);
        return;
    }
    wcopy(GM.note, countof(GM.note),
          TR("Служба MixBooster не запустилась: фиксы не будут ставиться при следующих запусках игр. Подробности в log.txt",
             "The MixBooster service didn't start: the fixes won't be set on the next game starts. Details in log.txt"));
    GM.note_bad = true;
}

// --- the games' icons ----------------------------------------------------------------------------------------------

// The exe's own icon at px, premultiplied BGRA as the icon atlas takes it; false when it has none.
static bool exe_icon(const WCHAR *path, int px, u32 *out)
{
    HICON ico = NULL;
    if (PrivateExtractIconsW(path, 0, px, px, &ico, NULL, 1, 0) != 1 || !ico) return false;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB } };
    void *bits = NULL, *mbits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP mb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &mbits, NULL, 0);
    bool ok = false;
    if (bm && mb) {
        HGDIOBJ old = SelectObject(dc, bm);
        memset(bits, 0, (size_t)px * px * 4);
        DrawIconEx(dc, 0, 0, ico, px, px, 0, NULL, DI_NORMAL);
        const u32 *src = (const u32 *)bits;
        bool alpha = false;
        for (int i = 0; i < px * px && !alpha; i++) alpha = (src[i] >> 24) != 0;
        if (!alpha) {
            // an old icon without alpha: its mask says what is transparent
            SelectObject(dc, mb);
            memset(mbits, 0, (size_t)px * px * 4);
            DrawIconEx(dc, 0, 0, ico, px, px, 0, NULL, DI_MASK);
        }
        const u32 *msk = (const u32 *)mbits;
        for (int i = 0; i < px * px; i++)
            out[i] = alpha ? src[i] : (msk[i] & 0xFFFFFF) ? 0 : (src[i] | 0xFF000000u);
        SelectObject(dc, old);
        ok = true;
    }
    if (bm) DeleteObject(bm);
    if (mb) DeleteObject(mb);
    DeleteDC(dc);
    DestroyIcon(ico);
    return ok;
}

// The list in pages of GM_PAGE games: how many, and the games of the page shown.
static int gm_npages(void) { return MAX(1, (GM.n + GM_PAGE - 1) / GM_PAGE); }
static int gm_first(void) { return GM.page * GM_PAGE; }
static int gm_last(void) { return MIN(GM.n, gm_first() + GM_PAGE); }

// Loads the icons of the page shown into the atlas, a cell per row, at the size they are drawn (read again when the
// DPI, the device or the page changes: another page's game may have the cell now).
static void gm_icons(int px)
{
    px = MIN(px, GM_ICON_SLOT);
    if (GM.ico_gen != g_r_gen) {
        GM.ico_gen = g_r_gen;
        for (int i = 0; i < GM.n; i++) GM.ico[i] = 0;
        memset(GM.slot_of, 0, sizeof GM.slot_of);
    }
    static u32 buf[GM_ICON_SLOT * GM_ICON_SLOT];
    int per = ICON_ATLAS / GM_ICON_SLOT;
    for (int i = gm_first(); i < gm_last(); i++) {
        int slot = i - gm_first();
        if (GM.ico[i] > 0 && GM.slot_of[slot] != i + 1) GM.ico[i] = 0;
        if (GM.ico[i] == px || GM.ico[i] < 0) continue;
        if (!GM.list[i].path[0] || !exe_icon(GM.list[i].path, px, buf)) {
            GM.ico[i] = -1;
            continue;
        }
        r_upload_icon((slot % per) * GM_ICON_SLOT, (slot / per) * GM_ICON_SLOT, px, buf);
        GM.slot_of[slot] = i + 1;
        GM.ico[i] = px;
    }
}

static void gm_draw_icon(int i, f32 x, f32 y, f32 sz)
{
    const Theme *t = &A.th;
    int per = ICON_ATLAS / GM_ICON_SLOT, slot = i - gm_first();
    if (GM.ico[i] > 0) {
        f32 u = (f32)((slot % per) * GM_ICON_SLOT) / ICON_ATLAS, v = (f32)((slot / per) * GM_ICON_SLOT) / ICON_ATLAS, d = (f32)GM.ico[i] / ICON_ATLAS;
        r_icon(x, y, sz, sz, u, v, u + d, v + d, 1.f);
        return;
    }
    // no icon: the name's first letter on a tile
    r_rect(x, y, sz, sz, color_alpha(t->accent, t->dark ? 0.16f : 0.14f), floorf(sz * 0.28f));
    WCHAR l[2] = { (WCHAR)towupper(GM.list[i].name[0]), 0 };
    text_center(FONT_TEXT_SEMIBOLD, sz * 0.42f, x + sz * 0.5f, y + sz * 0.5f, l, t->accent);
}

// --- the page ------------------------------------------------------------------------------------------------------

static f32 gm_view_h(void) { return (f32)A.H - S(HW_TOP) - S(8); }
static f32 gm_max_scroll(void) { return MAX(0.f, GM.content_h - gm_view_h()); }

static const WCHAR *gm_desc(void)
{
    return TR("Нажмите на игру, чтобы выбрать её фиксы: приоритет, ядра процессора, энергосбережение. Фиксы для отдельных процессоров видны только на них. Всё держится при каждом запуске игры.",
              "Click a game to pick its fixes: priority, CPU cores, power saving. The fixes for particular CPUs show only on them. All of it is kept on every start of the game.");
}

// The callouts under the explanation: where Process Lasso stands.
static int gm_notes(const WCHAR **txt, u32 *col, u32 *glyph)
{
    const Theme *t = &A.th;
    int n = 0;
    if (GM.lasso == LS_RUNNING) {
        txt[n] = TR("Process Lasso запущен: приоритет и ядра игр держит он, служба MixBooster для них не нужна",
                    "Process Lasso is running: it keeps the games' priority and cores, the MixBooster service isn't needed for them");
        col[n] = t->good;
        glyph[n++] = 0xE73E;
    } else if (GM.lasso == LS_IDLE) {
        txt[n] = TR("Process Lasso установлен, но не запущен: правила записаны в него и заработают, когда он запустится",
                    "Process Lasso is installed but not running: the rules are in it and work once it starts");
        col[n] = t->warn;
        glyph[n++] = 0xE946;
    }
    return n;
}

static int gm_on_count(void)
{
    int n = 0;
    for (int i = 0; i < GM.n; i++) n += GM.list[i].fix != 0;
    return n;
}

static void gm_layout(void)
{
    f32 x0 = content_x0(), cw = content_x1() - x0, y = S(HW_TOP) - floorf(GM.scroll + 0.5f);
    // the fix's card grows with the explanation's lines and the callouts under it
    f32 tx = x0 + S(24) + S(44) + S(16), tw = x0 + cw - S(24) - S(150) - tx;
    int lines = text_wrap(FONT_TEXT, SC(13), tx, 0, tw, S(18), gm_desc(), 0, false);
    const WCHAR *nt[1];
    u32 nc[1], ng[1];
    int notes = gm_notes(nt, nc, ng);
    GM.hero_h = S(60) + S(18) * (f32)lines + S(20) + (notes ? S(44) * (f32)notes : 0);
    GM.hero = (Box){ x0, y, cw, GM.hero_h };

    // "Games", the count and "Add a game" over the list
    GM.sec_y = y + GM.hero_h + S(22);
    const WCHAR *lbl = TR("Добавить игру", "Add a game");
    f32 bw = floorf(text_width(FONT_TEXT_SEMIBOLD, SC(13), lbl, -1) + S(54));
    GM.add = (Box){ x0 + cw - bw, GM.sec_y, bw, S(34) };

    f32 ly = GM.sec_y + S(46);
    GM.page = CLAMP(GM.page, 0, gm_npages() - 1);
    int first = gm_first(), last = gm_last();
    for (int i = first; i < last; i++) {
        GM.rows[i] = (Box){ x0 + S(6), ly + S(6) + S(GM_ROW_H) * (f32)(i - first), cw - S(12), S(GM_ROW_H) };
        GM.del[i] = GM.list[i].custom ? (Box){ x0 + cw - S(22) - S(32), GM.rows[i].y + (S(GM_ROW_H) - S(32)) * 0.5f, S(32), S(32) } : (Box){ 0 };
    }
    GM.list_card = (Box){ x0, ly, cw, GM.n ? S(GM_ROW_H) * (f32)(last - first) + S(12) : S(150) };
    f32 bottom = GM.list_card.y + GM.list_card.h;

    // more than a page: the pages under the list, centered: back, a button per page, forward
    GM.npg = gm_npages() > 1 ? MIN(gm_npages(), countof(GM.pg)) : 0;
    if (GM.npg) {
        f32 bs = S(34), gap = S(6), w = bs * (f32)(GM.npg + 2) + gap * (f32)(GM.npg + 1), px = floorf(x0 + (cw - w) * 0.5f), py = bottom + S(14);
        GM.pg_prev = (Box){ px, py, bs, bs };
        for (int k = 0; k < GM.npg; k++) GM.pg[k] = (Box){ px + (bs + gap) * (f32)(k + 1), py, bs, bs };
        GM.pg_next = (Box){ px + (bs + gap) * (f32)(GM.npg + 1), py, bs, bs };
        bottom = py + bs;
    }
    GM.content_h = bottom - y + (GM.note[0] ? S(48) : S(16));
}

static void gm_card(const Box *b)
{
    draw_card(b);
}

// A capsule with a small glyph and a label; returns its width.
static f32 gm_badge(f32 x, f32 cy, u32 glyph, const WCHAR *label, u32 fg, u32 bg)
{
    f32 fs = SC(11.5f), h = S(22), tw = text_width(FONT_TEXT, fs, label, -1), w = S(10) + S(13) + S(5) + tw + S(10);
    r_rect(x, floorf(cy - h * 0.5f), w, h, bg, h * 0.5f);
    text_draw_icon(glyph, SC(11), x + S(10) + S(6.5f), cy, fg);
    text_draw(FONT_TEXT, fs, x + S(28), baseline_for(FONT_TEXT, fs, cy), label, -1, fg);
    return w;
}

static void draw_gm_hero(void)
{
    const Theme *t = &A.th;
    Box *b = &GM.hero;
    gm_card(b);
    f32 isz = S(44), ix = b->x + S(24), tx = ix + isz + S(16), tw = b->x + b->w - S(24) - S(150) - tx;
    draw_icon_tile(ix, b->y + S(24), isz, 0xE7FC);
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(16), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(16), b->y + S(36)), tw,
                  TR("Фиксы для игр", "Fixes for games"), -1, t->text, false);
    text_wrap(FONT_TEXT, SC(13), tx, baseline_for(FONT_TEXT, SC(13), b->y + S(62)), tw, S(18), gm_desc(), t->dim, true);

    // how many games have it, on the right
    WCHAR num[16], of[32];
    int on = gm_on_count();
    wfmt(num, countof(num), L"%d", on);
    wfmt(of, countof(of), L" / %d", GM.n);
    f32 right = b->x + b->w - S(26), nfs = SC(30), ofs = SC(16), ny = b->y + S(46);
    f32 ow = text_width(FONT_DISPLAY, ofs, of, -1), nw = text_width(FONT_DISPLAY, nfs, num, -1);
    text_draw(FONT_DISPLAY, nfs, right - ow - nw, baseline_for(FONT_DISPLAY, nfs, ny), num, -1, on ? t->accent : t->text);
    text_draw(FONT_DISPLAY, ofs, right - ow, baseline_for(FONT_DISPLAY, nfs, ny), of, -1, t->faint);
    text_right(FONT_TEXT, SC(12), right, ny + S(26), TR("игр с фиксами", "games with fixes"), t->faint);

    const WCHAR *nt[1];
    u32 nc[1], ng[1];
    int notes = gm_notes(nt, nc, ng);
    for (int i = 0; i < notes; i++) {
        Box cb = { b->x + S(24), b->y + b->h - S(20) - S(36) - S(44) * (f32)(notes - 1 - i), b->w - S(48), S(36) };
        f32 cy = cb.y + cb.h * 0.5f;
        r_rect(cb.x, cb.y, cb.w, cb.h, color_alpha(nc[i], t->dark ? 0.12f : 0.10f), S(8));
        text_draw_icon(ng[i], SC(14), cb.x + S(20), cy, nc[i]);
        text_draw_fit(FONT_TEXT, SC(12.5f), cb.x + S(38), baseline_for(FONT_TEXT, SC(12.5f), cy), cb.w - S(52), nt[i], -1,
                      color_mix(nc[i], t->text, 0.25f), false);
    }
}

// A button of the pages: a number, or an arrow (glyph); the page shown is on an accent plate.
static void gm_page_btn(const Box *b, int idx, const WCHAR *label, u32 glyph, bool sel, bool off)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    bool hov = !off && !sel && A.hover.type == HT_GMPAGE && A.hover.idx == idx;
    f32 hk = hover_amt(b, hov);
    RXf xf = press_push(b, off ? 0.f : press_amt(b, hov && A.press.type == HT_GMPAGE && A.press.idx == idx));
    u32 bg = sel ? color_alpha(t->accent, t->dark ? 0.2f : 0.16f) : color_alpha(c->ctl_hover, hk);
    r_rect(b->x, b->y, b->w, b->h, bg, S(8));
    f32 cx = b->x + b->w * 0.5f, cy = b->y + b->h * 0.5f;
    u32 fg = off ? t->faint : sel ? t->accent : color_mix(t->dim, t->text, hk);
    if (glyph) text_draw_icon(glyph, SC(11), cx, cy, fg);
    else text_center(sel ? FONT_TEXT_SEMIBOLD : FONT_TEXT, SC(13), cx, cy, label, fg);
    r_xf_pop(xf);
}

static void draw_gm_pages(void)
{
    const Theme *t = &A.th;
    int np = gm_npages();
    gm_page_btn(&GM.pg_prev, GMP_PREV, NULL, 0xE76B, false, GM.page == 0);
    for (int k = 0; k < GM.npg; k++) {
        WCHAR num[8];
        wfmt(num, countof(num), L"%d", k + 1);
        gm_page_btn(&GM.pg[k], k, num, 0, k == GM.page, false);
    }
    gm_page_btn(&GM.pg_next, GMP_NEXT, NULL, 0xE76C, false, GM.page >= np - 1);
    // which games are shown, at the left
    WCHAR rng[64];
    wfmt(rng, countof(rng), TR("%d-%d из %d", "%d-%d of %d"), gm_first() + 1, gm_last(), GM.n);
    text_draw(FONT_TEXT, SC(12.5f), GM.list_card.x + S(4), baseline_for(FONT_TEXT, SC(12.5f), GM.pg_prev.y + GM.pg_prev.h * 0.5f), rng, -1, t->faint);
}

static void gm_set_page(int p)
{
    p = CLAMP(p, 0, gm_npages() - 1);
    if (p == GM.page) return;
    GM.page = p;
    GM.pg_t = g_reduced_motion ? 1.f : 0.f;
    // back up to the top of the list when it is scrolled past
    f32 top = GM.hero_h + S(22);
    if (GM.scroll_target > top) GM.scroll_target = top;
    A.hover.type = HT_NONE;
    invalidate();
}

static void draw_gm_list(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    f32 x0 = GM.list_card.x, cy = GM.sec_y + S(17);
    f32 hx = text_draw(FONT_TEXT_SEMIBOLD, SC(15), x0 + S(2), baseline_for(FONT_TEXT_SEMIBOLD, SC(15), cy), TR("Игры", "Games"), -1, t->text);
    if (GM.n) {
        WCHAR cnt[16];
        wfmt(cnt, countof(cnt), L"%d", GM.n);
        f32 pw = MAX(S(24), text_width(FONT_TEXT_SEMIBOLD, SC(11.5f), cnt, -1) + S(14)), px = hx + S(10);
        r_rect(px, cy - S(11), pw, S(22), c->ctl, S(11));
        text_center(FONT_TEXT_SEMIBOLD, SC(11.5f), px + pw * 0.5f, cy, cnt, t->dim);
    }
    draw_button(&GM.add, TR("Добавить игру", "Add a game"), 0xE710, BTN_ACCENT, hover_amt(&GM.add, A.hover.type == HT_GMADD),
                A.press.type == HT_GMADD && A.hover.type == HT_GMADD, false);

    Box *b = &GM.list_card;
    gm_card(b);
    if (!GM.n) {
        f32 my = b->y + b->h * 0.5f;
        text_draw_icon(0xE7FC, SC(26), b->x + b->w * 0.5f, my - S(24), t->faint);
        text_center(FONT_TEXT_SEMIBOLD, SC(13.5f), b->x + b->w * 0.5f, my + S(10), TR("Известные игры не найдены", "No known games found"), t->dim);
        text_center(FONT_TEXT, SC(12.5f), b->x + b->w * 0.5f, my + S(32), TR("Добавьте exe своей игры кнопкой «Добавить игру»", "Add your game's exe with Add a game"), t->faint);
        return;
    }
    f32 isz = S(36);
    gm_icons((int)isz);
    int first = gm_first(), last = gm_last();
    f32 base = R.opacity;
    for (int i = first; i < last; i++) {
        const Game *g = &GM.list[i];
        Box *r = &GM.rows[i];
        // a page just opened: its rows come in one after another
        f32 kr = ease_out_cubic(GM.pg_t * 1.6f - 0.03f * (f32)(i - first));
        R.opacity = base * kr;
        RXf rxf = r_xf_push(1.f, 0, 0, 0, (1.f - kr) * S(8));
        bool hov = (A.hover.type == HT_GMROW || A.hover.type == HT_GMDEL) && A.hover.idx == i;
        f32 rh = hover_amt(r, hov), nh = i + 1 < last ? hover_amt(&GM.rows[i + 1], (A.hover.type == HT_GMROW || A.hover.type == HT_GMDEL) && A.hover.idx == i + 1) : 0.f;
        if (rh > 0) r_rect(r->x, r->y + S(2), r->w, r->h - S(4), color_alpha(c->nav_hover, rh), S(8));
        // the line under a row gives way to the hover of either row next to it
        if (i + 1 < last) r_rect(r->x + S(16), r->y + r->h - 1, r->w - S(32), 1, color_alpha(t->sep, 0.7f * (1.f - MAX(rh, nh))), 0);
        f32 ry = r->y + r->h * 0.5f, ix = r->x + S(16), nx = ix + isz + S(14);
        f32 right = (g->custom ? GM.del[i].x : r->x + r->w - S(16)) - S(16);
        gm_draw_icon(i, ix, ry - isz * 0.5f, isz);

        // the name, and what is true of it now
        f32 ny = ry - S(11);
        f32 nw = text_draw_fit(FONT_TEXT_SEMIBOLD, SC(14), nx, baseline_for(FONT_TEXT_SEMIBOLD, SC(14), ny), right - nx - S(170), g->name, -1, t->text, false);
        f32 px = nx + nw + S(10);
        if (g->running) {
            const WCHAR *lbl = TR("запущена", "running");
            f32 pw = text_width(FONT_TEXT_SEMIBOLD, SC(11), lbl, -1) + S(22);
            u32 pc = g->fix ? t->good : t->dim;
            r_rect(px, ny - S(9), pw, S(18), color_alpha(pc, 0.16f), S(9));
            r_rect(px + S(8), ny - S(2.5f), S(5), S(5), pc, S(2.5f));
            text_draw(FONT_TEXT_SEMIBOLD, SC(11), px + S(16), baseline_for(FONT_TEXT_SEMIBOLD, SC(11), ny), lbl, -1, pc);
            px += pw + S(6);
        }
        if (g->custom) {
            const WCHAR *lbl = TR("своя", "added");
            f32 pw = text_width(FONT_TEXT_SEMIBOLD, SC(11), lbl, -1) + S(16);
            r_rect(px, ny - S(9), pw, S(18), color_alpha(t->accent, 0.14f), S(9));
            text_center(FONT_TEXT_SEMIBOLD, SC(11), px + pw * 0.5f, ny, lbl, t->accent);
        }

        // where it lives: the drive and the exe
        f32 by = ry + S(13), bx = nx;
        u32 bfg = t->dim, bbg = c->ctl;
        if (g->path[0] && g->path[1] == L':') {
            WCHAR drv[32];
            wfmt(drv, countof(drv), TR("Диск %c", "Drive %c"), towupper(g->path[0]));
            bx += gm_badge(bx, by, 0xEDA2, drv, bfg, bbg) + S(6);
        } else if (g->path[0] == L'\\') {
            bx += gm_badge(bx, by, 0xE968, TR("Сеть", "Network"), bfg, bbg) + S(6);
        }
        gm_badge(bx, by, 0xE7C3, g->exe, bfg, bbg);

        if (g->custom) {
            Box *d = &GM.del[i];
            bool dh = A.hover.type == HT_GMDEL && A.hover.idx == i;
            f32 dk = hover_amt(d, dh);
            if (dk > 0) r_rect(d->x, d->y, d->w, d->h, color_alpha(color_alpha(t->danger, 0.14f), dk), S(6));
            RXf dxf = press_push(d, press_amt(d, dh && A.press.type == HT_GMDEL));
            text_draw_icon(0xE74D, SC(13), d->x + d->w * 0.5f, d->y + d->h * 0.5f, color_mix(t->faint, t->danger, dk));
            r_xf_pop(dxf);
        }
        // the fixes on, and the way into the dialog
        WCHAR on[160];
        u32 oc = g->fix ? t->accent : t->faint;
        if (!gm_fix_text(g->fix, on, countof(on))) wcopy(on, countof(on), TR("нет фиксов", "no fixes"));
        text_draw_icon(0xE76C, SC(11), right - S(4), ry, color_mix(t->faint, t->text, rh));
        f32 ow = MIN(text_width(FONT_TEXT_SEMIBOLD, SC(12.5f), on, -1), S(260));
        text_draw_fit(FONT_TEXT_SEMIBOLD, SC(12.5f), right - S(18) - ow, baseline_for(FONT_TEXT_SEMIBOLD, SC(12.5f), ry), ow, on, -1, oc, false);
        r_xf_pop(rxf);
    }
    R.opacity = base;
    if (GM.npg) draw_gm_pages();
}

static void draw_game_page(void)
{
    const Theme *t = &A.th;
    if (!GM.ready) gm_read();
    gm_layout();
    r_set_clip(0, S(HW_TOP) - S(6), (f32)A.W, (f32)A.H);
    draw_gm_hero();
    draw_gm_list();
    if (GM.note[0]) {
        f32 ny = (GM.npg ? GM.pg_prev.y + GM.pg_prev.h : GM.list_card.y + GM.list_card.h) + S(24);
        text_draw_icon(0xE783, SC(13), GM.list_card.x + S(10), ny, t->danger);
        text_draw_fit(FONT_TEXT, SC(12.5f), GM.list_card.x + S(26), baseline_for(FONT_TEXT, SC(12.5f), ny), GM.list_card.w - S(26), GM.note, -1, t->danger,
                      false);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = gm_max_scroll();
    if (ms > 0 && GM.sb_t > 0) {
        f32 vh = gm_view_h(), bar = MAX(S(32), vh * vh / GM.content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(GM.scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, GM.sb_t), S(1.5f));
    }
}

// --- a game's fixes ------------------------------------------------------------------------------------------------

static const struct {
    int bit;
    u32 glyph;
    const WCHAR *ru, *en, *dru, *den, *sru, *sen;  // the name, what it does in a line, the short name in the list
} k_fx[] = {
    { FX_HIGH, 0xEC4A, L"Высокий приоритет CPU", L"High CPU priority", L"Windows запускает игру с высоким приоритетом процессора",
      L"Windows starts the game with High CPU priority", L"CPU High", L"CPU High" },
    { FX_GPU, 0xF211, L"Высокий приоритет GPU", L"High GPU priority", L"Видеокарта выполняет работу игры раньше фоновых программ",
      L"The GPU runs the game's work ahead of background apps", L"GPU High", L"GPU High" },
    { FX_CPU0, 0xEEA1, L"Без CPU 0", L"CPU 0 off", L"CPU 0 остаётся Windows и прерываниям драйверов",
      L"CPU 0 is left to Windows and the drivers' interrupts", L"без CPU 0", L"no CPU 0" },
    { FX_PCORES, 0xE950, L"Только P-ядра", L"P-cores only", L"Игра не попадает на медленные E-ядра",
      L"The game stays off the slower E-cores", L"P-ядра", L"P-cores" },
    { FX_VCACHE, 0xE964, L"Только CCD с 3D V-Cache", L"3D V-Cache CCD only", L"Игра работает только на кристалле с большим кэшем L3",
      L"The game runs only on the die with the big L3 cache", L"V-Cache", L"V-Cache" },
    { FX_ECO, 0xE945, L"Без энергосбережения (EcoQoS)", L"No power throttling (EcoQoS)", L"Windows не снижает игре частоту, когда окно не в фокусе",
      L"Windows doesn't slow the game down when its window loses focus", L"без EcoQoS", L"no EcoQoS" },
};

static int fx_index(int bit)
{
    for (int k = 0; k < countof(k_fx); k++)
        if (k_fx[k].bit == bit) return k;
    return 0;
}

// The short names of the fixes on, joined; returns how many.
static int gm_fix_text(int fix, WCHAR *out, int cap)
{
    int n = 0, len = 0;
    out[0] = 0;
    for (int k = 0; k < countof(k_fx); k++)
        if (fix & k_fx[k].bit) {
            int w = wfmt(out + len, cap - len, n ? L" · %s" : L"%s", g_lang_ru ? k_fx[k].sru : k_fx[k].sen);
            if (w > 0) len += w;
            n++;
        }
    return n;
}

static void gm_dlg_show(int i)
{
    if (i < 0 || i >= GM.n) return;
    GM.dlg = i;
    GM.dlg_open = true;
    GM.dlg_t = GM.dlg_v = 0;
    wcopy(GM.dlg_exe, countof(GM.dlg_exe), GM.list[i].exe);
    // the fixes this CPU gets, and any that is on from before
    int avail = fx_avail() | GM.list[i].fix;
    GM.ndlg = 0;
    for (int k = 0; k < countof(k_fx); k++)
        if (avail & k_fx[k].bit) {
            GM.ftog[GM.ndlg] = (GM.list[i].fix & k_fx[k].bit) ? 1.f : 0.f;
            GM.ftogv[GM.ndlg] = 0;
            GM.dlg_fx[GM.ndlg++] = k_fx[k].bit;
        }
    A.hover.type = HT_NONE;
    invalidate();
}

static void gm_dlg_close(void)
{
    GM.dlg_open = false;
    GM.dlg_exe[0] = 0;
    A.hover.type = HT_NONE;
    invalidate();
}

static bool gm_dlg_open(void) { return GM.dlg_open; }

// The note after a fix's line: the CPUs it leaves the game, or a warning.
static void gm_fix_note(int bit, WCHAR *out, int cap, u32 *col)
{
    const Theme *t = &A.th;
    WCHAR mt[120];
    out[0] = 0;
    *col = t->faint;
    if (bit == FX_CPU0 && A.cpu.count <= 4) {
        wcopy(out, cap, TR("мало потоков, FPS может упасть", "few threads, FPS can drop"));
        *col = t->warn;
    } else if (bit == FX_PCORES || bit == FX_VCACHE) {
        mask_text(bit == FX_PCORES ? GC.pcores : GC.vcache, mt, countof(mt));
        wcopy(out, cap, mt);
    }
}

// Lays the dialog out; with draw set it also draws it.
static void gm_dlg_pass(bool draw)
{
    if (GM.dlg < 0 || GM.dlg >= GM.n) return;
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    const Game *g = &GM.list[GM.dlg];
    // a fix is a short row: there will be more of them
    f32 w = MIN(S(580), (f32)A.W - S(40)), pad = S(24), rh = S(54), head = S(44);
    f32 h = pad + head + S(12) + rh * (f32)GM.ndlg + S(12) + S(34) + pad;
    f32 x = floorf(((f32)A.W - w) * 0.5f), y = floorf(((f32)A.H - h) * 0.5f), ry = y + pad + head + S(12);
    GM.dlg_panel = (Box){ x, y, w, h };
    for (int k = 0; k < GM.ndlg; k++) {
        GM.dlg_rows[k] = (Box){ x + S(8), ry + rh * (f32)k, w - S(16), rh };
        GM.dlg_sw[k] = (Box){ x + w - S(24) - S(52), GM.dlg_rows[k].y + (rh - S(32)) * 0.5f, S(52), S(32) };
    }
    GM.dlg_done = (Box){ x + w - pad - S(130), y + h - pad - S(34), S(130), S(34) };
    if (!draw) return;

    RXf xf = popup_push(&GM.dlg_panel, GM.dlg_t);
    r_rect(x, y, w, h, t->dark ? RGBA(43, 43, 46, 255) : RGBA(251, 251, 252, 255), S(14));
    r_rect_ex(x, y, w, h, c->pop_border, S(14), 1.f, 0);

    // the game: its icon, name and exe
    f32 isz = S(36), cy = y + pad + head * 0.5f, tx = x + pad + isz + S(14);
    gm_draw_icon(GM.dlg, x + pad, floorf(cy - isz * 0.5f), isz);
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(18), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(18), cy - S(9)), x + w - pad - tx, g->name, -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(12.5f), tx, baseline_for(FONT_TEXT, SC(12.5f), cy + S(12)), x + w - pad - tx, g->exe, -1, t->dim, false);

    for (int k = 0; k < GM.ndlg; k++) {
        Box *r = &GM.dlg_rows[k];
        int fi = fx_index(GM.dlg_fx[k]);
        bool hov = A.hover.type == HT_GMFIX && A.hover.idx == k;
        f32 rhv = hover_amt(r, hov);
        if (rhv > 0) r_rect(r->x, r->y + S(2), r->w, r->h - S(4), color_alpha(c->nav_hover, rhv), S(8));
        // the icon, the name, and what it does in a line with its note after it
        f32 rcy = r->y + r->h * 0.5f, isz2 = S(30), ix = r->x + S(14), rtx = ix + isz2 + S(12), tw = GM.dlg_sw[k].x - S(12) - rtx;
        draw_icon_tile(ix, floorf(rcy - isz2 * 0.5f), isz2, k_fx[fi].glyph);
        text_draw_fit(FONT_TEXT_SEMIBOLD, SC(13.5f), rtx, baseline_for(FONT_TEXT_SEMIBOLD, SC(13.5f), rcy - S(9)), tw, g_lang_ru ? k_fx[fi].ru : k_fx[fi].en, -1, t->text, false);
        WCHAR note[160], nl[170];
        u32 nc;
        gm_fix_note(GM.dlg_fx[k], note, countof(note), &nc);
        f32 dfs = SC(12), dby = baseline_for(FONT_TEXT, dfs, rcy + S(10)), nw = 0;
        if (note[0]) {
            wfmt(nl, countof(nl), L" · %s", note);
            nw = MIN(text_width(FONT_TEXT, dfs, nl, -1), tw * 0.6f);
        }
        f32 dw = text_draw_fit(FONT_TEXT, dfs, rtx, dby, tw - nw, g_lang_ru ? k_fx[fi].dru : k_fx[fi].den, -1, t->dim, false);
        if (note[0]) text_draw_fit(FONT_TEXT, dfs, rtx + dw, dby, nw, nl, -1, nc, false);
        draw_switch(&GM.dlg_sw[k], GM.ftog[k], true, hov);
        if (k + 1 < GM.ndlg) r_rect(r->x + S(14), r->y + r->h - 1, r->w - S(28), 1, color_alpha(t->sep, 0.7f * (1.f - rhv)), 0);
    }

    // where the game runs now, and Done
    r_rect(x + S(1), GM.dlg_done.y - S(12), w - S(2), 1, t->sep, 0);
    WCHAR foot[160], mt[120];
    if (g->fix & FX_AFF) {
        gm_cpu();
        mask_text(fx_mask(g->fix, GC.all), mt, countof(mt));
        wfmt(foot, countof(foot), TR("Ядра игры: %s", "The game's cores: %s"), mt);
    } else {
        wcopy(foot, countof(foot), TR("Применяется сразу и держится при каждом запуске игры", "Applies at once and is kept on every start of the game"));
    }
    f32 my = GM.dlg_done.y + GM.dlg_done.h * 0.5f;
    text_draw_fit(FONT_TEXT, SC(12.5f), x + pad, baseline_for(FONT_TEXT, SC(12.5f), my), GM.dlg_done.x - x - pad - S(12), foot, -1, t->dim, false);
    draw_button(&GM.dlg_done, TR("Готово", "Done"), 0, BTN_ACCENT, hover_amt(&GM.dlg_done, A.hover.type == HT_GMDONE),
                A.press.type == HT_GMDONE && A.hover.type == HT_GMDONE, false);
    r_xf_pop(xf);
    R.opacity = 1.f;
}

static void draw_gm_dialog(void)
{
    if (GM.dlg_open || GM.dlg_t > 0) gm_dlg_pass(true);
}

static Hit gm_dlg_hit(f32 x, f32 y)
{
    gm_dlg_pass(false);
    if (in_b(&GM.dlg_done, x, y)) return (Hit){ HT_GMDONE, -1 };
    for (int k = 0; k < GM.ndlg; k++)
        if (in_b(&GM.dlg_rows[k], x, y)) return (Hit){ HT_GMFIX, k };
    return (Hit){ in_b(&GM.dlg_panel, x, y) ? HT_GMDLG : HT_GMOUT, -1 };
}

static bool gm_animate(f32 dt)
{
    bool anim = false;
    // the dialog opens with a little bounce and closes quickly without one, as the others
    anim |= spring(&GM.dlg_t, &GM.dlg_v, GM.dlg_open ? 1.f : 0.f, GM.dlg_open ? 0.42f : 0.24f, GM.dlg_open ? 0.74f : 1.f, 0.002f, dt);
    if (!GM.dlg_open && GM.dlg_t <= 0.f) GM.dlg = -1;
    if (GM.dlg >= 0 && GM.dlg < GM.n)
        for (int k = 0; k < GM.ndlg; k++)
            anim |= spring(&GM.ftog[k], &GM.ftogv[k], (GM.list[GM.dlg].fix & GM.dlg_fx[k]) ? 1.f : 0.f, 0.34f, 0.7f, 0.002f, dt);
    if (GM.pg_t < 1.f) {
        GM.pg_t = MIN(1.f, GM.pg_t + dt / 0.45f);
        anim = true;
    }
    GM.scroll_target = CLAMP(GM.scroll_target, 0.f, gm_max_scroll());
    anim |= spring_step(&GM.scroll, &GM.scroll_v, GM.scroll_target, 24.f, dt);
    bool sb = A.page == PG_GAMES && gm_max_scroll() > 0 && A.mx >= content_x0();
    anim |= approach(&GM.sb_t, sb ? 1.f : 0.f, sb ? 20.f : 7.f, dt);
    return anim;
}

// A click anywhere on a game's row opens its dialog, but on the remove button.
static Hit gm_hit(f32 x, f32 y)
{
    if (y < S(HW_TOP) - S(6)) return (Hit){ HT_NONE, -1 };
    if (in_b(&GM.add, x, y)) return (Hit){ HT_GMADD, -1 };
    if (GM.npg) {
        if (in_b(&GM.pg_prev, x, y)) return (Hit){ HT_GMPAGE, GMP_PREV };
        if (in_b(&GM.pg_next, x, y)) return (Hit){ HT_GMPAGE, GMP_NEXT };
        for (int k = 0; k < GM.npg; k++)
            if (in_b(&GM.pg[k], x, y)) return (Hit){ HT_GMPAGE, k };
    }
    for (int i = gm_first(); i < gm_last(); i++) {
        if (GM.list[i].custom && in_b(&GM.del[i], x, y)) return (Hit){ HT_GMDEL, i };
        if (in_b(&GM.rows[i], x, y)) return (Hit){ HT_GMROW, i };
    }
    return (Hit){ HT_NONE, -1 };
}

static void gm_click(Hit h)
{
    switch (h.type) {
    case HT_GMADD: gm_add(); break;
    case HT_GMROW:
        if (h.idx >= 0 && h.idx < GM.n) gm_dlg_show(h.idx);
        break;
    case HT_GMFIX: gm_fix_toggle(h.idx); break;
    case HT_GMDONE:
    case HT_GMOUT: gm_dlg_close(); break;
    case HT_GMDEL:
        if (h.idx >= 0 && h.idx < GM.n) gm_remove(h.idx);
        break;
    case HT_GMPAGE: gm_set_page(h.idx == GMP_PREV ? GM.page - 1 : h.idx == GMP_NEXT ? GM.page + 1 : h.idx); break;
    }
}
