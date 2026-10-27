// The "Game fixes" page. One fix for now, "CPU 0 + High" (CPU and GPU priority): a card about the fix, then the games
// found on this PC (games.c) with their own icons, a switch for each, and "Add a game" for any other exe. Each switch
// applies on a click; the service or Process Lasso keeps it for later starts.

#define GM_ROW_H 68.f
#define GM_ICON_SLOT 96   // px, an icon's cell in the renderer's icon atlas; the atlas holds GM_MAX of them

enum { LS_NONE, LS_IDLE, LS_RUNNING };

static struct {
    bool ready;
    Game list[GM_MAX];
    int n;
    int lasso;             // LS_*
    f32 tog[GM_MAX], togv[GM_MAX];
    int seq[GM_MAX], pend;  // clicks per game, and jobs not done: the list is read again only once they are
    bool rescan;            // a game was added or removed: read the list when the jobs are done
    int ico[GM_MAX];       // the icon's size in the atlas: 0 not read yet, -1 none
    int ico_gen;           // the renderer device the icons were uploaded to
    WCHAR note[160];
    bool note_bad;
    f32 scroll, scroll_target, scroll_v, content_h, sb_t, hero_h;
    Box hero, list_card, add, rows[GM_MAX], sw[GM_MAX], del[GM_MAX];
    f32 sec_y;
} GM;

static void gm_read(void)
{
    if (GM.pend) return;
    WCHAR ini[MAX_PATH];
    GM.lasso = lasso_running() ? LS_RUNNING : lasso_ini(ini) ? LS_IDLE : LS_NONE;
    // Lasso put in or removed since: the service is needed or not any more
    if (lasso_sync()) games_svc_update(A.hwnd);
    GM.n = games_scan(GM.list);
    for (int i = 0; i < GM.n; i++) {
        GM.tog[i] = GM.list[i].on ? 1.f : 0.f;
        GM.ico[i] = 0;
    }
    GM.ready = true;
}

// The jobs: a switch, a game added, a game removed. Once none is left the service is put in or removed as the games
// now need it.
static bool gm_set_run(Job *j)
{
    if (game_set(&j->u.game, j->on)) return true;
    // half made: back to how it was, as the switch goes back
    game_set(&j->u.game, !j->on);
    return false;
}

static bool gm_forget_run(Job *j) { return game_forget(&j->u.game); }

static void gm_job_done(Job *j, bool ok)
{
    GM.pend--;
    if (!ok) {
        if (j->idx >= 0 && j->idx < GM.n && j->seq == GM.seq[j->idx] && !_wcsicmp(GM.list[j->idx].exe, j->u.game.exe)) GM.list[j->idx].on = !j->on;
        toast_show(TR("Не удалось применить фикс к игре: подробности в log.txt", "Couldn't apply the fix to the game: details in log.txt"));
    }
    if (GM.pend) return;
    games_svc_update(A.hwnd);
    if (GM.rescan) {
        GM.rescan = false;
        GM.ready = false;
    }
}

static void gm_post(int i, const Game *g, bool on, bool (*run)(Job *))
{
    Job j = { run, gm_job_done, i, i >= 0 ? ++GM.seq[i] : 0, on };
    j.u.game = *g;
    GM.pend++;
    job_post(A.hwnd, &j);
}

static void gm_toggle(int i)
{
    Game *g = &GM.list[i];
    g->on = !g->on;
    GM.note[0] = 0;
    gm_post(i, g, g->on, gm_set_run);
}

// "Add a game": any exe; a known one keeps its name.
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
            if (!GM.list[i].on) gm_toggle(i);
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
    gm_post(-1, &g, true, gm_set_run);
}

static void gm_remove(int i)
{
    GM.rescan = true;
    gm_post(-1, &GM.list[i], false, gm_forget_run);
    // gone from the list at once; the list is read again when the job is done
    memmove(&GM.list[i], &GM.list[i + 1], sizeof(Game) * (size_t)(GM.n - i - 1));
    memmove(&GM.tog[i], &GM.tog[i + 1], sizeof(f32) * (size_t)(GM.n - i - 1));
    memmove(&GM.togv[i], &GM.togv[i + 1], sizeof(f32) * (size_t)(GM.n - i - 1));
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
          TR("Служба MixBooster не запустилась: CPU 0 не будет сниматься при следующих запусках игр. Подробности в log.txt",
             "The MixBooster service didn't start: CPU 0 won't be taken off on the next game starts. Details in log.txt"));
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

// Loads the icons not read yet into the atlas, at the size they are drawn (read again when the DPI or the device
// changes).
static void gm_icons(int px)
{
    px = MIN(px, GM_ICON_SLOT);
    if (GM.ico_gen != g_r_gen) {
        GM.ico_gen = g_r_gen;
        for (int i = 0; i < GM.n; i++) GM.ico[i] = 0;
    }
    static u32 buf[GM_ICON_SLOT * GM_ICON_SLOT];
    int per = ICON_ATLAS / GM_ICON_SLOT;
    for (int i = 0; i < GM.n && i < per * per; i++) {
        if (GM.ico[i] == px || GM.ico[i] < 0) continue;
        if (!GM.list[i].path[0] || !exe_icon(GM.list[i].path, px, buf)) {
            GM.ico[i] = -1;
            continue;
        }
        r_upload_icon((i % per) * GM_ICON_SLOT, (i / per) * GM_ICON_SLOT, px, buf);
        GM.ico[i] = px;
    }
}

static void gm_draw_icon(int i, f32 x, f32 y, f32 sz)
{
    const Theme *t = &A.th;
    int per = ICON_ATLAS / GM_ICON_SLOT;
    if (GM.ico[i] > 0) {
        f32 u = (f32)((i % per) * GM_ICON_SLOT) / ICON_ATLAS, v = (f32)((i / per) * GM_ICON_SLOT) / ICON_ATLAS, d = (f32)GM.ico[i] / ICON_ATLAS;
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
    return TR("Игра работает на всех потоках, кроме CPU 0, где Windows и драйверы обрабатывают большую часть прерываний, с высоким приоритетом процессора и видеокарты. Держится при каждом запуске игры.",
              "The game runs on every CPU but CPU 0, where Windows and the drivers handle most interrupts, with High CPU and GPU priority. Kept on every start of the game.");
}

// The callouts under the explanation: a warning for small CPUs, and where Process Lasso stands.
static int gm_notes(const WCHAR **txt, u32 *col, u32 *glyph)
{
    const Theme *t = &A.th;
    int n = 0;
    if (A.cpu.count <= 4) {
        txt[n] = TR("У процессора 4 потока или меньше: без CPU 0 игре останется 3 и меньше, FPS может упасть. Включайте, только если проверили разницу",
                    "This CPU has 4 threads or fewer: without CPU 0 a game is left with 3 or fewer and can lose FPS. Turn it on only after checking the difference");
        col[n] = t->warn;
        glyph[n++] = 0xE7BA;
    }
    if (GM.lasso == LS_RUNNING) {
        txt[n] = TR("Process Lasso запущен: включённые игры держит он, служба MixBooster для них не нужна",
                    "Process Lasso is running: it keeps the games turned on, the MixBooster service isn't needed for them");
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
    for (int i = 0; i < GM.n; i++) n += GM.list[i].on;
    return n;
}

static void gm_layout(void)
{
    f32 x0 = content_x0(), cw = content_x1() - x0, y = S(HW_TOP) - floorf(GM.scroll + 0.5f);
    // the fix's card grows with the explanation's lines and the callouts under it
    f32 tx = x0 + S(24) + S(44) + S(16), tw = x0 + cw - S(24) - S(150) - tx;
    int lines = text_wrap(FONT_TEXT, SC(13), tx, 0, tw, S(18), gm_desc(), 0, false);
    const WCHAR *nt[2];
    u32 nc[2], ng[2];
    int notes = gm_notes(nt, nc, ng);
    GM.hero_h = S(60) + S(18) * (f32)lines + S(20) + (notes ? S(44) * (f32)notes : 0);
    GM.hero = (Box){ x0, y, cw, GM.hero_h };

    // "Games", the count and "Add a game" over the list
    GM.sec_y = y + GM.hero_h + S(22);
    const WCHAR *lbl = TR("Добавить игру", "Add a game");
    f32 bw = floorf(text_width(FONT_TEXT_SEMIBOLD, SC(13), lbl, -1) + S(54));
    GM.add = (Box){ x0 + cw - bw, GM.sec_y, bw, S(34) };

    f32 ly = GM.sec_y + S(46);
    for (int i = 0; i < GM.n; i++) {
        GM.rows[i] = (Box){ x0 + S(6), ly + S(6) + S(GM_ROW_H) * (f32)i, cw - S(12), S(GM_ROW_H) };
        GM.sw[i] = (Box){ x0 + cw - S(22) - S(52), GM.rows[i].y + (S(GM_ROW_H) - S(32)) * 0.5f, S(52), S(32) };
        GM.del[i] = GM.list[i].custom ? (Box){ GM.sw[i].x - S(10) - S(32), GM.rows[i].y + (S(GM_ROW_H) - S(32)) * 0.5f, S(32), S(32) } : (Box){ 0 };
    }
    GM.list_card = (Box){ x0, ly, cw, GM.n ? S(GM_ROW_H) * (f32)GM.n + S(12) : S(150) };
    GM.content_h = GM.list_card.y + GM.list_card.h - y + (GM.note[0] ? S(48) : S(16));
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
                  TR("CPU 0 + высокий приоритет CPU и GPU", "CPU 0 + High CPU and GPU priority"), -1, t->text, false);
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
    text_right(FONT_TEXT, SC(12), right, ny + S(26), TR("игр с фиксом", "games with the fix"), t->faint);

    const WCHAR *nt[2];
    u32 nc[2], ng[2];
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
    for (int i = 0; i < GM.n; i++) {
        const Game *g = &GM.list[i];
        Box *r = &GM.rows[i];
        bool hov = (A.hover.type == HT_GMTOG || A.hover.type == HT_GMDEL) && A.hover.idx == i;
        f32 rh = hover_amt(r, hov), nh = i + 1 < GM.n ? hover_amt(&GM.rows[i + 1], (A.hover.type == HT_GMTOG || A.hover.type == HT_GMDEL) && A.hover.idx == i + 1) : 0.f;
        if (rh > 0) r_rect(r->x, r->y + S(2), r->w, r->h - S(4), color_alpha(c->nav_hover, rh), S(8));
        // the line under a row gives way to the hover of either row next to it
        if (i + 1 < GM.n) r_rect(r->x + S(16), r->y + r->h - 1, r->w - S(32), 1, color_alpha(t->sep, 0.7f * (1.f - MAX(rh, nh))), 0);
        f32 ry = r->y + r->h * 0.5f, ix = r->x + S(16), nx = ix + isz + S(14);
        f32 right = (g->custom ? GM.del[i].x : GM.sw[i].x) - S(16);
        gm_draw_icon(i, ix, ry - isz * 0.5f, isz);

        // the name, and what is true of it now
        f32 ny = ry - S(11);
        f32 nw = text_draw_fit(FONT_TEXT_SEMIBOLD, SC(14), nx, baseline_for(FONT_TEXT_SEMIBOLD, SC(14), ny), right - nx - S(170), g->name, -1, t->text, false);
        f32 px = nx + nw + S(10);
        if (g->running) {
            const WCHAR *lbl = TR("запущена", "running");
            f32 pw = text_width(FONT_TEXT_SEMIBOLD, SC(11), lbl, -1) + S(22);
            u32 pc = g->on ? t->good : t->dim;
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
        draw_switch(&GM.sw[i], GM.tog[i], true, A.hover.type == HT_GMTOG && A.hover.idx == i);
    }
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
        f32 ny = GM.list_card.y + GM.list_card.h + S(24);
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

static bool gm_animate(f32 dt)
{
    bool anim = false;
    for (int i = 0; i < GM.n; i++) anim |= spring(&GM.tog[i], &GM.togv[i], GM.list[i].on ? 1.f : 0.f, 0.34f, 0.7f, 0.002f, dt);
    GM.scroll_target = CLAMP(GM.scroll_target, 0.f, gm_max_scroll());
    anim |= spring_step(&GM.scroll, &GM.scroll_v, GM.scroll_target, 24.f, dt);
    bool sb = A.page == PG_GAMES && gm_max_scroll() > 0 && A.mx >= content_x0();
    anim |= approach(&GM.sb_t, sb ? 1.f : 0.f, sb ? 20.f : 7.f, dt);
    return anim;
}

// A click anywhere on a game's row flips its switch, but on the remove button.
static Hit gm_hit(f32 x, f32 y)
{
    if (y < S(HW_TOP) - S(6)) return (Hit){ HT_NONE, -1 };
    if (in_b(&GM.add, x, y)) return (Hit){ HT_GMADD, -1 };
    for (int i = 0; i < GM.n; i++) {
        if (GM.list[i].custom && in_b(&GM.del[i], x, y)) return (Hit){ HT_GMDEL, i };
        if (in_b(&GM.rows[i], x, y)) return (Hit){ HT_GMTOG, i };
    }
    return (Hit){ HT_NONE, -1 };
}

static void gm_click(Hit h)
{
    switch (h.type) {
    case HT_GMADD: gm_add(); break;
    case HT_GMTOG:
        if (h.idx >= 0 && h.idx < GM.n) gm_toggle(h.idx);
        break;
    case HT_GMDEL:
        if (h.idx >= 0 && h.idx < GM.n) gm_remove(h.idx);
        break;
    }
}
