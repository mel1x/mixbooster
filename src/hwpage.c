// The "PC components" page: four summary tiles, then a card per part in two columns (the shorter column takes the
// next card). Scrolls with the wheel. The data comes from hw.c.

#define HW_TOP 88.f        // the page content starts under the title, in DIPs
#define HW_ROW_H 34.f
#define HW_HEAD_H 86.f

static f32 hw_card_h(const HwCard *c) { return S(HW_HEAD_H) + (c->nrows ? S(HW_ROW_H) * (f32)c->nrows + S(8) : 0); }

// Lays the page out at scroll 0: the stat tiles and the cards. Returns the content height.
static f32 hw_layout(Box *stats, Box *cards)
{
    f32 x0 = content_x0(), x1 = content_x1(), cw = x1 - x0, gap = S(14);
    f32 tw = floorf((cw - gap * 3) / 4), y = S(HW_TOP);
    for (int i = 0; i < 4; i++) stats[i] = (Box){ x0 + (tw + gap) * (f32)i, y, tw, S(84) };
    y += S(84) + gap;
    f32 colw = floorf((cw - gap) / 2), col_y[2] = { y, y };
    for (int i = 0; i < HWC__COUNT; i++) {
        int col = col_y[1] < col_y[0] ? 1 : 0;
        f32 h = hw_card_h(&HW.card[i]);
        cards[i] = (Box){ x0 + (colw + gap) * (f32)col, col_y[col], colw, h };
        col_y[col] += h + gap;
    }
    return MAX(col_y[0], col_y[1]) - S(HW_TOP);
}

static f32 hw_view_h(void) { return (f32)A.H - S(HW_TOP) - S(8); }
static f32 hw_max_scroll(void) { return MAX(0.f, A.hw_content_h - hw_view_h()); }

static void draw_icon_tile(f32 x, f32 y, f32 sz, u32 glyph)
{
    const Theme *t = &A.th;
    r_rect(x, y, sz, sz, color_alpha(t->accent, t->dark ? 0.16f : 0.14f), floorf(sz * 0.28f));
    text_draw_icon(glyph, sz * 0.46f, x + sz * 0.5f, y + sz * 0.5f, t->accent);
}

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
    text_draw_fit(FONT_TEXT, SC(12), tx, floorf(b->y + S(28)), tmax, cd->cat, -1, t->dim, false);
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
        text_draw_fit(FONT_TEXT, fs, vx, baseline_for(FONT_TEXT, fs, cy), vw + 1, r->v, -1, r->item ? t->dim : t->text, false);
        if (i + 1 < cd->nrows) r_rect(b->x + pad, floorf(cy + S(HW_ROW_H) * 0.5f), w, 1, color_alpha(t->sep, 0.5f), 0);
    }
}

static void draw_hw_page(f32 rise)
{
    const Theme *t = &A.th;
    if (!HW.ready) {
        f32 cx = (content_x0() + content_x1()) * 0.5f, cy = (S(HW_TOP) + (f32)A.H) * 0.5f;
        text_center(FONT_TEXT, SC(13.5f), cx, cy + rise, TR("Собираю сведения о ПК…", "Reading the PC's parts…"), t->dim);
        return;
    }
    Box stats[4], cards[HWC__COUNT];
    A.hw_content_h = hw_layout(stats, cards);
    f32 off = rise - floorf(A.hw_scroll + 0.5f), top = S(HW_TOP) - S(6);
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    for (int i = 0; i < 4; i++) {
        Box b = stats[i];
        b.y += off;
        if (b.y + b.h >= top && b.y <= A.H) draw_hw_stat(&b, &HW.stat[i]);
    }
    for (int i = 0; i < HWC__COUNT; i++) {
        Box b = cards[i];
        b.y += off;
        if (b.y + b.h >= top && b.y <= A.H) draw_hw_card(&b, &HW.card[i]);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = hw_max_scroll();
    if (ms > 0 && A.hw_sb_t > 0) {
        f32 vh = hw_view_h(), bar = MAX(S(32), vh * vh / A.hw_content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(A.hw_scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, A.hw_sb_t), S(1.5f));
    }
}
