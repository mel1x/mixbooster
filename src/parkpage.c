// The "Core parking" page: the power plans (with the MixBoost one to add where it's missing), the chosen plan's parking
// and processor state, and the cores live. Everything applies on the spot; the data is in park.c.

#define PR_HEAD_H 66.f
#define PR_PLAN_H 46.f
#define PR_ROW_H 90.f
#define PR_CELL_H 58.f

static struct {
    bool ready;
    PkPlan plan[PK_MAX_PLANS];
    int nplan, active, sel;
    bool battery, hybrid;
    int dc;                     // the values shown: 0 plugged in, 1 on battery
    DWORD val[PS__COUNT];
    int drag;                   // the slider held, PS_*; -1 none
    f32 drag_v;
    f32 tog_park, seg_t, tog_park_v, seg_v;
    f32 scroll, scroll_target, scroll_v, content_h, sb_t;
    Box plans, plan_row[PK_MAX_PLANS], act_btn[PK_MAX_PLANS], add, set, seg, park_sw, cores;
    Box slider[PS__COUNT];
    f32 slider_row[PS__COUNT];  // each slider row's top, 0 when not shown
    int ncells;
    Box cell[MAX_CPUS];
    WCHAR note[160];
    bool note_bad;
    int park_seq, park_pend;    // the parking switch's clicks and jobs not done
    int park_want;              // what the last click asked for while its job runs, -1 none
    GUID park_plan;             // and for which plan and power source
    int park_dc;
} PR = { .drag = -1, .park_want = -1 };

static bool pk_parking_read(const GUID *plan, bool dc, bool hybrid)
{
    return pk_read(plan, PS_PARK, dc) < 100 || (hybrid && pk_read(plan, PS_PARK1, dc) < 100);
}

// The switch: what was clicked while the job runs on the plan shown, else what the plan has.
static bool pr_parking_on(void)
{
    if (PR.park_want >= 0 && PR.sel >= 0 && PR.sel < PR.nplan && IsEqualGUID(&PR.park_plan, &PR.plan[PR.sel].id) && PR.park_dc == PR.dc)
        return PR.park_want != 0;
    return PR.val[PS_PARK] < 100 || (PR.hybrid && PR.val[PS_PARK1] < 100);
}

static bool pr_park_run(Job *j)
{
    pk_set_parking(&j->u.park.plan, j->u.park.dc, j->on);
    return pk_parking_read(&j->u.park.plan, j->u.park.dc, A.cpu.hybrid) == j->on;
}

static void pr_values(void);

static void pr_park_done(Job *j, bool ok)
{
    PR.park_pend--;
    if (j->seq == PR.park_seq) {
        PR.park_want = -1;
        if (!ok)
            toast_show(TR("Не удалось изменить парковку ядер: Windows не приняла значение схемы питания",
                          "Couldn't change core parking: Windows didn't take the power plan value"));
    }
    if (!PR.park_pend) pr_values();
}

static void pr_values(void)
{
    if (PR.sel < 0 || PR.sel >= PR.nplan) return;
    for (int s = 0; s < PS__COUNT; s++) PR.val[s] = pk_read(&PR.plan[PR.sel].id, s, PR.dc != 0);
}

static void pr_read(void)
{
    GUID keep = PR.sel >= 0 && PR.sel < PR.nplan ? PR.plan[PR.sel].id : (GUID){ 0 };
    bool had = PR.ready && PR.nplan;
    PR.nplan = pk_plans(PR.plan, PK_MAX_PLANS, &PR.active);
    PR.sel = PR.active >= 0 ? PR.active : 0;
    for (int i = 0; had && i < PR.nplan; i++)
        if (IsEqualGUID(&PR.plan[i].id, &keep)) PR.sel = i;
    PR.battery = pk_has_battery();
    if (!PR.battery) PR.dc = 0;
    PR.hybrid = A.cpu.hybrid;
    pr_values();
    if (!PR.ready) {
        PR.tog_park = pr_parking_on() ? 1.f : 0.f;
        PR.seg_t = (f32)PR.dc;
    }
    PR.ready = true;
}

static int pr_plan_index(const GUID *g)
{
    for (int i = 0; i < PR.nplan; i++)
        if (IsEqualGUID(&PR.plan[i].id, g)) return i;
    return -1;
}

// Which settings rows show, top to bottom: on a hybrid CPU the P- and E-cores' parking apart.
static int pr_rows(int *out)
{
    int n = 0;
    if (PR.hybrid) out[n++] = PS_PARK1;
    out[n++] = PS_PARK;
    out[n++] = PS_MIN;
    out[n++] = PS_MAX;
    return n;
}

static f32 pr_view_h(void) { return (f32)A.H - S(HW_TOP) - S(8); }
static f32 pr_max_scroll(void) { return MAX(0.f, PR.content_h - pr_view_h()); }

static void pr_layout(void)
{
    f32 x0 = content_x0(), x1 = content_x1(), cw = x1 - x0, gap = S(14), y0 = S(HW_TOP) - floorf(PR.scroll + 0.5f), y = y0;

    PR.plans = (Box){ x0, y, cw, S(PR_HEAD_H) + S(PR_PLAN_H) * (f32)PR.nplan + S(8) };
    bool have = false;
    for (int i = 0; i < PR.nplan; i++) have |= PR.plan[i].mixboost;
    PR.add = have ? (Box){ 0 } : (Box){ x1 - S(20) - S(270), y + (S(PR_HEAD_H) - S(32)) * 0.5f, S(270), S(32) };
    for (int i = 0; i < PR.nplan; i++) {
        PR.plan_row[i] = (Box){ x0 + S(8), y + S(PR_HEAD_H) + S(PR_PLAN_H) * (f32)i, cw - S(16), S(PR_PLAN_H) };
        PR.act_btn[i] = (Box){ x1 - S(20) - S(120), PR.plan_row[i].y + (S(PR_PLAN_H) - S(30)) * 0.5f, S(120), S(30) };
    }
    y += PR.plans.h + gap;

    int rows[PS__COUNT], nr = pr_rows(rows);
    PR.set = (Box){ x0, y, cw, S(PR_HEAD_H) + S(PR_ROW_H) * (f32)(nr + 1) + S(6) };
    PR.seg = PR.battery ? (Box){ x1 - S(20) - S(240), y + (S(PR_HEAD_H) - S(32)) * 0.5f, S(240), S(32) } : (Box){ 0 };
    f32 ry = y + S(PR_HEAD_H);
    PR.park_sw = (Box){ x1 - S(24) - S(52), ry + (S(PR_ROW_H) - S(32)) * 0.5f, S(52), S(32) };
    memset(PR.slider_row, 0, sizeof PR.slider_row);
    for (int k = 0; k < nr; k++) {
        f32 top = ry + S(PR_ROW_H) * (f32)(k + 1);
        PR.slider_row[rows[k]] = top;
        PR.slider[rows[k]] = (Box){ x1 - S(24) - S(64) - S(240), top + S(PR_ROW_H) * 0.5f, S(240), S(1) };
    }
    y += PR.set.h + gap;

    int n = PM.n ? MIN(PM.n, A.cpu.count ? A.cpu.count : PM.n) : A.cpu.count;
    PR.ncells = n;
    f32 cgap = S(8), inner = cw - S(40);
    int cols = MAX(4, (int)((inner + cgap) / (S(112) + cgap)));
    cols = MIN(cols, MAX(1, n));
    f32 cwid = floorf((inner - cgap * (f32)(cols - 1)) / (f32)cols);
    int nrow = (n + cols - 1) / MAX(1, cols);
    PR.cores = (Box){ x0, y, cw, S(PR_HEAD_H) + (S(PR_CELL_H) + cgap) * (f32)nrow - cgap + S(20) };
    for (int i = 0; i < n; i++)
        PR.cell[i] = (Box){ x0 + S(20) + (cwid + cgap) * (f32)(i % cols), y + S(PR_HEAD_H) + (S(PR_CELL_H) + cgap) * (f32)(i / cols), cwid, S(PR_CELL_H) };
    y += PR.cores.h;
    PR.content_h = y - y0 + S(16);

}

static void pr_card(const Box *b)
{
    draw_card(b);
}

static void pr_head(const Box *b, u32 glyph, const WCHAR *title, const WCHAR *sub, f32 right, u32 subcol)
{
    const Theme *t = &A.th;
    f32 isz = S(36), ix = b->x + S(20), cy = b->y + S(PR_HEAD_H) * 0.5f, tx = ix + isz + S(16);
    draw_icon_tile(ix, cy - isz * 0.5f, isz, glyph);
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(15), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(15), cy - S(9)), right - tx - S(12), title, -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(12.5f), tx, baseline_for(FONT_TEXT, SC(12.5f), cy + S(11)), right - tx - S(12), sub, -1, subcol ? subcol : t->dim, false);
}

// A Windows 11 slider: the track, the accent part up to the knob, a ring with an accent dot.
static void draw_hslider(const Box *b, f32 v01, bool hov, bool held)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    f32 cy = b->y, th = S(4), kx = floorf(b->x + b->w * CLAMP(v01, 0.f, 1.f) + 0.5f);
    r_rect(b->x, cy - th * 0.5f, b->w, th, t->dark ? RGBA(255, 255, 255, 60) : RGBA(0, 0, 0, 70), th * 0.5f);
    r_rect(b->x, cy - th * 0.5f, kx - b->x, th, t->accent, th * 0.5f);
    f32 k = S(20);
    r_rect_ex(kx - k * 0.5f, cy - k * 0.5f + S(1), k, k, RGBA(0, 0, 0, t->dark ? 90 : 40), k * 0.5f, 0, S(2));
    r_rect(kx - k * 0.5f, cy - k * 0.5f, k, k, c->knob, k * 0.5f);
    r_rect_ex(kx - k * 0.5f, cy - k * 0.5f, k, k, c->ctl_border, k * 0.5f, 1.f, 0);
    f32 hk = hover_amt(b, hov || held), d = held ? S(10) : S(12) + S(2) * hk;
    r_rect(kx - d * 0.5f, cy - d * 0.5f, d, d, t->accent, d * 0.5f);
}

static void draw_pr_plans(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *b = &PR.plans;
    pr_card(b);
    pr_head(b, 0xE945, TR("Схемы электропитания", "Power plans"),
            PR.add.w ? TR("Схемы MixBoost здесь нет: её можно добавить, она сразу станет активной", "The MixBoost plan isn't here: add it and it turns on at once")
                     : TR("Выберите схему, чтобы настроить её ниже. «Включить» делает её активной", "Pick a plan to set it up below. Turn on makes it the active one"),
            PR.add.w ? PR.add.x : b->x + b->w - S(20), 0);
    if (PR.add.w)
        draw_button(&PR.add, TR("Добавить схему MixBoost", "Add the MixBoost plan"), 0xE710, BTN_ACCENT, hover_amt(&PR.add, A.hover.type == HT_PRADD),
                    A.press.type == HT_PRADD && A.hover.type == HT_PRADD, false);
    r_rect(b->x + S(20), b->y + S(PR_HEAD_H) - S(4), b->w - S(40), 1, t->sep, 0);
    for (int i = 0; i < PR.nplan; i++) {
        Box *r = &PR.plan_row[i];
        bool sel = i == PR.sel, act = i == PR.active, hov = A.hover.type == HT_PRPLAN && A.hover.idx == i;
        f32 hk = hover_amt(r, hov && !sel);
        if (sel) {
            r_rect(r->x, r->y + S(2), r->w, r->h - S(4), c->nav_sel, S(8));
            r_rect(r->x + S(4), r->y + r->h * 0.5f - S(8), S(3), S(16), t->accent, S(1.5f));
        } else if (hk > 0) {
            r_rect(r->x, r->y + S(2), r->w, r->h - S(4), color_alpha(c->nav_hover, hk), S(8));
        }
        f32 cy = r->y + r->h * 0.5f, rx = r->x + S(24), ro = S(18);
        // the radio: filled on the active plan
        r_rect_ex(rx - ro * 0.5f, cy - ro * 0.5f, ro, ro, act ? t->accent : t->dim, ro * 0.5f, S(1.5f), 0);
        if (act) r_rect(rx - S(4), cy - S(4), S(8), S(8), t->accent, S(4));
        f32 tx = rx + S(22), right = PR.act_btn[i].x - S(12);
        f32 nx = tx + text_draw_fit(sel ? FONT_TEXT_SEMIBOLD : FONT_TEXT, SC(13.5f), tx, baseline_for(FONT_TEXT, SC(13.5f), cy), right - tx - S(110), PR.plan[i].name, -1,
                               t->text, false);
        if (PR.plan[i].mixboost) {
            const WCHAR *pb = TR("из плейбука", "from the playbook");
            f32 bw = text_width(FONT_TEXT, SC(11.5f), pb, -1) + S(16);
            r_rect(nx + S(10), cy - S(10), bw, S(20), c->ctl, S(10));
            text_center(FONT_TEXT, SC(11.5f), nx + S(10) + bw * 0.5f, cy, pb, t->dim);
        }
        if (act) {
            const WCHAR *lbl = TR("активна", "active");
            f32 bw = text_width(FONT_TEXT_SEMIBOLD, SC(11.5f), lbl, -1) + S(18), bx = r->x + r->w - S(12) - bw;
            r_rect(bx, cy - S(11), bw, S(22), color_alpha(t->accent, 0.18f), S(11));
            text_center(FONT_TEXT_SEMIBOLD, SC(11.5f), bx + bw * 0.5f, cy, lbl, t->accent);
        } else {
            draw_button(&PR.act_btn[i], TR("Включить", "Turn on"), 0, BTN_NEUTRAL, hover_amt(&PR.act_btn[i], A.hover.type == HT_PRACT && A.hover.idx == i),
                        A.press.type == HT_PRACT && A.hover.type == HT_PRACT && A.hover.idx == i, false);
        }
    }
}

static const WCHAR *ps_title(int s)
{
    switch (s) {
    case PS_PARK: return PR.hybrid ? TR("Минимум активных E-ядер", "Minimum active E-cores") : TR("Минимум активных ядер", "Minimum active cores");
    case PS_PARK1: return TR("Минимум активных P-ядер", "Minimum active P-cores");
    case PS_MIN: return TR("Минимальное состояние процессора", "Minimum processor state");
    case PS_MAX: return TR("Максимальное состояние процессора", "Maximum processor state");
    }
    return L"";
}

static const WCHAR *ps_desc(int s)
{
    switch (s) {
    case PS_PARK:
    case PS_PARK1:
        return TR("Сколько ядер Windows держит неспящими. 100% - парковки нет, ядро не тратит время на пробуждение.",
                  "How many cores Windows keeps awake. 100% is no parking: no core spends time waking up.");
    case PS_MIN:
        return TR("Ниже этой частоты процессор не опустится. 100% держит его на максимуме всегда: отклик быстрее, нагрев больше.",
                  "The processor won't clock below this. 100% keeps it at full speed: quicker response, more heat.");
    case PS_MAX:
        return TR("Меньше 100% выключает турбобуст: процессор холоднее и тише, но медленнее.",
                  "Under 100% turns turbo boost off: the processor runs cooler and quieter, but slower.");
    }
    return L"";
}

static void draw_pr_settings(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *b = &PR.set;
    pr_card(b);
    WCHAR title[160];
    wfmt(title, countof(title), TR("Настройки схемы «%s»", "\"%s\" settings"), PR.sel >= 0 && PR.sel < PR.nplan ? PR.plan[PR.sel].name : L"");
    pr_head(b, 0xEEA1, title,
            PR.sel == PR.active ? TR("Это активная схема: изменения работают сразу", "The active plan: changes work at once")
                                : TR("Изменения заработают, когда схема станет активной", "Changes work once the plan is active"),
            PR.seg.w ? PR.seg.x : b->x + b->w - S(20), 0);
    // plugged in / on battery: a segmented control like the language switch
    if (PR.seg.w) {
        Box *s = &PR.seg;
        f32 half = floorf(s->w * 0.5f), in = S(3), st = MIN(fabsf(PR.seg_v) * S(3), S(14));
        f32 px = floorf(s->x + in + (half - in) * PR.seg_t - st * 0.5f + 0.5f);
        r_rect(s->x, s->y, s->w, s->h, c->ctl, S(8));
        r_rect(px, s->y + in, half - in + floorf(st + 0.5f), s->h - in * 2, t->dark ? RGBA(255, 255, 255, 30) : RGBA(255, 255, 255, 255), S(6));
        for (int k = 0; k < 2; k++) {
            bool on = k == PR.dc;
            text_center(on ? FONT_TEXT_SEMIBOLD : FONT_TEXT, SC(12.5f), s->x + half * (f32)k + half * 0.5f, s->y + s->h * 0.5f,
                        k ? TR("От батареи", "On battery") : TR("От сети", "Plugged in"), on ? t->text : t->dim);
        }
    }
    r_rect(b->x + S(20), b->y + S(PR_HEAD_H) - S(4), b->w - S(40), 1, t->sep, 0);

    // parking on or off
    Box row = { b->x, b->y + S(PR_HEAD_H), b->w, S(PR_ROW_H) };
    bool on = pr_parking_on();
    draw_set_row(&row, PR.park_sw.x - S(16), 0xE708, TR("Парковка ядер", "Core parking"),
                 TR("Windows усыпляет простаивающие ядра. Выключение убирает задержку их пробуждения в играх, в простое ПК тратит чуть больше энергии.",
                    "Windows puts idle cores to sleep. Off removes their wake-up delay in games; at idle the PC uses a little more power."),
                 on ? TR("Включена", "On") : TR("Выключена: все ядра всегда активны", "Off: every core always awake"), on ? t->dim : t->good);
    draw_switch(&PR.park_sw, PR.tog_park, true, A.hover.type == HT_PRPARK);
    int rows[PS__COUNT], nr = pr_rows(rows);
    for (int k = 0; k < nr; k++) {
        int s = rows[k];
        r_rect(b->x + S(20), PR.slider_row[s], b->w - S(40), 1, color_alpha(t->sep, 0.7f), 0);
        Box r = { b->x, PR.slider_row[s], b->w, S(PR_ROW_H) };
        f32 v = PR.drag == s ? PR.drag_v : (f32)PR.val[s];
        WCHAR note[64] = L"";
        u32 nc = t->dim;
        if ((s == PS_PARK || s == PS_PARK1) && v >= 99.5f) wcopy(note, countof(note), TR("Парковки нет", "No parking")), nc = t->good;
        if (s == PS_MAX && v < 99.5f) wcopy(note, countof(note), TR("Турбобуст выключен", "Turbo boost off")), nc = t->warn;
        draw_set_row(&r, PR.slider[s].x - S(28), s == PS_MAX ? 0xEC4A : s == PS_MIN ? 0xEC48 : 0xE950, ps_title(s), ps_desc(s), note, nc);
        draw_hslider(&PR.slider[s], v / 100.f, A.hover.type == HT_PRSLIDER && A.hover.idx == s, PR.drag == s);
        WCHAR pct[16];
        wfmt(pct, countof(pct), L"%d%%", (int)(v + 0.5f));
        text_right(FONT_TEXT_SEMIBOLD, SC(14), b->x + b->w - S(24), PR.slider[s].y, pct, t->text);
    }
}

static void draw_pr_cores(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *b = &PR.cores;
    pr_card(b);
    int n = PR.ncells, parked = 0;
    f32 load = 0, mhz = 0;
    for (int i = 0; i < n && PM.n; i++) {
        parked += PM.parked[i];
        load += PM.load[i];
        mhz = MAX(mhz, PM.mhz[i]);
    }
    WCHAR sub[160];
    if (!PM.n) wcopy(sub, countof(sub), PM.ok || !PM.q ? TR("Считываю…", "Reading…") : TR("Счётчики производительности недоступны", "Performance counters aren't available"));
    else if (mhz > 0)
        wfmt(sub, countof(sub), TR("Спят: %d из %d · загрузка %.0f%% · до %.2f ГГц", "Asleep: %d of %d · load %.0f%% · up to %.2f GHz"), parked, n, load / (f32)n,
             mhz / 1000.f);
    else wfmt(sub, countof(sub), TR("Спят: %d из %d · загрузка %.0f%%", "Asleep: %d of %d · load %.0f%%"), parked, n, load / (f32)n);
    pr_head(b, 0xE9D9, TR("Ядра сейчас", "The cores now"), sub, b->x + b->w - S(20), 0);
    for (int i = 0; i < n; i++) {
        Box *cl = &PR.cell[i];
        bool zz = PM.n && PM.parked[i];
        f32 ld = PM.n ? PM.load[i] : 0;
        r_rect(cl->x, cl->y, cl->w, cl->h, zz ? color_alpha(c->ctl, 0.5f) : c->ctl, S(8));
        r_rect_ex(cl->x, cl->y, cl->w, cl->h, c->ctl_border, S(8), 1.f, 0);
        WCHAR nm[16], v[24];
        wfmt(nm, countof(nm), L"CPU %d", i);
        f32 x = cl->x + S(12), cy = cl->y + S(19);
        f32 ex = text_draw(FONT_TEXT_SEMIBOLD, SC(12.5f), x, baseline_for(FONT_TEXT_SEMIBOLD, SC(12.5f), cy), nm, -1, zz ? t->faint : t->text);
        if (A.cpu.hybrid && A.cpu.eff[i] == 0) text_draw(FONT_TEXT_SEMIBOLD, SC(10.5f), ex + S(5), baseline_for(FONT_TEXT_SEMIBOLD, SC(10.5f), cy), L"E", -1, t->dim);
        if (zz) {
            text_draw_icon(0xE708, SC(11), cl->x + cl->w - S(16), cy, t->faint);
        } else if (PM.n) {
            wfmt(v, countof(v), L"%.0f%%", ld);
            text_right(FONT_TEXT, SC(12), cl->x + cl->w - S(12), cy, v, t->dim);
        }
        f32 by = cl->y + cl->h - S(17), bw = cl->w - S(24);
        r_rect(x, by, bw, S(5), t->dark ? RGBA(255, 255, 255, 22) : RGBA(0, 0, 0, 18), S(2.5f));
        if (!zz && ld > 0.5f) r_rect(x, by, MAX(S(5), floorf(bw * ld / 100.f)), S(5), ld > 90.f ? t->warn : t->accent, S(2.5f));
        if (zz) text_draw(FONT_TEXT, SC(11), x, baseline_for(FONT_TEXT, SC(11), by - S(9)), TR("спит", "parked"), -1, t->faint);
    }
}

static void draw_park_page(void)
{
    const Theme *t = &A.th;
    if (!PR.ready) {
        pr_read();
        pm_sample();
    }
    pr_layout();
    f32 top = S(HW_TOP) - S(6);
    r_set_clip(0, top, (f32)A.W, (f32)A.H);
    draw_pr_plans();
    draw_pr_settings();
    draw_pr_cores();
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = pr_max_scroll();
    if (ms > 0 && PR.sb_t > 0) {
        f32 vh = pr_view_h(), bar = MAX(S(32), vh * vh / PR.content_h), by = S(HW_TOP) + (vh - bar) * CLAMP(PR.scroll / ms, 0.f, 1.f);
        r_rect((f32)A.W - S(9), floorf(by), S(3), floorf(bar), color_alpha(t->faint, PR.sb_t), S(1.5f));
    }
}

static bool pr_animate(f32 dt)
{
    bool anim = false;
    anim |= spring(&PR.tog_park, &PR.tog_park_v, pr_parking_on() ? 1.f : 0.f, 0.34f, 0.7f, 0.002f, dt);
    anim |= spring(&PR.seg_t, &PR.seg_v, (f32)PR.dc, 0.4f, 0.72f, 0.002f, dt);
    PR.scroll_target = CLAMP(PR.scroll_target, 0.f, pr_max_scroll());
    anim |= spring_step(&PR.scroll, &PR.scroll_v, PR.scroll_target, 24.f, dt);
    bool sb = A.page == PG_PARK && pr_max_scroll() > 0 && A.mx >= content_x0();
    anim |= approach(&PR.sb_t, sb ? 1.f : 0.f, sb ? 20.f : 7.f, dt);
    return anim;
}

static Hit pr_hit(f32 x, f32 y)
{
    if (y < S(HW_TOP) - S(6)) return (Hit){ HT_NONE, -1 };
    if (PR.add.w && in_b(&PR.add, x, y)) return (Hit){ HT_PRADD, -1 };
    for (int i = 0; i < PR.nplan; i++) {
        if (i != PR.active && in_b(&PR.act_btn[i], x, y)) return (Hit){ HT_PRACT, i };
        if (in_b(&PR.plan_row[i], x, y)) return (Hit){ HT_PRPLAN, i };
    }
    if (PR.seg.w && in_b(&PR.seg, x, y)) return (Hit){ HT_PRSEG, x < PR.seg.x + PR.seg.w * 0.5f ? 0 : 1 };
    if (in_b(&PR.park_sw, x, y)) return (Hit){ HT_PRPARK, -1 };
    for (int s = 0; s < PS__COUNT; s++) {
        if (!PR.slider_row[s]) continue;
        Box sl = { PR.slider[s].x - S(12), PR.slider[s].y - S(16), PR.slider[s].w + S(24), S(32) };
        if (in_b(&sl, x, y)) return (Hit){ HT_PRSLIDER, s };
    }
    return (Hit){ HT_NONE, -1 };
}

static void pr_drag(int mx)
{
    if (PR.drag < 0) return;
    Box *b = &PR.slider[PR.drag];
    PR.drag_v = floorf(CLAMP(((f32)mx - b->x) / b->w, 0.f, 1.f) * 100.f + 0.5f);
    invalidate();
}

// The held slider let go: its value goes into the plan, with min and max state kept in order.
static void pr_drop(void)
{
    if (PR.drag < 0) return;
    int s = PR.drag;
    PR.drag = -1;
    if (PR.sel < 0 || PR.sel >= PR.nplan) return;
    const GUID *g = &PR.plan[PR.sel].id;
    DWORD v = (DWORD)PR.drag_v;
    bool dc = PR.dc != 0;
    pk_write(g, s, dc, v);
    if (s == PS_MIN && v > PR.val[PS_MAX]) pk_write(g, PS_MAX, dc, v);
    if (s == PS_MAX && v < PR.val[PS_MIN]) pk_write(g, PS_MIN, dc, v);
    log_msg("park: %ls %d = %lu (%s)", PR.plan[PR.sel].name, s, v, dc ? "dc" : "ac");
    pr_values();
}

static void pr_press(Hit h, int mx)
{
    if (h.type != HT_PRSLIDER) return;
    PR.drag = h.idx;
    pr_drag(mx);
}

static void pr_click(Hit h)
{
    switch (h.type) {
    case HT_PRPLAN:
        PR.sel = h.idx;
        pr_values();
        break;
    case HT_PRACT:
        if (h.idx >= 0 && h.idx < PR.nplan) {
            PowerSetActiveScheme(NULL, &PR.plan[h.idx].id);
            PR.sel = h.idx;
            pr_read();
        }
        break;
    case HT_PRADD:
        if (!pk_has_plan(&k_plan_mixboost) && pk_add_mixboost()) {
            PR.sel = -1;
            pr_read();
        }
        break;
    case HT_PRSEG:
        PR.dc = h.idx;
        pr_values();
        break;
    case HT_PRPARK:
        if (PR.sel >= 0 && PR.sel < PR.nplan) {
            bool on = !pr_parking_on();
            PR.park_want = on;
            PR.park_plan = PR.plan[PR.sel].id;
            PR.park_dc = PR.dc;
            PR.park_pend++;
            Job j = { pr_park_run, pr_park_done, 0, ++PR.park_seq, on };
            j.u.park.plan = PR.park_plan;
            j.u.park.dc = PR.dc != 0;
            job_post(A.hwnd, &j);
        }
        break;
    }
}

// Once a second while the page is up: the cores, and the active plan (Windows or another app may have switched it).
static void pr_tick(void)
{
    pm_sample();
    GUID *act = NULL;
    if (PowerGetActiveScheme(NULL, &act) == ERROR_SUCCESS) {
        int i = pr_plan_index(act);
        if (i != PR.active) PR.active = i;
        LocalFree(act);
    }
}
