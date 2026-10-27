// The "PC components" page: four summary tiles, then a card per part in two columns (the shorter column takes the
// next card). Scrolls with the wheel. The data comes from hw.c.

#define HW_TOP 88.f        // the page content starts under the title, in DIPs
#define HW_ROW_H 34.f
#define HW_HEAD_H 86.f
#define HW_SCORE_H 248.f

// Light green into the app's accent, along the gauge (0..1).
static u32 score_color(f32 p)
{
    const Theme *t = &A.th;
    u32 light = t->dark ? RGBA(196, 242, 178, 255) : RGBA(146, 214, 150, 255);
    return color_mix(light, t->accent, CLAMP(p, 0.f, 1.f));
}

static void draw_icon_tile(f32 x, f32 y, f32 sz, u32 glyph)
{
    const Theme *t = &A.th;
    r_rect(x, y, sz, sz, color_alpha(t->accent, t->dark ? 0.16f : 0.14f), floorf(sz * 0.28f));
    text_draw_icon(glyph, sz * 0.46f, x + sz * 0.5f, y + sz * 0.5f, t->accent);
}

// The score: a ring gauge on the left, the four parts it is made of on the right.
static void draw_hw_score(const Box *b)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    r_rect(b->x, b->y, b->w, b->h, c->card, S(10));
    r_rect_ex(b->x, b->y, b->w, b->h, c->card_border, S(10), 1.f, 0);
    f32 k = ease_out_cubic(A.score_t), v = (f32)HW.d.score_total * k;

    // A 270-degree ring open at the bottom, drawn as short overlapping segments of solid color so the joins don't
    // show; the filled part runs light green into the accent along the ring, with round caps.
    f32 r = S(80), th_w = S(14), cx = b->x + S(40) + r, cy = b->y + b->h * 0.5f + S(10);
    u32 track = t->dark ? RGBA(58, 59, 63, 255) : RGBA(225, 226, 230, 255);
    const int n = 180;
    f32 seg = r * 4.712389f / (f32)n * 1.7f;
    for (int i = 0; i < n; i++) {
        f32 p = ((f32)i + 0.5f) / (f32)n, a = (225.f - 270.f * p) * 0.01745329f;
        r_rect_rot(cx + cosf(a) * r, cy - sinf(a) * r, th_w, seg, -a, 1.f, track, 0);
    }
    f32 a0 = 225.f * 0.01745329f, a1 = -45.f * 0.01745329f;
    r_rect(cx + cosf(a0) * r - th_w * 0.5f, cy - sinf(a0) * r - th_w * 0.5f, th_w, th_w, track, th_w * 0.5f);
    r_rect(cx + cosf(a1) * r - th_w * 0.5f, cy - sinf(a1) * r - th_w * 0.5f, th_w, th_w, track, th_w * 0.5f);
    f32 pv = v / 100.f;
    if (pv > 0.f) {
        int m = (int)ceilf((f32)n * pv);
        for (int i = 0; i < m; i++) {
            f32 p = MIN(((f32)i + 0.5f) / (f32)n, pv), a = (225.f - 270.f * p) * 0.01745329f;
            r_rect_rot(cx + cosf(a) * r, cy - sinf(a) * r, th_w, seg, -a, 1.f, score_color(p / pv), 0);
        }
        r_rect(cx + cosf(a0) * r - th_w * 0.5f, cy - sinf(a0) * r - th_w * 0.5f, th_w, th_w, score_color(0), th_w * 0.5f);
        f32 ae = (225.f - 270.f * pv) * 0.01745329f, ex = cx + cosf(ae) * r, ey = cy - sinf(ae) * r;
        r_rect(ex - th_w * 0.5f, ey - th_w * 0.5f, th_w, th_w, score_color(1.f), th_w * 0.5f);
        r_rect(ex - S(3), ey - S(3), S(6), S(6), RGBA(255, 255, 255, 230), S(3));
    }
    WCHAR num[8];
    wfmt(num, countof(num), L"%d", (int)(v + 0.5f));
    text_center(FONT_DISPLAY, SC(48), cx, cy - S(8), num, t->text);
    text_center(FONT_TEXT, SC(12), cx, cy + S(22), TR("из 100", "of 100"), t->dim);
    // the tier on a pill in the ring's opening
    const WCHAR *tier = score_tier(HW.d.score_total);
    u32 tc = t->accent;
    f32 tfs = SC(12.5f), tw = text_width(FONT_TEXT_SEMIBOLD, tfs, tier, -1) + S(22), tph = S(24), ty = cy + r * 0.72f - tph * 0.5f + S(4);
    r_rect(floorf(cx - tw * 0.5f), ty, tw, tph, color_alpha(tc, 0.18f), tph * 0.5f);
    text_center(FONT_TEXT_SEMIBOLD, tfs, cx, ty + tph * 0.5f, tier, tc);

    // right: what the score means, then a tile per part
    f32 x = cx + r + S(52), x1 = b->x + b->w - S(24), y = b->y + S(24);
    text_draw(FONT_TEXT_SEMIBOLD, SC(17), x, baseline_for(FONT_TEXT_SEMIBOLD, SC(17), y + S(4)), TR("Оценка ПК", "PC score"), -1, t->text);
    text_draw_fit(FONT_TEXT, SC(12.5f), x, baseline_for(FONT_TEXT, SC(12.5f), y + S(30)), x1 - x,
                  TR("Оценка комплектующих ПК по игровой мощности", "The PC parts rated by gaming power"), -1, t->dim, false);
    static const struct { u32 glyph; const WCHAR *ru, *en; } parts[SC__COUNT] = {
        { 0xF211, L"Видеокарта", L"Graphics" }, { 0xEEA1, L"Процессор", L"Processor" },
        { 0xE964, L"Память", L"Memory" },       { 0xEDA2, L"Системный диск", L"System drive" },
    };
    f32 gap = S(10), cw = floorf((x1 - x - gap) * 0.5f), ch = S(68), gy = b->y + b->h - S(20) - ch * 2 - gap;
    for (int i = 0; i < SC__COUNT; i++) {
        Box tb = { x + (cw + gap) * (f32)(i % 2), gy + (ch + gap) * (f32)(i / 2), cw, ch };
        r_rect(tb.x, tb.y, tb.w, tb.h, c->ctl, S(8));
        f32 isz = S(34);
        draw_icon_tile(tb.x + S(12), tb.y + (tb.h - isz) * 0.5f, isz, parts[i].glyph);
        f32 tx = tb.x + S(12) + isz + S(12), tr = tb.x + tb.w - S(14), pv2 = (f32)HW.d.score_part[i] * k;
        wfmt(num, countof(num), L"%d", (int)(pv2 + 0.5f));
        f32 nw = text_width(FONT_TEXT_SEMIBOLD, SC(17), num, -1);
        // the part, its score, and under them what it is: the name cut with an ellipsis when long
        text_draw_fit(FONT_TEXT, SC(12), tx, baseline_for(FONT_TEXT, SC(12), tb.y + S(18)), tr - tx - nw - S(8), g_lang_ru ? parts[i].ru : parts[i].en,
                      -1, t->dim, false);
        text_right(FONT_TEXT_SEMIBOLD, SC(17), tr, tb.y + S(20), num, t->text);
        const WCHAR *nm = HW.d.score_name[i];
        text_draw_fit(FONT_TEXT_SEMIBOLD, SC(12.5f), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(12.5f), tb.y + S(37)), tr - tx - nw - S(8),
                      nm[0] ? nm : TR("Не определено", "Unknown"), -1, nm[0] ? t->text : t->faint, false);
        f32 bw = tr - tx, by = tb.y + S(52);
        r_rect(tx, by, bw, S(5), track, S(2.5f));
        if (pv2 > 0.5f) r_rect(tx, by, MAX(S(5), floorf(bw * pv2 / 100.f)), S(5), t->accent, S(2.5f));
    }
}

static f32 hw_card_h(const HwCard *c) { return S(HW_HEAD_H) + (c->nrows ? S(HW_ROW_H) * (f32)c->nrows + S(8) : 0); }

// --- skeletons -----------------------------------------------------------------------------------------------------
// While a page reads its data: grey shapes where the content goes, a light band sweeping across all of them at once.
// They take the real layout when there is older data, so nothing jumps when the new data fades in over them.

static void skel(f32 x, f32 y, f32 w, f32 h, f32 rad)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    if (w <= 0 || h <= 0) return;
    r_rect(x, y, w, h, c->ctl, rad);
    f32 x0 = content_x0(), span = content_x1() - x0, band = S(180);
    f32 ph = (f32)fmod(time_now() * 0.8, 1.0), bx = x0 - band + (span + band * 2.f) * ph;
    f32 saved[4];
    memcpy(saved, R.clip, sizeof saved);
    r_set_clip(MAX(x, saved[0]), MAX(y, saved[1]), MIN(x + w, saved[2]), MIN(y + h, saved[3]));
    if (R.clip[2] > R.clip[0] && R.clip[3] > R.clip[1])
        r_rect_ex(bx, y - S(20), band, h + S(40), t->dark ? RGBA(255, 255, 255, 16) : RGBA(255, 255, 255, 150), 0, 0, S(60));
    memcpy(R.clip, saved, sizeof saved);
}

static void skel_frame(const Box *b)
{
    const Colors *c = &A.c;
    r_rect(b->x, b->y, b->w, b->h, c->card, S(10));
    r_rect_ex(b->x, b->y, b->w, b->h, c->card_border, S(10), 1.f, 0);
}

static void draw_skel_card(const Box *b, int nrows, int seed)
{
    skel_frame(b);
    f32 pad = S(18), isz = S(42), tx = b->x + pad + isz + S(14), w = b->w - pad * 2;
    skel(b->x + pad, b->y + pad, isz, isz, floorf(isz * 0.28f));
    skel(tx, b->y + S(20), S(84), S(10), S(5));
    skel(tx, b->y + S(38), floorf((b->w - tx + b->x) * 0.55f), S(14), S(7));
    skel(tx, b->y + S(61), floorf((b->w - tx + b->x) * 0.35f), S(10), S(5));
    if (!nrows) return;
    f32 y = b->y + S(HW_HEAD_H) - S(4);
    for (int i = 0; i < nrows; i++) {
        f32 cy = y + S(HW_ROW_H) * ((f32)i + 0.5f) + S(2);
        f32 kw = floorf(w * (0.22f + 0.07f * (f32)((seed * 5 + i * 3) % 4))), vw = floorf(w * (0.10f + 0.06f * (f32)((seed * 3 + i * 7) % 3)));
        skel(b->x + pad, cy - S(5), kw, S(10), S(5));
        skel(b->x + b->w - pad - vw, cy - S(5), vw, S(10), S(5));
    }
}

static void draw_skel_stat(const Box *b)
{
    skel_frame(b);
    f32 isz = S(40), cy = b->y + b->h * 0.5f, tx = b->x + S(18) + isz + S(14);
    skel(b->x + S(18), floorf(cy - isz * 0.5f), isz, isz, floorf(isz * 0.28f));
    skel(tx, cy - S(19), S(54), S(18), S(6));
    skel(tx, cy + S(10), MIN(S(120), b->x + b->w - tx - S(16)), S(10), S(5));
}

static void draw_skel_score(const Box *b)
{
    skel_frame(b);
    f32 r = S(80), cx = b->x + S(40) + r, cy = b->y + b->h * 0.5f + S(10);
    // the ring's place, as a round shape
    skel(cx - r - S(7), cy - r - S(7), (r + S(7)) * 2, (r + S(7)) * 2, r + S(7));
    f32 x = cx + r + S(52), x1 = b->x + b->w - S(24), y = b->y + S(24);
    skel(x, y - S(4), S(120), S(16), S(8));
    skel(x, y + S(24), floorf((x1 - x) * 0.8f), S(10), S(5));
    skel(x, y + S(42), floorf((x1 - x) * 0.5f), S(10), S(5));
    f32 gap = S(10), cw = floorf((x1 - x - gap) * 0.5f), ch = S(68), gy = b->y + b->h - S(20) - ch * 2 - gap;
    for (int i = 0; i < 4; i++) skel(x + (cw + gap) * (f32)(i % 2), gy + (ch + gap) * (f32)(i / 2), cw, ch, S(8));
}

// A spinner of eight dots, the bright one going round.
static void draw_spinner(f32 cx, f32 cy, f32 r, u32 col)
{
    f32 ph = (f32)fmod(time_now() * 1.1, 1.0);
    for (int i = 0; i < 8; i++) {
        f32 a = (f32)i / 8.f * 6.2831853f - 1.5707963f, d = S(5);
        f32 lag = (f32)fmod((f32)i / 8.f - ph + 1.f, 1.f);
        r_rect(cx + cosf(a) * r - d * 0.5f, cy + sinf(a) * r - d * 0.5f, d, d, color_alpha(col, 0.2f + 0.8f * (1.f - lag)), d * 0.5f);
    }
}

// Lays cards out in two columns, each into the shorter one. Returns where the lower column ends.
static f32 masonry(const f32 *heights, int n, f32 y, Box *out)
{
    f32 x0 = content_x0(), cw = content_x1() - x0, gap = S(14), colw = floorf((cw - gap) / 2), col_y[2] = { y, y };
    for (int i = 0; i < n; i++) {
        int col = col_y[1] < col_y[0] ? 1 : 0;
        out[i] = (Box){ x0 + (colw + gap) * (f32)col, col_y[col], colw, heights[i] };
        col_y[col] += heights[i] + gap;
    }
    return MAX(col_y[0], col_y[1]);
}

// Lays the page out at scroll 0: the stat tiles and the cards. Returns the content height.
static f32 hw_layout(Box *score, Box *stats, Box *cards)
{
    f32 x0 = content_x0(), x1 = content_x1(), cw = x1 - x0, gap = S(14);
    f32 tw = floorf((cw - gap * 3) / 4), y = S(HW_TOP);
    *score = (Box){ x0, y, cw, S(HW_SCORE_H) };
    y += score->h + gap;
    for (int i = 0; i < 4; i++) stats[i] = (Box){ x0 + (tw + gap) * (f32)i, y, tw, S(84) };
    y += S(84) + gap;
    f32 colw = floorf((cw - gap) / 2), col_y[2] = { y, y };
    for (int i = 0; i < HWC__COUNT; i++) {
        int col = col_y[1] < col_y[0] ? 1 : 0;
        f32 h = hw_card_h(&HW.d.card[i]);
        cards[i] = (Box){ x0 + (colw + gap) * (f32)col, col_y[col], colw, h };
        col_y[col] += h + gap;
    }
    return MAX(col_y[0], col_y[1]) - S(HW_TOP);
}

static f32 hw_view_h(void) { return (f32)A.H - S(HW_TOP) - S(8); }
static f32 hw_max_scroll(void) { return MAX(0.f, A.hw_content_h - hw_view_h()); }

static void draw_hw_stat(const Box *b, const HwStat *s)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    r_rect(b->x, b->y, b->w, b->h, c->card, S(10));
    r_rect_ex(b->x, b->y, b->w, b->h, c->card_border, S(10), 1.f, 0);
    f32 isz = S(40), cy = b->y + b->h * 0.5f;
    draw_icon_tile(b->x + S(18), floorf(cy - isz * 0.5f), isz, s->glyph);
    f32 tx = b->x + S(18) + isz + S(14), vfs = SC(24), ufs = SC(14);
    f32 vx = text_draw(FONT_DISPLAY, vfs, tx, baseline_for(FONT_DISPLAY, vfs, cy - S(9)), s->value, -1, t->text);
    if (s->unit[0]) text_draw(FONT_TEXT, ufs, vx + S(5), baseline_for(FONT_DISPLAY, vfs, cy - S(9)), s->unit, -1, t->dim);
    text_draw_fit(FONT_TEXT, SC(12.5f), tx, baseline_for(FONT_TEXT, SC(12.5f), cy + S(15)), b->x + b->w - tx - S(12), s->label, -1, t->dim, false);
}

static void draw_hw_card(const Box *b, const HwCard *cd)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    r_rect(b->x, b->y, b->w, b->h, c->card, S(10));
    r_rect_ex(b->x, b->y, b->w, b->h, c->card_border, S(10), 1.f, 0);
    f32 pad = S(18), isz = S(42);
    draw_icon_tile(b->x + pad, b->y + pad, isz, cd->glyph);
    f32 tx = b->x + pad + isz + S(14), tmax = b->x + b->w - tx - pad;
    text_draw_fit(FONT_TEXT, SC(12), tx, floorf(b->y + S(28)), tmax - (cd->badge[0] ? S(110) : 0), cd->cat, -1, t->dim, false);
    if (cd->badge[0]) {
        // the status pill on the health page: "Good · 98%"
        u32 sc = cd->state == HS_BAD ? t->danger : cd->state == HS_WARN ? t->warn : t->good;
        f32 bfs = SC(12), bw = text_width(FONT_TEXT_SEMIBOLD, bfs, cd->badge, -1) + S(20), bh = S(24);
        f32 bx = b->x + b->w - pad - bw, by = b->y + pad;
        r_rect(bx, by, bw, bh, color_alpha(sc, 0.16f), bh * 0.5f);
        text_center(FONT_TEXT_SEMIBOLD, bfs, bx + bw * 0.5f, by + bh * 0.5f, cd->badge, sc);
        tmax = bx - S(10) - tx;
    }
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(15), tx, floorf(b->y + S(48)), tmax, cd->head, -1, t->text, false);
    if (cd->sub[0]) text_draw_fit(FONT_TEXT, SC(12.5f), tx, floorf(b->y + S(66)), tmax, cd->sub, -1, t->dim, false);
    if (!cd->nrows) return;
    f32 y = b->y + S(HW_HEAD_H) - S(4);
    r_rect(b->x + pad, y, b->w - pad * 2, 1, t->sep, 0);
    f32 fs = SC(13);
    for (int i = 0; i < cd->nrows; i++) {
        const HwRow *r = &cd->rows[i];
        f32 cy = y + S(HW_ROW_H) * ((f32)i + 0.5f) + S(2), w = b->w - pad * 2;
        // the value keeps up to 60% of the row; the name takes what is left
        f32 vw = MIN(text_width(FONT_TEXT, fs, r->v, -1), floorf(w * (r->item ? 0.55f : 0.62f)));
        f32 kmax = w - vw - S(16);
        text_draw_fit(FONT_TEXT, fs, b->x + pad, baseline_for(FONT_TEXT, fs, cy), kmax, r->k, -1, r->item ? t->text : t->dim, false);
        f32 vx = b->x + b->w - pad - vw;
        u32 vc = r->state == HS_BAD ? t->danger : r->state == HS_WARN ? t->warn : r->state == HS_GOOD ? t->good : r->item ? t->dim : t->text;
        text_draw_fit(FONT_TEXT, fs, vx, baseline_for(FONT_TEXT, fs, cy), vw + 1, r->v, -1, vc, false);
        if (i + 1 < cd->nrows) r_rect(b->x + pad, floorf(cy + S(HW_ROW_H) * 0.5f), w, 1, color_alpha(t->sep, 0.5f), 0);
    }
}

static Box box_off(Box b, f32 dy)
{
    b.y += dy;
    return b;
}

static bool box_seen(const Box *b, f32 top) { return b->y + b->h >= top && b->y <= (f32)A.H; }

// The page: a skeleton until the parts are read (and while they are read again), the parts fading in over it.
// The caller holds HW.lock.
static void draw_hw_page(f32 rise)
{
    const Theme *t = &A.th;
    bool have = HW.ready != 0;
    Box score, stats[4], cards[HWC__COUNT];
    int rows[HWC__COUNT];
    if (have) {
        A.hw_content_h = hw_layout(&score, stats, cards);
        for (int i = 0; i < HWC__COUNT; i++) rows[i] = HW.d.card[i].nrows;
    } else {
        // the first start: a page of the usual shape
        static const int k_rows[HWC__COUNT] = { 6, 5, 4, 6, 3, 0, 0, 5 };
        f32 x0 = content_x0(), cw = content_x1() - x0, gap = S(14), tw = floorf((cw - gap * 3) / 4), y = S(HW_TOP), hs[HWC__COUNT];
        score = (Box){ x0, y, cw, S(HW_SCORE_H) };
        y += score.h + gap;
        for (int i = 0; i < 4; i++) stats[i] = (Box){ x0 + (tw + gap) * (f32)i, y, tw, S(84) };
        y += S(84) + gap;
        for (int i = 0; i < HWC__COUNT; i++) {
            rows[i] = k_rows[i];
            hs[i] = S(HW_HEAD_H) + (rows[i] ? S(HW_ROW_H) * (f32)rows[i] + S(8) : 0);
        }
        A.hw_content_h = masonry(hs, HWC__COUNT, y, cards) - S(HW_TOP);
    }
    f32 off = rise - floorf(A.hw_scroll + 0.5f), top = S(HW_TOP) - S(6), base = R.opacity, f = have ? A.hw_fade : 0.f;
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    if (f < 1.f) {
        R.opacity = base * (1.f - f);
        Box b = box_off(score, off);
        if (box_seen(&b, top)) draw_skel_score(&b);
        for (int i = 0; i < 4; i++)
            if (b = box_off(stats[i], off), box_seen(&b, top)) draw_skel_stat(&b);
        for (int i = 0; i < HWC__COUNT; i++)
            if (b = box_off(cards[i], off), box_seen(&b, top)) draw_skel_card(&b, rows[i], i);
    }
    if (f > 0.f) {
        R.opacity = base * f;
        Box b = box_off(score, off);
        if (box_seen(&b, top)) draw_hw_score(&b);
        for (int i = 0; i < 4; i++)
            if (b = box_off(stats[i], off), box_seen(&b, top)) draw_hw_stat(&b, &HW.d.stat[i]);
        for (int i = 0; i < HWC__COUNT; i++)
            if (b = box_off(cards[i], off), box_seen(&b, top)) draw_hw_card(&b, &HW.d.card[i]);
    }
    R.opacity = base;
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = hw_max_scroll();
    if (ms > 0 && A.hw_sb_t > 0) {
        f32 vh = hw_view_h(), bar = MAX(S(32), vh * vh / A.hw_content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(A.hw_scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, A.hw_sb_t), S(1.5f));
    }
}
