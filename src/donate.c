// "Support the author", the same panel as MixLauncher's: the DonationAlerts QR, a link and crypto wallets to copy.
// Opened from the button at the bottom of the sidebar.

#define TIMER_FLASH 2
enum { DON_OUT = -1, DON_PANEL = -2, DON_CLOSE = -3, DON_LINK = -4 };

static struct {
    bool open, closing;
    f32 t, scroll, scroll_target, scroll_v, content_h, view_h;
    f32 panel[4], close[4], link[4], copy[8][4];
    f32 hov[10], cp[8], sb_t, sb_prev;
    int flash;
    f64 flash_until, open_time, sb_seen;
} D;

static void clipboard_set(const WCHAR *s, int n)
{
    if (n < 0) n = wlen(s);
    if (!OpenClipboard(A.hwnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t)(n + 1) * sizeof(WCHAR));
    if (g) {
        WCHAR *p = (WCHAR *)GlobalLock(g);
        memcpy(p, s, (size_t)n * sizeof(WCHAR));
        p[n] = 0;
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

// The app runs as administrator; going through Explorer opens the browser as the user.
static void open_url(const WCHAR *url) { ShellExecuteW(NULL, L"open", L"explorer.exe", url, NULL, SW_SHOWNORMAL); }

// The DonationAlerts QR, drawn from its modules.
static const u64 k_don_qr[49] = {
    0x1fd2bd08f807full, 0x105fd5bfcf641ull, 0x1758b389c715dull, 0x174b0c6a5d95dull,
    0x17428eff8915dull, 0x1047c0476a541ull, 0x1fd555555557full, 0x00067d45dcb00ull,
    0x07d588fdec87dull, 0x0beb0f323b796ull, 0x1c1ceb57e46dbull, 0x098615aa0b6a3ull,
    0x0e938814dadc0ull, 0x08ca0d9e00bb5ull, 0x1e04e37e96962ull, 0x11c48c0a50182ull,
    0x06d91fd32f652ull, 0x08e138129db34ull, 0x138646f924be5ull, 0x09fa9f409fd1dull,
    0x1c57269d5c759ull, 0x09e11648f8f35ull, 0x1bf5eaff4d9f1ull, 0x091255c66e717ull,
    0x0d53ecd76af58ull, 0x07199e46f9518ull, 0x11f5607da71f8ull, 0x1822378a949a6ull,
    0x07f9e8f83524dull, 0x026ab56bb39b7ull, 0x1d4cdab15d2f6ull, 0x1a3095c5a6824ull,
    0x1d29068589ff0ull, 0x08a1855811c34ull, 0x134fe196ed376ull, 0x0196f57a2a69full,
    0x152d088fa905cull, 0x0abb17d2a6091ull, 0x16f5e20ce0be2ull, 0x10167df8b510eull,
    0x0fffce7de1ec7ull, 0x09130f44ef700ull, 0x1d55c854b3e7full, 0x1912154695541ull,
    0x17f1e87c8155dull, 0x13f11c840eb5dull, 0x00effa16dd55dull, 0x106a0c786fa41ull,
    0x1e2984a961b7full,
};
#define DON_QR_N 49
#define DON_LINK_URL L"https://dalink.to/mel1xy"

static const struct { const WCHAR *net, *addr; } k_wallets[8] = {
    { L"TRC20", L"TD1yryydTBLre9ATmhhMjcdBxUZSuLhcYd" },
    { L"ERC20", L"0x10B2945C3222F9Fb27CDcDBbEfF298961Db10e96" },
    { L"BEP20", L"0x10B2945C3222F9Fb27CDcDBbEfF298961Db10e96" },
    { L"ERC20", L"0x10B2945C3222F9Fb27CDcDBbEfF298961Db10e96" },
    { L"Bitcoin", L"bc1qrhtwhv75jrh8fz6y8qc38evkxrvg7ge0nu4z3w" },
    { L"Litecoin", L"LVtmyVY3YbMMKzjxhkVu5bgNFY5T7hhbf2" },
    { L"ERC20", L"0x10B2945C3222F9Fb27CDcDBbEfF298961Db10e96" },
    { L"TRC20", L"TD1yryydTBLre9ATmhhMjcdBxUZSuLhcYd" },
};

enum { LOGO_USDT, LOGO_ETH, LOGO_BTC, LOGO_LTC, LOGO_USDC, LOGO_TRX };

// One row per coin; USDT has a chip per network, the others copy their single wallet.
static const struct { const WCHAR *name; u32 top, bottom; int logo, wallet, nets; } k_coins[6] = {
    { L"USDT", RGBA(84, 196, 160, 255), RGBA(22, 126, 98, 255), LOGO_USDT, 0, 3 },
    { L"ETH", RGBA(146, 166, 252, 255), RGBA(76, 92, 212, 255), LOGO_ETH, 3, 1 },
    { L"BTC", RGBA(255, 190, 80, 255), RGBA(238, 118, 8, 255), LOGO_BTC, 4, 1 },
    { L"Litecoin", RGBA(112, 152, 222, 255), RGBA(38, 76, 146, 255), LOGO_LTC, 5, 1 },
    { L"USDC", RGBA(86, 164, 242, 255), RGBA(28, 92, 182, 255), LOGO_USDC, 6, 1 },
    { L"TRON", RGBA(255, 86, 104, 255), RGBA(196, 0, 32, 255), LOGO_TRX, 7, 1 },
};

// The logos no font has (Tether, Ethereum, TRON) as shapes in a unit box, rasterized into the glyph atlas.
static bool pt_in_poly(const f32 *p, int n, f32 x, f32 y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        f32 xi = p[i * 2], yi = p[i * 2 + 1], xj = p[j * 2], yj = p[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

static bool near_seg(f32 ax, f32 ay, f32 bx, f32 by, f32 x, f32 y, f32 half)
{
    f32 dx = bx - ax, dy = by - ay, t = CLAMP(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
    f32 ex = ax + dx * t - x, ey = ay + dy * t - y;
    return ex * ex + ey * ey <= half * half;
}

static bool logo_inside(int id, f32 x, f32 y)
{
    switch (id) {
    case LOGO_USDT: {
        if (x >= 0.10f && x <= 0.90f && y >= 0.14f && y <= 0.33f) return true;
        if (x >= 0.40f && x <= 0.60f && y >= 0.33f && y <= 0.90f) return true;
        f32 ox = (x - 0.5f) / 0.43f, oy = (y - 0.50f) / 0.13f, ix = (x - 0.5f) / 0.33f, iy = (y - 0.50f) / 0.065f;
        return ox * ox + oy * oy <= 1.f && ix * ix + iy * iy >= 1.f && y >= 0.40f;
    }
    case LOGO_ETH: {
        static const f32 top[] = { 0.5f, 0.0f, 0.82f, 0.545f, 0.5f, 0.735f, 0.18f, 0.545f };
        static const f32 bot[] = { 0.5f, 0.80f, 0.82f, 0.605f, 0.5f, 1.0f, 0.18f, 0.605f };
        return pt_in_poly(top, 4, x, y) || pt_in_poly(bot, 4, x, y);
    }
    case LOGO_TRX: {
        const f32 ax = 0.06f, ay = 0.10f, bx = 0.96f, by = 0.36f, cx = 0.44f, cy = 0.96f, dx = 0.66f, dy = 0.46f, w = 0.055f;
        return near_seg(ax, ay, bx, by, x, y, w) || near_seg(bx, by, cx, cy, x, y, w) || near_seg(cx, cy, ax, ay, x, y, w) ||
               near_seg(ax, ay, dx, dy, x, y, w) || near_seg(dx, dy, cx, cy, x, y, w) || near_seg(dx, dy, bx, by, x, y, w * 0.8f);
    }
    }
    return false;
}

static struct { int id, px; u32 gen; int x, y; } g_logo_cache[16];

static bool logo_region(int id, int px, int *ox, int *oy)
{
    int free_slot = -1;
    for (int i = 0; i < countof(g_logo_cache); i++) {
        if (g_logo_cache[i].px == px && g_logo_cache[i].id == id && g_logo_cache[i].gen == g_atlas_gen) {
            *ox = g_logo_cache[i].x;
            *oy = g_logo_cache[i].y;
            return true;
        }
        if (free_slot < 0 && (!g_logo_cache[i].px || g_logo_cache[i].gen != g_atlas_gen)) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    if (!R.glyph_cpu || !atlas_alloc(px, px, ox, oy)) {
        F.overflow = true;
        return false;
    }
    const int ss = 4;
    for (int yy = 0; yy < px; yy++)
        for (int xx = 0; xx < px; xx++) {
            int hit = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++)
                    hit += logo_inside(id, ((f32)xx + ((f32)sx + 0.5f) / ss) / (f32)px, ((f32)yy + ((f32)sy + 0.5f) / ss) / (f32)px);
            R.glyph_cpu[(size_t)(*oy + yy) * GLYPH_ATLAS + *ox + xx] = (u8)(hit * 255 / (ss * ss));
        }
    r_glyph_dirty(*ox, *oy, px, px);
    g_logo_cache[free_slot].id = id;
    g_logo_cache[free_slot].px = px;
    g_logo_cache[free_slot].gen = g_atlas_gen;
    g_logo_cache[free_slot].x = *ox;
    g_logo_cache[free_slot].y = *oy;
    return true;
}

// Squircle with the coin's vertical gradient and its white logo.
static void draw_coin_icon(int i, f32 x, f32 y, f32 sz)
{
    x = floorf(x);
    y = floorf(y);
    f32 rad = floorf(sz * 0.3f + 0.5f), saved[4];
    memcpy(saved, R.clip, sizeof saved);
    for (int r = 0; r < (int)sz; r++) {
        f32 y0 = MAX(y + (f32)r, saved[1]), y1 = MIN(y + (f32)r + 1.f, saved[3]);
        if (y1 <= y0) continue;
        r_set_clip(saved[0], y0, saved[2], y1);
        r_rect(x, y, sz, sz, color_mix(k_coins[i].top, k_coins[i].bottom, ((f32)r + 0.5f) / sz), rad);
    }
    memcpy(R.clip, saved, sizeof saved);
    u32 white = RGBA(255, 255, 255, 255);
    f32 cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    int logo = k_coins[i].logo;
    if (logo == LOGO_USDT || logo == LOGO_ETH || logo == LOGO_TRX) {
        int px = (int)(sz * (logo == LOGO_ETH ? 0.62f : 0.58f) + 0.5f), ox, oy;
        if (logo_region(logo, px, &ox, &oy)) {
            const f32 inv = 1.0f / GLYPH_ATLAS;
            f32 lx = floorf(cx - (f32)px * 0.5f + 0.5f), ly = floorf(cy - (f32)px * 0.5f + 0.5f);
            r_glyph(lx, ly, (f32)px, (f32)px, (f32)ox * inv, (f32)oy * inv, (f32)(ox + px) * inv, (f32)(oy + px) * inv, white);
        }
        return;
    }
    if (logo == LOGO_USDC) {
        f32 ring = floorf(sz * 0.62f + 0.5f);
        r_rect_ex(floorf(cx - ring * 0.5f + 0.5f), floorf(cy - ring * 0.5f + 0.5f), ring, ring, white, ring * 0.5f, MAX(1.f, floorf(sz * 0.06f + 0.5f)), 0);
    }
    const WCHAR *sym = logo == LOGO_BTC ? L"\x20BF" : logo == LOGO_LTC ? L"\x0141" : L"$";
    f32 fs = sz * (logo == LOGO_USDC ? 0.40f : 0.56f), w = text_width(FONT_TEXT_SEMIBOLD, fs, sym, -1);
    text_draw(FONT_TEXT_SEMIBOLD, fs, floorf(cx - w * 0.5f + 0.5f), baseline_for(FONT_TEXT_SEMIBOLD, fs, cy), sym, -1, white);
}

#define DON_HOV_LINK 8
#define DON_HOV_CLOSE 9

static void don_open(void)
{
    D.open = true;
    D.closing = false;
    D.t = (!g_reduced_motion) ? 0.f : 1.f;
    D.open_time = time_now();
    D.scroll = D.scroll_target = D.scroll_v = 0;
    D.sb_t = D.sb_prev = 0;
    D.sb_seen = 0;
    D.flash = -1;
    memset(D.hov, 0, sizeof D.hov);
    memset(D.cp, 0, sizeof D.cp);
    invalidate();
}

static void don_close(void)
{
    if (!D.open) return;
    if ((!g_reduced_motion)) D.closing = true;
    else D.open = false;
    invalidate();
}

static bool don_active(void) { return D.open && !D.closing; }

// Confetti in the canvas-confetti manner: fired from both top corners, braked, then drifting down with wobble and spin.
typedef struct Confetti { f32 x, y, vx, vy, fall, sway, size, rot, rot_v, wob, wob_v, age, life; u32 color; } Confetti;
static Confetti g_conf[320];
static int g_nconf;
static u32 g_conf_seed = 0x9E3779B9u;

static f32 conf_rand(void)
{
    g_conf_seed = g_conf_seed * 1664525u + 1013904223u;
    return (f32)(g_conf_seed >> 8) / 16777216.f;
}

static void confetti_burst(void)
{
    if (!(!g_reduced_motion)) return;
    static const u32 colors[] = { RGBA(255, 94, 58, 255), RGBA(255, 184, 0, 255), RGBA(46, 196, 182, 255), RGBA(58, 134, 255, 255),
                                  RGBA(255, 77, 141, 255), RGBA(150, 92, 255, 255) };
    g_conf_seed ^= (u32)(time_now() * 1000.0);
    for (int i = 0; i < 220; i++) {
        if (g_nconf == countof(g_conf)) memmove(g_conf, g_conf + 1, sizeof(Confetti) * (size_t)--g_nconf);
        Confetti *c = &g_conf[g_nconf++];
        bool left = i & 1;
        // up to 50 degrees above the horizon, toward the middle; varied speeds spread them over the window
        f32 r = conf_rand(), a = (-8.f + conf_rand() * 58.f) * 0.0174533f, sp = SC(500.f + (1.f - r * r) * 2600.f);
        c->x = left ? 0.f : (f32)A.W;
        c->y = SC(150.f + conf_rand() * 20.f);
        c->vx = cosf(a) * sp * (left ? 1.f : -1.f);
        c->vy = -sinf(a) * sp;
        c->fall = SC(130.f + conf_rand() * 140.f);
        c->sway = SC(5.f + conf_rand() * 12.f);
        c->size = SC(6.f + conf_rand() * 4.f);
        c->rot = conf_rand() * 6.283f;
        c->rot_v = (conf_rand() - 0.5f) * 10.f;
        c->wob = conf_rand() * 6.283f;
        c->wob_v = 4.f + conf_rand() * 4.f;
        c->age = -conf_rand() * 0.08f;
        c->life = 3.2f + conf_rand() * 1.6f;
        c->color = colors[(int)(conf_rand() * countof(colors)) % countof(colors)];
    }
    invalidate();
}

static bool confetti_animate(f32 dt)
{
    int n = 0;
    f32 k = expf(-6.3f * dt);  // canvas-confetti: velocity *= 0.9 per 60 Hz frame
    for (int i = 0; i < g_nconf; i++) {
        Confetti c = g_conf[i];
        c.age += dt;
        if (c.age >= c.life || c.y > (f32)A.H + SC(20)) continue;
        if (c.age > 0) {
            c.vx *= k;
            c.vy *= k;
            c.x += c.vx * dt;
            c.y += (c.vy + c.fall) * dt;
            c.wob += c.wob_v * dt;
            c.rot += c.rot_v * dt;
        }
        g_conf[n++] = c;
    }
    g_nconf = n;
    return n > 0;
}

static void confetti_draw(void)
{
    for (int i = 0; i < g_nconf; i++) {
        Confetti *c = &g_conf[i];
        if (c->age < 0) continue;
        f32 fade = CLAMP((c->life - c->age) / 0.6f, 0.f, 1.f), tumble = cosf(c->wob);
        f32 x = c->x + sinf(c->wob) * c->sway;
        u32 col = color_mix(c->color, RGBA(0, 0, 0, 255), (1.f - fabsf(tumble)) * 0.3f);
        r_rect_rot(x, c->y, c->size, c->size, c->rot, MAX(0.12f, fabsf(tumble)), color_alpha(col, fade), SC(1.f));
    }
}

static void don_copy(int w)
{
    clipboard_set(k_wallets[w].addr, -1);
    D.flash = w;
    D.flash_until = time_now() + 1.4;
    SetTimer(A.hwnd, TIMER_FLASH, 1500, NULL);
    invalidate();
}

static void box_set(f32 *b, f32 x, f32 y, f32 w, f32 h)
{
    b[0] = x;
    b[1] = y;
    b[2] = x + w;
    b[3] = y + h;
}

#define DON_STAGGER 0.035f
#define DON_ITEM_DUR 0.32f
#define DON_ITEMS 11

// Entrance of the k-th block of the modal (header, QR, link, rows...), staggered after opening.
static f32 don_item(int k)
{
    if (!(!g_reduced_motion)) return 1.f;
    f32 t = (f32)(time_now() - D.open_time) - DON_STAGGER * (f32)k - 0.05f;
    return ease_out_cubic(t / DON_ITEM_DUR);
}

static bool don_animate(f32 dt)
{
    if (!D.open) return false;
    bool anim = false;
    if (D.closing) {
        f32 d = 0.13f;
        D.t = d > 0.f ? D.t - dt / d : 0.f;
        if (D.t <= 0.f) {
            D.t = 0;
            D.open = D.closing = false;
        }
        return true;
    }
    if (D.t < 1.f) {
        f32 d = 0.24f;
        D.t = d > 0.f ? MIN(1.f, D.t + dt / d) : 1.f;
        anim = true;
    }
    if (time_now() - D.open_time < (DON_ITEM_DUR + DON_STAGGER * DON_ITEMS + 0.05f)) anim = true;
    f32 ms = MAX(0.f, D.content_h - D.view_h);
    D.scroll_target = CLAMP(D.scroll_target, 0.f, ms);
    anim |= spring_step(&D.scroll, &D.scroll_v, D.scroll_target, 24.f, dt);
    f64 now = time_now();
    if (D.scroll != D.sb_prev) D.sb_seen = now;
    D.sb_prev = D.scroll;
    bool recent = now - D.sb_seen < 0.9;
    anim |= approach(&D.sb_t, recent && ms > 0 ? 1.f : 0.f, recent ? 20.f : 7.f, dt);
    if (recent && ms > 0) anim = true;
    bool flash_on = D.flash >= 0 && now < D.flash_until;
    for (int i = 0; i < 8; i++) anim |= approach(&D.cp[i], flash_on && D.flash == i ? 1.f : 0.f, 16.f, dt);
    for (int i = 0; i < 10; i++) {
        int want = i == DON_HOV_LINK ? DON_LINK : i == DON_HOV_CLOSE ? DON_CLOSE : i;
        anim |= approach(&D.hov[i], A.hover.type == HT_DON && A.hover.idx == want ? 1.f : 0.f, 20.f, dt);
    }
    return anim;
}

static void draw_donate(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    f32 e = D.closing ? D.t * D.t : ease_out_quart(D.t);
    R.opacity = e;
    r_rect(0, 0, (f32)A.W, (f32)A.H, c->overlay, 0);
    f32 pw = S(380), m = S(16);
    f32 qr_sz = S(184), row_h = S(54), row_gap = S(8), link_h = S(32);
    qr_sz = floorf(qr_sz / (DON_QR_N + 7)) * (DON_QR_N + 7);
    f32 content = S(20) + S(24) + S(16) + qr_sz + S(16) + link_h + S(22) + S(20) + (row_h + row_gap) * 6.f - row_gap + S(18);
    D.content_h = content;
    f32 ph = MIN(content, (f32)A.H - m * 2);
    D.view_h = ph;
    f32 ms = MAX(0.f, content - ph);
    D.scroll = CLAMP(D.scroll, 0.f, ms);
    f32 px = floorf(((f32)A.W - pw) * 0.5f), py = floorf(((f32)A.H - ph) * 0.5f + (1.f - e) * S(D.closing ? 8.f : 22.f));
    box_set(D.panel, px, py, pw, ph);
    r_rect_ex(px, py + S(14), pw, ph, c->shadow, S(16), 0, S(30));
    r_rect(px, py, pw, ph, t->dark ? RGBA(36, 36, 39, 255) : RGBA(252, 252, 253, 255), S(16));
    r_rect_ex(px, py, pw, ph, c->pop_border, S(16), 1.f, 0);

    r_set_clip(px, py + 1, px + pw, py + ph - 1);
    f32 x0 = px + S(20), x1 = px + pw - S(20), y = py + S(20) - floorf(D.scroll + 0.5f);
    int k = 0;
    // Closing: content first, then the empty panel (a fading white QR reads as a gray flash).
#define DON_BLOCK(dy_var)                                                  \
    f32 ek_ = D.closing ? 1.f : don_item(k++);                        \
    R.opacity = D.closing ? CLAMP(e * 2.f - 1.f, 0.f, 1.f) : e * ek_; \
    f32 dy_var = floorf((1.f - ek_) * S(10) + 0.5f)

    {
        DON_BLOCK(dy);
        f32 hy = y + dy;
        text_draw(FONT_TEXT_SEMIBOLD, SC(17), x0, baseline_for(FONT_TEXT_SEMIBOLD, SC(17), hy + S(12)),
                  TR("Поддержать автора", "Support the author"), -1, t->text);
        f32 cb = S(30), hv = D.hov[DON_HOV_CLOSE];
        box_set(D.close, x1 - cb + S(6), y - S(3), cb, cb);
        if (hv > 0) r_rect(D.close[0], D.close[1] + dy, cb, cb, color_alpha(c->ctl_hover, hv), S(8));
        text_draw_icon(0xE8BB, SC(10), D.close[0] + cb * 0.5f, D.close[1] + dy + cb * 0.5f, color_mix(t->dim, t->text, hv));
    }
    y += S(24) + S(16);

    // QR: always dark on white
    {
        DON_BLOCK(dy);
        f32 mod = floorf(qr_sz / (DON_QR_N + 7)), tile = mod * (DON_QR_N + 7), qo = floorf((tile - mod * DON_QR_N) * 0.5f);
        f32 qx = floorf(px + (pw - tile) * 0.5f), qy = y + floorf((qr_sz - tile) * 0.5f) + dy;
        r_rect(qx, qy, tile, tile, RGBA(255, 255, 255, 255), S(10));
        if (!t->dark) r_rect_ex(qx, qy, tile, tile, RGBA(0, 0, 0, 20), S(10), 1.f, 0);
        for (int r = 0; r < DON_QR_N; r++) {
            u64 bits = k_don_qr[r];
            for (int col = 0; col < DON_QR_N;) {
                if (!((bits >> col) & 1)) {
                    col++;
                    continue;
                }
                int run = col;
                while (run < DON_QR_N && ((bits >> run) & 1)) run++;
                r_rect(qx + qo + mod * (f32)col, qy + qo + mod * (f32)r, mod * (f32)(run - col), mod, RGBA(17, 17, 19, 255), 0);
                col = run;
            }
        }
    }
    y += qr_sz + S(16);

    {
        DON_BLOCK(dy);
        f32 hv = D.hov[DON_HOV_LINK], fs = SC(13.5f), cy = y + dy + link_h * 0.5f;
        const WCHAR *lbl = L"DonationAlerts";
        f32 lw = text_width(FONT_TEXT_SEMIBOLD, fs, lbl, -1), bw = floorf(S(16) + lw + S(8) + S(12) + S(14));
        f32 bx = floorf(px + (pw - bw) * 0.5f);
        box_set(D.link, bx, y, bw, link_h);
        r_rect(bx, y + dy, bw, link_h, color_mix(c->ctl, c->ctl_hover, hv), link_h * 0.5f);
        text_draw(FONT_TEXT_SEMIBOLD, fs, bx + S(16), baseline_for(FONT_TEXT_SEMIBOLD, fs, cy), lbl, -1, t->text);
        text_draw_icon(0xE8A7, SC(11), bx + S(16) + lw + S(8) + S(6), cy, color_mix(t->dim, t->text, hv));
    }
    y += link_h + S(22);

    {
        DON_BLOCK(dy);
        text_draw(FONT_TEXT_SEMIBOLD, SC(12.5f), x0 + S(2), baseline_for(FONT_TEXT_SEMIBOLD, SC(12.5f), y + dy + S(6)),
                  TR("Криптовалюта", "Crypto"), -1, color_mix(t->dim, t->text, 0.35f));
    }
    y += S(20);

    u32 ok_col = t->dark ? RGBA(74, 210, 130, 255) : RGBA(24, 140, 72, 255);
    for (int i = 0; i < 6; i++) {
        DON_BLOCK(dy);
        f32 base_op = R.opacity, ry = y + dy, cy = ry + row_h * 0.5f;
        int w0 = k_coins[i].wallet, nets = k_coins[i].nets;
        r_rect(x0, ry, x1 - x0, row_h, c->ctl, S(12));
        if (nets == 1) box_set(D.copy[w0], x0, y, x1 - x0, row_h);
        f32 isz = S(34), ix = x0 + S(10);
        draw_coin_icon(i, ix, cy - isz * 0.5f, isz);
        f32 tx = ix + isz + S(12), nfs = SC(14.5f), afs = SC(12);
        text_draw(FONT_TEXT_SEMIBOLD, nfs, tx, baseline_for(FONT_TEXT_SEMIBOLD, nfs, cy - S(9)), k_coins[i].name, -1, t->text);
        if (nets > 1) {
            text_draw(FONT_TEXT, afs, tx, baseline_for(FONT_TEXT, afs, cy + S(10)), L"Tether", -1, t->faint);
            f32 xr = x1 - S(10), chh = S(30), cfs = SC(12.5f);
            for (int n = nets - 1; n >= 0; n--) {
                int wi = w0 + n;
                f32 hv = D.hov[wi], cp = D.cp[wi];
                const WCHAR *lbl = k_wallets[wi].net;
                f32 lw = text_width(FONT_TEXT, cfs, lbl, -1), cw = floorf(S(10) + lw + S(6) + S(10) + S(10));
                f32 chx = xr - cw, chy = floorf(cy - chh * 0.5f);
                box_set(D.copy[wi], chx, chy - dy, cw, chh);
                if (hv > 0) r_rect(chx, chy, cw, chh, color_alpha(c->ctl_hover, hv), S(8));
                if (cp > 0) r_rect(chx, chy, cw, chh, color_alpha(ok_col, 0.16f * cp), S(8));
                u32 col = color_mix(color_mix(t->dim, t->text, hv), ok_col, cp);
                text_draw(FONT_TEXT, cfs, chx + S(10), baseline_for(FONT_TEXT, cfs, cy), lbl, -1, col);
                f32 gx = chx + cw - S(10) - S(5), slide = S(7);
                r_set_clip(chx, MAX(chy, py + 1), chx + cw, MIN(chy + chh, py + ph - 1));
                R.opacity = base_op * (1.f - cp);
                text_draw_icon(0xE8A7, SC(9), gx, cy - cp * slide, col);
                R.opacity = base_op * cp;
                text_draw_icon(0xE73E, SC(10), gx, cy + (1.f - cp) * slide, ok_col);
                R.opacity = base_op;
                r_set_clip(px, py + 1, px + pw, py + ph - 1);
                xr = chx - S(2);
            }
        } else {
            WCHAR shortaddr[32];
            const WCHAR *a = k_wallets[w0].addr;
            int n = wlen(a);
            wfmt(shortaddr, countof(shortaddr), L"%ls  \x00B7  %.6ls\x2026%ls", k_wallets[w0].net, a, a + n - 4);
            text_draw(FONT_TEXT, afs, tx, baseline_for(FONT_TEXT, afs, cy + S(10)), shortaddr, -1, t->faint);
            f32 hv = D.hov[w0], cp = D.cp[w0];
            const WCHAR *l0 = TR("Копировать", "Copy"), *l1 = TR("Скопировано", "Copied");
            f32 hfs = SC(12.5f), phh = S(30), slide = S(9);
            f32 w0p = floorf(S(11) + text_width(FONT_TEXT, hfs, l0, -1) + S(8) + S(14) + S(10));
            f32 w1p = floorf(S(11) + text_width(FONT_TEXT, hfs, l1, -1) + S(8) + S(14) + S(10));
            f32 pw2 = floorf(w0p + (w1p - w0p) * cp + 0.5f), hx = x1 - S(10) - pw2, hy = floorf(cy - phh * 0.5f);
            if (hv > 0) r_rect(hx, hy, pw2, phh, color_alpha(c->ctl_hover, hv * (1.f - cp)), S(8));
            if (cp > 0) r_rect(hx, hy, pw2, phh, color_alpha(ok_col, 0.16f * cp), S(8));
            r_set_clip(hx, MAX(hy, py + 1), hx + pw2, MIN(hy + phh, py + ph - 1));
            f32 gx = hx + pw2 - S(10) - S(7), base = baseline_for(FONT_TEXT, hfs, cy);
            u32 c0 = color_mix(t->dim, t->text, hv);
            R.opacity = base_op * (1.f - cp);
            text_draw(FONT_TEXT, hfs, hx + S(11), base - cp * slide, l0, -1, c0);
            text_draw_icon(0xE8C8, SC(12), gx, cy - cp * slide, c0);
            R.opacity = base_op * cp;
            text_draw(FONT_TEXT, hfs, hx + S(11), base + (1.f - cp) * slide, l1, -1, ok_col);
            text_draw_icon(0xE73E, SC(12), gx, cy + (1.f - cp) * slide, ok_col);
            R.opacity = base_op;
            r_set_clip(px, py + 1, px + pw, py + ph - 1);
        }
        y += row_h + row_gap;
    }
#undef DON_BLOCK
    R.opacity = e;
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    if (ms > 0 && D.sb_t > 0.f) {
        f32 vh = ph - S(24), bar_h = MAX(S(28), vh * ph / content), bw = S(3);
        r_rect(px + pw - S(6) - bw, floorf(py + S(12) + (vh - bar_h) * D.scroll / ms), bw, floorf(bar_h), color_alpha(t->faint, D.sb_t),
               bw * 0.5f);
    }
    R.opacity = 1.f;
}
