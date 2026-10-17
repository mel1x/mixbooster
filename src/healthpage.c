// The "PC health" page: the verdict at the top, four tiles, then a card per drive, the battery, stability and
// devices, laid out like the parts page. The data comes from health.c.

#define HL_HERO_H 104.f

static f32 hl_layout(Box *hero, Box *stats, Box *cards)
{
    f32 x0 = content_x0(), x1 = content_x1(), cw = x1 - x0, gap = S(14);
    f32 tw = floorf((cw - gap * 3) / 4), y = S(HW_TOP);
    *hero = (Box){ x0, y, cw, S(HL_HERO_H) };
    y += hero->h + gap;
    for (int i = 0; i < 4; i++) stats[i] = (Box){ x0 + (tw + gap) * (f32)i, y, tw, S(84) };
    y += S(84) + gap;
    f32 colw = floorf((cw - gap) / 2), col_y[2] = { y, y };
    for (int i = 0; i < HL.d.ncards; i++) {
        int col = col_y[1] < col_y[0] ? 1 : 0;
        f32 h = hw_card_h(&HL.d.card[i]);
        cards[i] = (Box){ x0 + (colw + gap) * (f32)col, col_y[col], colw, h };
        col_y[col] += h + gap;
    }
    return MAX(col_y[0], col_y[1]) - S(HW_TOP);
}

static f32 hl_max_scroll(void) { return MAX(0.f, A.hl_content_h - hw_view_h()); }

static u32 state_color(u8 s)
{
    const Theme *t = &A.th;
    return s == HS_BAD ? t->danger : s == HS_WARN ? t->warn : t->good;
}

static void draw_hl_hero(const Box *b)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    r_rect(b->x, b->y, b->w, b->h, c->card, S(10));
    r_rect_ex(b->x, b->y, b->w, b->h, c->card_border, S(10), 1.f, 0);
    f32 isz = S(56), cy = b->y + b->h * 0.5f, ix = b->x + S(24);
    u32 col = state_color(HL.d.state);
    r_rect(ix, cy - isz * 0.5f, isz, isz, color_alpha(col, 0.16f), isz * 0.5f);
    text_draw_icon(HL.d.state == HS_BAD ? 0xEA39 : HL.d.state == HS_WARN ? 0xE7BA : 0xE930, SC(26), ix + isz * 0.5f, cy, col);
    const WCHAR *title = HL.d.state == HS_BAD    ? TR("Есть проблемы", "There are problems")
                         : HL.d.state == HS_WARN ? TR("Есть на что обратить внимание", "A few things need a look")
                                               : TR("Всё в порядке", "All good");
    const WCHAR *sub = HL.d.state == HS_BAD ? TR("Посмотрите карточки с красной отметкой: возможно, пора заменить деталь или проверить ПК",
                                               "Check the cards marked red: a part may need replacing, or the PC a check-up")
                       : HL.d.state == HS_WARN ? TR("Посмотрите карточки с жёлтой отметкой", "Check the cards marked yellow")
                                             : TR("Диски, батарея и система работают без проблем", "Drives, battery and system are fine");
    A.hl_refresh = (Box){ b->x + b->w - S(24) - S(170), cy - S(17), S(170), S(34) };
    f32 tx = ix + isz + S(20), tmax = A.hl_refresh.x - S(16) - tx;
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(20), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(20), cy - S(12)), tmax, title, -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(13), tx, baseline_for(FONT_TEXT, SC(13), cy + S(15)), tmax, sub, -1, t->dim, false);
    draw_button(&A.hl_refresh, TR("Проверить снова", "Check again"), 0xE72C, BTN_NEUTRAL, A.hv[HV_HLREFRESH],
                A.press.type == HT_HLREFRESH && A.hover.type == HT_HLREFRESH, HL.busy != 0);
}

// The verdict while the check runs: a spinner instead of the status.
static void draw_hl_hero_checking(const Box *b)
{
    const Theme *t = &A.th;
    skel_frame(b);
    f32 isz = S(56), cy = b->y + b->h * 0.5f, ix = b->x + S(24);
    r_rect(ix, cy - isz * 0.5f, isz, isz, color_alpha(t->accent, 0.12f), isz * 0.5f);
    draw_spinner(ix + isz * 0.5f, cy, S(13), t->accent);
    A.hl_refresh = (Box){ b->x + b->w - S(24) - S(170), cy - S(17), S(170), S(34) };
    f32 tx = ix + isz + S(20), tmax = A.hl_refresh.x - S(16) - tx;
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(20), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(20), cy - S(12)), tmax,
                  TR("Проверяю здоровье ПК…", "Checking the PC…"), -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(13), tx, baseline_for(FONT_TEXT, SC(13), cy + S(15)), tmax,
                  TR("Диски, журнал Windows, батарея и устройства", "Drives, the Windows log, battery and devices"), -1, t->dim, false);
    draw_button(&A.hl_refresh, TR("Проверяю…", "Checking…"), 0xE72C, BTN_NEUTRAL, 0, false, true);
}

// The page: a skeleton with the checking verdict while the check runs (in the shape of the last results when there
// are some), then the results fading in over it. The caller holds HL.lock.
static void draw_health_page(f32 rise)
{
    const Theme *t = &A.th;
    bool have = HL.ready != 0;
    Box hero, stats[4], cards[HL_CARDS];
    int n, rows[HL_CARDS];
    if (have) {
        A.hl_content_h = hl_layout(&hero, stats, cards);
        n = HL.d.ncards;
        for (int i = 0; i < n; i++) rows[i] = HL.d.card[i].nrows;
    } else {
        static const int k_rows[] = { 8, 4, 8, 0 };
        n = countof(k_rows);
        f32 x0 = content_x0(), cw = content_x1() - x0, gap = S(14), tw = floorf((cw - gap * 3) / 4), y = S(HW_TOP), hs[countof(k_rows)];
        hero = (Box){ x0, y, cw, S(HL_HERO_H) };
        y += hero.h + gap;
        for (int i = 0; i < 4; i++) stats[i] = (Box){ x0 + (tw + gap) * (f32)i, y, tw, S(84) };
        y += S(84) + gap;
        for (int i = 0; i < n; i++) {
            rows[i] = k_rows[i];
            hs[i] = S(HW_HEAD_H) + (rows[i] ? S(HW_ROW_H) * (f32)rows[i] + S(8) : 0);
        }
        A.hl_content_h = masonry(hs, n, y, cards) - S(HW_TOP);
    }
    f32 off = rise - floorf(A.hl_scroll + 0.5f), top = S(HW_TOP) - S(6), base = R.opacity, f = have ? A.hl_fade : 0.f;
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    if (f < 1.f) {
        R.opacity = base * (1.f - f);
        Box b = box_off(hero, off);
        if (box_seen(&b, top)) draw_hl_hero_checking(&b);
        for (int i = 0; i < 4; i++)
            if (b = box_off(stats[i], off), box_seen(&b, top)) draw_skel_stat(&b);
        for (int i = 0; i < n; i++)
            if (b = box_off(cards[i], off), box_seen(&b, top)) draw_skel_card(&b, rows[i], i);
    }
    if (f > 0.f) {
        R.opacity = base * f;
        Box b = box_off(hero, off);
        if (box_seen(&b, top)) draw_hl_hero(&b);
        for (int i = 0; i < 4; i++)
            if (b = box_off(stats[i], off), box_seen(&b, top)) draw_hw_stat(&b, &HL.d.stat[i]);
        for (int i = 0; i < n; i++)
            if (b = box_off(cards[i], off), box_seen(&b, top)) draw_hw_card(&b, &HL.d.card[i]);
    }
    R.opacity = base;
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = hl_max_scroll();
    if (ms > 0 && A.hl_sb_t > 0) {
        f32 vh = hw_view_h(), bar = MAX(S(32), vh * vh / A.hl_content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(A.hl_scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, A.hl_sb_t), S(1.5f));
    }
}
