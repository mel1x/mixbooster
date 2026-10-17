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
    for (int i = 0; i < HL.ncards; i++) {
        int col = col_y[1] < col_y[0] ? 1 : 0;
        f32 h = hw_card_h(&HL.card[i]);
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
    u32 col = state_color(HL.state);
    r_rect(ix, cy - isz * 0.5f, isz, isz, color_alpha(col, 0.16f), isz * 0.5f);
    text_draw_icon(HL.state == HS_BAD ? 0xEA39 : HL.state == HS_WARN ? 0xE7BA : 0xE930, SC(26), ix + isz * 0.5f, cy, col);
    const WCHAR *title = HL.state == HS_BAD    ? TR("Есть проблемы", "There are problems")
                         : HL.state == HS_WARN ? TR("Есть на что обратить внимание", "A few things need a look")
                                               : TR("Всё в порядке", "All good");
    const WCHAR *sub = HL.state == HS_BAD ? TR("Посмотрите карточки с красной отметкой: возможно, пора заменить деталь или проверить ПК",
                                               "Check the cards marked red: a part may need replacing, or the PC a check-up")
                       : HL.state == HS_WARN ? TR("Посмотрите карточки с жёлтой отметкой", "Check the cards marked yellow")
                                             : TR("Диски, батарея и система работают без проблем", "Drives, battery and system are fine");
    A.hl_refresh = (Box){ b->x + b->w - S(24) - S(170), cy - S(17), S(170), S(34) };
    f32 tx = ix + isz + S(20), tmax = A.hl_refresh.x - S(16) - tx;
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(20), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(20), cy - S(12)), tmax, title, -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(13), tx, baseline_for(FONT_TEXT, SC(13), cy + S(15)), tmax, sub, -1, t->dim, false);
    bool busy = HL.busy != 0;
    draw_button(&A.hl_refresh, busy ? TR("Проверяю…", "Checking…") : TR("Проверить снова", "Check again"), 0xE72C, BTN_NEUTRAL, A.hv[HV_HLREFRESH],
                A.press.type == HT_HLREFRESH && A.hover.type == HT_HLREFRESH, busy);
}

static void draw_health_page(f32 rise)
{
    const Theme *t = &A.th;
    if (!HL.ready && !HL.ncards) {
        f32 cx = (content_x0() + content_x1()) * 0.5f, cy = (S(HW_TOP) + (f32)A.H) * 0.5f;
        text_center(FONT_TEXT, SC(13.5f), cx, cy + rise, TR("Проверяю диски и журнал Windows…", "Checking the drives and the Windows log…"), t->dim);
        return;
    }
    Box hero, stats[4], cards[HL_CARDS];
    A.hl_content_h = hl_layout(&hero, stats, cards);
    f32 off = rise - floorf(A.hl_scroll + 0.5f), top = S(HW_TOP) - S(6);
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    hero.y += off;
    if (hero.y + hero.h >= top) draw_hl_hero(&hero);
    for (int i = 0; i < 4; i++) {
        Box b = stats[i];
        b.y += off;
        if (b.y + b.h >= top && b.y <= A.H) draw_hw_stat(&b, &HL.stat[i]);
    }
    for (int i = 0; i < HL.ncards; i++) {
        Box b = cards[i];
        b.y += off;
        if (b.y + b.h >= top && b.y <= A.H) draw_hw_card(&b, &HL.card[i]);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = hl_max_scroll();
    if (ms > 0 && A.hl_sb_t > 0) {
        f32 vh = hw_view_h(), bar = MAX(S(32), vh * vh / A.hl_content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(A.hl_scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, A.hl_sb_t), S(1.5f));
    }
}
