// The "MSI Mode" page: every PCI device with an interrupt, what it supports and runs on now, a switch for MSI and a
// menu for the interrupt priority. Edits wait for "Apply"; the data and the registry work are in msi.c.

static struct {
    f32 scroll, scroll_target, scroll_v, content_h, sb_t;
    f32 tog[MSI_MAX];          // the switches' knobs, 0 off .. 1 on
    f32 togv[MSI_MAX];
    f32 row_y[MSI_MAX];        // each device's row, in content coordinates
    f32 grp_y[MG_SYSTEM + 1];  // each group's header, -1 when the group is empty
    int dd;                    // the device whose priority menu is open, -1 none
    f32 dd_t;
    Box dd_box, dd_item[4];
    Box card, table, rows, apply, discard, resetall, reboot;
    Box tog_box[MSI_MAX], prio_box[MSI_MAX];
    bool applied;              // settings were written this session: a reboot is due
    bool pb_task;              // the playbook's "MixBoost MSI Mode" task is there
    int pend_dev, pend_val;    // a disk controller's change, waiting behind the warning
    WCHAR note[160];
    bool note_bad;
} MP = { .dd = -1, .pend_dev = -1 };

#define MSI_ROW_H 54.f
#define MSI_GRP_H 36.f

static const int k_prio_vals[4] = { -1, 1, 2, 3 };

static const WCHAR *prio_name(int v)
{
    switch (v) {
    case 1: return TR("Низкий", "Low");
    case 2: return TR("Обычный", "Normal");
    case 3: return TR("Высокий", "High");
    }
    return TR("Не задан", "Undefined");
}

static const WCHAR *msi_group_name(int g)
{
    switch (g) {
    case MG_GPU: return TR("Видеокарты", "Graphics");
    case MG_NET: return TR("Сеть", "Network");
    case MG_USB: return L"USB";
    case MG_AUDIO: return TR("Звук", "Audio");
    case MG_DISK: return TR("Накопители", "Storage");
    case MG_SYSTEM: return TR("Системные", "System");
    }
    return TR("Другие", "Other");
}

static void msi_rescan(void)
{
    MS.n = msi_scan(MS.d, MSI_MAX);
    MS.ready = true;
    MP.pb_task = pb_task_present();
    for (int i = 0; i < MS.n; i++) MP.tog[i] = msi_shown_on(&MS.d[i]) ? 1.f : 0.f;
    MP.dd = -1;
}

static int msi_dirty_count(void)
{
    int n = 0;
    for (int i = 0; i < MS.n; i++) n += msi_dirty(&MS.d[i]);
    return n;
}

static int msi_managed_count(void)
{
    int n = 0;
    for (int i = 0; i < MS.n; i++) n += MS.d[i].managed;
    return n;
}

static bool msi_reboot_due(void)
{
    if (MP.applied) return true;
    for (int i = 0; i < MS.n; i++)
        if (MS.d[i].managed && msi_pending(&MS.d[i])) return true;
    return false;
}

// An edit that lands back on what the registry has is no edit: a value that was absent stays absent.
static void msi_set_msi(MsiDev *m, int v)
{
    m->new_msi = m->msi < 0 && v == (m->irq_msi ? 1 : 0) ? -1 : v;
}

static void msi_set_prio(MsiDev *m, int v)
{
    m->new_prio = v < 0 && m->prio <= 0 ? m->prio : v;
}

static f32 msi_view_h(void) { return MP.rows.h; }
static f32 msi_max_scroll(void) { return MAX(0.f, MP.content_h - msi_view_h()); }

// The columns, from the right: priority, the switch, what runs now, what is supported, then the name.
typedef struct MsiCols {
    f32 name, name_w, sup, now, sw, prio, prio_w;
} MsiCols;

static MsiCols msi_cols(void)
{
    MsiCols c;
    f32 xr = MP.table.x + MP.table.w - S(20);
    c.prio_w = S(132);
    c.prio = xr - c.prio_w;
    c.sw = c.prio - S(28) - S(40);
    c.now = c.sw - S(28) - S(128);
    c.sup = c.now - S(16) - S(100);
    c.name = MP.table.x + S(20);
    c.name_w = c.sup - S(16) - c.name;
    return c;
}

static void msi_layout(void)
{
    f32 x0 = content_x0(), cw = content_x1() - x0, y = S(HW_TOP);
    MP.card = (Box){ x0, y, cw, S(76) };
    y += MP.card.h + S(14);
    MP.table = (Box){ x0, y, cw, MAX(S(120), (f32)A.H - S(16) - y) };
    MP.rows = (Box){ x0, y + S(42), cw, MP.table.h - S(42) - S(6) };

    // the buttons on the card, right to left
    f32 bx = MP.card.x + MP.card.w - S(20), by = MP.card.y + (MP.card.h - S(34)) * 0.5f;
    int dirty = msi_dirty_count();
    MP.apply = MP.discard = MP.reboot = MP.resetall = (Box){ 0 };
    if (dirty) {
        MP.apply = (Box){ bx - S(150), by, S(150), S(34) };
        MP.discard = (Box){ MP.apply.x - S(8) - S(120), by, S(120), S(34) };
    } else {
        if (msi_reboot_due()) {
            MP.reboot = (Box){ bx - S(170), by, S(170), S(34) };
            bx = MP.reboot.x - S(8);
        }
        if (msi_managed_count()) MP.resetall = (Box){ bx - S(150), by, S(150), S(34) };
    }

    // the rows: a header per group, then its devices
    f32 cy = 0;
    for (int g = 0; g <= MG_SYSTEM; g++) MP.grp_y[g] = -1;
    for (int i = 0; i < MS.n; i++) {
        int g = MS.d[i].group;
        if (MP.grp_y[g] < 0) {
            MP.grp_y[g] = cy;
            cy += S(MSI_GRP_H);
        }
        MP.row_y[i] = cy;
        cy += S(MSI_ROW_H);
    }
    MP.content_h = cy + S(8);
    MsiCols c = msi_cols();
    f32 off = MP.rows.y - floorf(MP.scroll + 0.5f);
    for (int i = 0; i < MS.n; i++) {
        f32 ry = off + MP.row_y[i] + S(MSI_ROW_H) * 0.5f;
        MP.tog_box[i] = (Box){ c.sw - S(6), ry - S(16), S(52), S(32) };
        MP.prio_box[i] = (Box){ c.prio, ry - S(16), c.prio_w, S(32) };
    }
    // the priority menu under its button, or over it when the window ends first
    if (MP.dd >= 0) {
        Box *pb = &MP.prio_box[MP.dd];
        f32 ih = S(34), mh = ih * 4 + S(8), my = pb->y + pb->h + S(4);
        if (my + mh > (f32)A.H - S(10)) my = pb->y - S(4) - mh;
        MP.dd_box = (Box){ pb->x, my, pb->w, mh };
        for (int k = 0; k < 4; k++) MP.dd_item[k] = (Box){ pb->x + S(4), my + S(4) + ih * (f32)k, pb->w - S(8), ih };
    }
}

static void draw_switch(const Box *hit, f32 on, bool enabled, bool hov)
{
    const Theme *t = &A.th;
    // An iOS-style switch: a filled track, a white knob with a soft shadow that rides inside it.
    f32 w = S(44), h = S(24), x = hit->x + S(4), y = floorf(hit->y + (hit->h - h) * 0.5f), in = S(2), d = h - in * 2;
    u32 off_track = t->dark ? RGBA(255, 255, 255, 38) : RGBA(0, 0, 0, 28);
    if (!enabled) {
        r_rect(x, y, w, h, color_alpha(off_track, 0.5f), h * 0.5f);
        r_rect(x + in, y + in, d, d, t->dark ? RGBA(255, 255, 255, 50) : RGBA(255, 255, 255, 170), d * 0.5f);
        return;
    }
    // on runs a little past 0..1 while the knob's spring settles
    f32 o = CLAMP(on, 0.f, 1.f), hv = hover_amt(hit, hov);
    // the mouse over it: the track a shade brighter
    u32 track = color_mix(off_track, t->accent, o);
    u32 lift = t->dark ? RGBA(255, 255, 255, 255) : RGBA(0, 0, 0, 255);
    r_rect(x, y, w, h, color_mix(track, color_alpha(lift, (f32)(track >> 24) / 255.f), (t->dark ? 0.1f : 0.06f) * hv), h * 0.5f);
    // held, the knob stretches toward the side it will go to, as on iOS
    bool held = hov && A.press.type != HT_NONE && A.press.type == A.hover.type && A.press.idx == A.hover.idx;
    f32 sw = S(7) * CLAMP(press_amt(hit, held), 0.f, 1.2f);
    f32 kx = x + in + (w - in * 2 - d) * on - sw * o, ky = y + in;
    r_rect_ex(kx, ky + S(1), d + sw, d, RGBA(0, 0, 0, t->dark ? 90 : 55), d * 0.5f, 0, S(2));
    r_rect(kx, ky, d + sw, d, RGBA(255, 255, 255, 255), d * 0.5f);
    if (!t->dark) r_rect_ex(kx, ky, d + sw, d, RGBA(0, 0, 0, 14), d * 0.5f, 1.f, 0);
}

static void draw_msi_card(void)
{
    const Theme *t = &A.th;
    Box *b = &MP.card;
    draw_card(b);
    int dirty = msi_dirty_count(), managed = msi_managed_count();
    bool busy = g_task_busy != 0, reboot = !dirty && msi_reboot_due();
    u32 icon = dirty ? 0xE70F : reboot ? 0xE777 : 0xE9E9, col = reboot ? t->warn : t->accent;
    WCHAR title[96], sub[200];
    if (dirty) {
        wfmt(title, countof(title), TR("Изменено устройств: %d", "Devices changed: %d"), dirty);
        if (MP.pb_task)
            wcopy(sub, countof(sub), TR("«Применить» заменит задачу плейбука MixBoost MSI Mode службой MixBooster, настройки заработают после перезагрузки",
                                        "Apply replaces the playbook's MixBoost MSI Mode task with the MixBooster service; it takes effect after a restart"));
        else
            wcopy(sub, countof(sub), TR("Нажмите «Применить», настройки заработают после перезагрузки ПК",
                                        "Press Apply; the settings take effect after a restart"));
    } else if (reboot) {
        wcopy(title, countof(title), TR("Нужна перезагрузка", "A restart is due"));
        wcopy(sub, countof(sub), TR("Новые настройки прерываний заработают после перезагрузки ПК", "The new interrupt settings take effect after a restart"));
    } else if (managed) {
        wcopy(title, countof(title), TR("Настройки прерываний сохранены", "Interrupt settings saved"));
        wfmt(sub, countof(sub),
             TR("Служба следит за устройствами (%d): после обновления драйверов настройки вернутся сами",
                "The service keeps %d device(s): the settings come back by themselves after driver updates"),
             managed);
    } else if (MP.pb_task) {
        wcopy(title, countof(title), TR("Настройки от плейбука MixBoost", "Settings from the MixBoost playbook"));
        wcopy(sub, countof(sub), TR("Их держит задача MixBoost MSI Mode. Первое «Применить» передаст их службе MixBooster",
                                    "The MixBoost MSI Mode task keeps them. The first Apply hands them over to the MixBooster service"));
    } else {
        wcopy(title, countof(title), TR("Режим прерываний MSI", "MSI interrupt mode"));
        wcopy(sub, countof(sub), TR("MSI снижает задержку прерываний. Включайте его устройствам, которые его поддерживают",
                                    "MSI cuts interrupt latency. Turn it on for the devices that support it"));
    }
    if (MP.note[0] && !dirty) wcopy(sub, countof(sub), MP.note);
    f32 isz = S(44), cy = b->y + b->h * 0.5f, ix = b->x + S(18);
    r_rect(ix, cy - isz * 0.5f, isz, isz, color_alpha(col, 0.16f), S(10));
    text_draw_icon(icon, SC(18), ix + isz * 0.5f, cy, col);
    f32 left = MP.discard.w ? MP.discard.x : MP.resetall.w ? MP.resetall.x : MP.reboot.w ? MP.reboot.x : b->x + b->w - S(20);
    f32 tx = ix + isz + S(16), tmax = left - S(16) - tx;
    text_draw_fit(FONT_TEXT_SEMIBOLD, SC(16), tx, baseline_for(FONT_TEXT_SEMIBOLD, SC(16), cy - S(10)), tmax, title, -1, t->text, false);
    text_draw_fit(FONT_TEXT, SC(12.5f), tx, baseline_for(FONT_TEXT, SC(12.5f), cy + S(12)), tmax, sub, -1,
                  MP.note[0] && MP.note_bad && !dirty ? t->danger : t->dim, false);
    Hit *h = &A.hover;
    if (MP.apply.w)
        draw_button(&MP.apply, busy ? TR("Применяю…", "Applying…") : TR("Применить", "Apply"), 0xE73E, BTN_ACCENT, A.hv[HV_MSIAPPLY],
                    A.press.type == HT_MSIAPPLY && h->type == HT_MSIAPPLY, busy);
    if (MP.discard.w)
        draw_button(&MP.discard, TR("Отменить", "Discard"), 0, BTN_NEUTRAL, A.hv[HV_MSIDISCARD], A.press.type == HT_MSIDISCARD && h->type == HT_MSIDISCARD,
                    busy);
    if (MP.reboot.w)
        draw_button(&MP.reboot, TR("Перезагрузить", "Restart"), 0xE777, BTN_ACCENT, A.hv[HV_MSIREBOOT], A.press.type == HT_MSIREBOOT && h->type == HT_MSIREBOOT,
                    busy);
    if (MP.resetall.w)
        draw_button(&MP.resetall, busy ? TR("Сбрасываю…", "Resetting…") : TR("Сбросить всё", "Reset all"), MP.reboot.w ? 0 : 0xE777, BTN_NEUTRAL,
                    A.hv[HV_MSIRESET], A.press.type == HT_MSIRESET && h->type == HT_MSIRESET, busy);
}

static void draw_msi_table(void)
{
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    Box *b = &MP.table;
    draw_card(b);
    MsiCols k = msi_cols();
    f32 hy = b->y + S(21), hfs = SC(12.5f);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, k.name, baseline_for(FONT_TEXT, hfs, hy), TR("Устройство", "Device"), -1, t->dim);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, k.sup, baseline_for(FONT_TEXT, hfs, hy), TR("Поддержка", "Supports"), -1, t->dim);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, k.now, baseline_for(FONT_TEXT, hfs, hy), TR("Сейчас", "Now"), -1, t->dim);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, k.sw, baseline_for(FONT_TEXT, hfs, hy), L"MSI", -1, t->dim);
    text_draw(FONT_TEXT_SEMIBOLD, hfs, k.prio, baseline_for(FONT_TEXT, hfs, hy), TR("Приоритет", "Priority"), -1, t->dim);
    r_rect(b->x + S(20), MP.rows.y - S(2), b->w - S(40), 1, t->sep, 0);

    if (!MS.n) {
        text_center(FONT_TEXT, SC(13), b->x + b->w * 0.5f, MP.rows.y + S(60), MS.ready ? TR("Устройства PCI не найдены", "No PCI devices found") : TR("Читаю устройства…", "Reading the devices…"), t->dim);
        return;
    }
    f32 off = MP.rows.y - floorf(MP.scroll + 0.5f);
    r_set_clip(b->x, MP.rows.y, b->x + b->w, MP.rows.y + MP.rows.h);
    for (int g = 0; g <= MG_SYSTEM; g++) {
        if (MP.grp_y[g] < 0) continue;
        f32 gy = off + MP.grp_y[g] + S(MSI_GRP_H) * 0.5f + S(4);
        if (gy < MP.rows.y - S(40) || gy > MP.rows.y + MP.rows.h + S(40)) continue;
        text_draw(FONT_TEXT_SEMIBOLD, SC(13), k.name, baseline_for(FONT_TEXT_SEMIBOLD, SC(13), gy), msi_group_name(g), -1, t->accent);
    }
    bool menu = MP.dd >= 0 || A.modal || A.picker || D.open;
    for (int i = 0; i < MS.n; i++) {
        MsiDev *m = &MS.d[i];
        f32 y = off + MP.row_y[i], cy = y + S(MSI_ROW_H) * 0.5f;
        if (y + S(MSI_ROW_H) < MP.rows.y || y > MP.rows.y + MP.rows.h) continue;
        bool row_hov = !menu && A.my >= (int)y && A.my < (int)(y + S(MSI_ROW_H)) && in_b(&MP.rows, (f32)A.mx, (f32)A.my);
        f32 rk = hover_amt(&MS.d[i], row_hov);
        if (rk > 0) r_rect(b->x + S(6), y + S(2), b->w - S(12), S(MSI_ROW_H) - S(4), color_alpha(c->nav_hover, rk), S(6));
        bool dirty = msi_dirty(m);
        // the name, with a dot while edited, and the PCI id under it
        f32 nx = k.name;
        if (dirty) {
            f32 d = S(6);
            r_rect(nx, floorf(cy - S(9) - d * 0.5f), d, d, t->accent, d * 0.5f);
            nx += d + S(8);
        }
        // a badge right after the name: the GPU's PCI port, or a warning on a disk controller
        u32 badge = m->gpu_bridge ? 0xF211 : m->group == MG_DISK ? 0xE7BA : 0;
        f32 bw = badge ? S(26) : 0, avail = k.name_w - (nx - k.name) - bw;
        f32 nw = MIN(text_width(FONT_TEXT, SC(13.5f), m->name, -1), avail);
        text_draw_fit(FONT_TEXT, SC(13.5f), nx, baseline_for(FONT_TEXT, SC(13.5f), cy - S(9)), avail, m->name, -1, t->text, false);
        if (badge) {
            f32 bx = nx + nw + S(16), by = cy - S(9);
            if (m->gpu_bridge) {
                r_rect(bx - S(11), by - S(9), S(22), S(18), color_alpha(t->accent, 0.18f), S(5));
                text_draw_icon(badge, SC(11), bx, by, t->accent);
            } else {
                text_draw_icon(badge, SC(12), bx, by, t->warn);
            }
        }
        WCHAR sub[220];
        wcopy(sub, countof(sub), m->id);
        WCHAR *cut = wcsstr(sub, L"&SUBSYS");
        if (cut) *cut = 0;
        int l = wlen(sub);
        if (m->gpu_bridge == 1) l += wfmt(sub + l, countof(sub) - l, TR(" · порт видеокарты", " · the GPU's port"));
        else if (m->gpu_bridge) l += wfmt(sub + l, countof(sub) - l, TR(" · шина видеокарты", " · on the GPU's path"));
        if (m->group == MG_DISK) l += wfmt(sub + l, countof(sub) - l, TR(" · лучше не трогать", " · best left alone"));
        if (m->managed) wfmt(sub + l, countof(sub) - l, TR(" · под защитой службы", " · kept by the service"));
        text_draw_fit(FONT_TEXT, SC(11.5f), k.name, baseline_for(FONT_TEXT, SC(11.5f), cy + S(10)), k.name_w, sub, -1, t->faint, false);

        // what the device can do
        WCHAR sup[48] = { 0 };
        int n = 0;
        if (m->support & IRQS_MSIX) n += wfmt(sup + n, countof(sup) - n, L"MSI-X");
        if (m->support & IRQS_MSI) n += wfmt(sup + n, countof(sup) - n, n ? L" · MSI" : L"MSI");
        if (!n) wcopy(sup, countof(sup), m->irq_msi ? L"MSI" : m->support & IRQS_LINE ? TR("Только IRQ", "IRQ only") : L"-");
        text_draw_fit(FONT_TEXT, SC(12.5f), k.sup, baseline_for(FONT_TEXT, SC(12.5f), cy), k.now - S(12) - k.sup, sup, -1,
                      msi_capable(m) ? t->text : t->faint, false);

        // what it runs on now, and a note when that changes on the next boot
        WCHAR now[48];
        u32 nc = t->dim;
        if (m->irq_msi) {
            if (m->nirq > 1) wfmt(now, countof(now), TR("MSI · %d сообщ.", "MSI · %d msgs"), m->nirq);
            else wcopy(now, countof(now), L"MSI");
            nc = t->good;
        } else if (m->has_irq) {
            wfmt(now, countof(now), L"IRQ %d", m->irq);
        } else {
            wcopy(now, countof(now), L"-");
        }
        bool next = (m->new_msi != m->msi && msi_shown_on(m) != m->irq_msi) || (!dirty && msi_pending(m));
        f32 ny = next ? cy - S(8) : cy;
        text_draw_fit(FONT_TEXT, SC(12.5f), k.now, baseline_for(FONT_TEXT, SC(12.5f), ny), k.sw - S(16) - k.now, now, -1, nc, false);
        if (next) {
            WCHAR nx2[64];
            wfmt(nx2, countof(nx2), TR("→ %s после перезагрузки", "→ %s after a restart"), msi_shown_on(m) ? L"MSI" : L"IRQ");
            text_draw_fit(FONT_TEXT, SC(11.5f), k.now, baseline_for(FONT_TEXT, SC(11.5f), cy + S(10)), k.sw - S(16) - k.now, nx2, -1,
                          dirty ? t->accent : t->warn, false);
        }

        bool cap = msi_capable(m);
        draw_switch(&MP.tog_box[i], MP.tog[i], cap, cap && A.hover.type == HT_MSITOG && A.hover.idx == i);

        // the priority: a drop-down button
        Box *pb = &MP.prio_box[i];
        bool phov = A.hover.type == HT_MSIPRIO && A.hover.idx == i, open = MP.dd == i;
        r_rect(pb->x, pb->y, pb->w, pb->h, color_mix(c->ctl, c->ctl_hover, hover_amt(pb, phov || open)), S(6));
        r_rect_ex(pb->x, pb->y, pb->w, pb->h, c->ctl_border, S(6), 1.f, 0);
        int pv = m->new_prio;
        text_draw_fit(FONT_TEXT, SC(13), pb->x + S(12), baseline_for(FONT_TEXT, SC(13), cy), pb->w - S(40), prio_name(pv), -1, pv > 0 ? t->text : t->dim,
                      false);
        text_draw_icon(0xE70D, SC(10), pb->x + pb->w - S(16), cy, t->dim);

        int last = i + 1 < MS.n && MS.d[i + 1].group == m->group;
        if (last && !row_hov) r_rect(b->x + S(20), y + S(MSI_ROW_H) - 1, b->w - S(40), 1, color_alpha(t->sep, 0.6f), 0);
    }
    r_set_clip(0, 0, (f32)A.W, (f32)A.H);
    f32 ms = msi_max_scroll();
    if (ms > 0 && MP.sb_t > 0) {
        f32 vh = msi_view_h(), bar = MAX(S(32), vh * vh / MP.content_h), by = MP.rows.y + (vh - bar) * CLAMP(MP.scroll / ms, 0.f, 1.f);
        r_rect(b->x + b->w - S(7), floorf(by), S(3), floorf(bar), color_alpha(t->faint, MP.sb_t), S(1.5f));
    }
}

static void draw_msi_menu(void)
{
    if (MP.dd < 0 || MP.dd >= MS.n) return;
    const Theme *t = &A.th;
    const Colors *c = &A.c;
    f32 base = R.opacity, k = ease_out_quint(MP.dd_t);
    R.opacity = base * CLAMP(k * 1.4f, 0.f, 1.f);
    Box *b = &MP.dd_box;
    // the menu unfolds from the button it belongs to
    f32 ay = MP.dd_box.y < MP.prio_box[MP.dd].y ? b->y + b->h : b->y;
    RXf xf = r_xf_push(0.94f + 0.06f * k, b->x + b->w * 0.5f, ay, 0, 0);
    r_rect_ex(b->x, b->y + S(4), b->w, b->h, c->shadow, S(8), 0, S(16));
    r_rect(b->x, b->y, b->w, b->h, t->dark ? RGBA(44, 44, 47, 255) : RGBA(251, 251, 252, 255), S(8));
    r_rect_ex(b->x, b->y, b->w, b->h, c->pop_border, S(8), 1.f, 0);
    int cur = MS.d[MP.dd].new_prio;
    for (int i = 0; i < 4; i++) {
        Box *it = &MP.dd_item[i];
        bool sel = k_prio_vals[i] == cur || (i == 0 && cur <= 0), hov = A.hover.type == HT_MSIDDITEM && A.hover.idx == i;
        f32 ik = hover_amt(it, hov);
        if (ik > 0 || sel) r_rect(it->x, it->y, it->w, it->h, color_mix(sel ? c->nav_hover : color_alpha(c->ctl_hover, 0.f), c->ctl_hover, ik), S(5));
        if (sel) r_rect(it->x, it->y + it->h * 0.5f - S(8), S(3), S(16), t->accent, S(1.5f));
        f32 cy = it->y + it->h * 0.5f;
        text_draw(FONT_TEXT, SC(13), it->x + S(14), baseline_for(FONT_TEXT, SC(13), cy), prio_name(k_prio_vals[i]), -1, t->text);
    }
    r_xf_pop(xf);
    R.opacity = base;
}

static void draw_msi_page(void)
{
    if (!MS.ready) msi_rescan();
    msi_layout();
    draw_msi_card();
    draw_msi_table();
    draw_msi_menu();
}

static bool msi_animate(f32 dt)
{
    bool anim = false;
    for (int i = 0; i < MS.n; i++) anim |= spring(&MP.tog[i], &MP.togv[i], msi_shown_on(&MS.d[i]) ? 1.f : 0.f, 0.34f, 0.7f, 0.002f, dt);
    anim |= approach(&MP.dd_t, MP.dd >= 0 ? 1.f : 0.f, 18.f, dt);
    MP.scroll_target = CLAMP(MP.scroll_target, 0.f, msi_max_scroll());
    anim |= spring_step(&MP.scroll, &MP.scroll_v, MP.scroll_target, 24.f, dt);
    bool sb = A.page == PG_MSI && msi_max_scroll() > 0 && !A.modal && in_b(&MP.table, (f32)A.mx, (f32)A.my);
    anim |= approach(&MP.sb_t, sb ? 1.f : 0.f, sb ? 20.f : 7.f, dt);
    return anim;
}

// The open priority menu takes every click: on an item, or anywhere else to close it.
static bool msi_menu_hit(f32 x, f32 y, Hit *h)
{
    if (A.page != PG_MSI || MP.dd < 0) return false;
    for (int i = 0; i < 4; i++)
        if (in_b(&MP.dd_item[i], x, y)) {
            *h = (Hit){ HT_MSIDDITEM, i };
            return true;
        }
    *h = (Hit){ in_b(&MP.dd_box, x, y) ? HT_MSIDDPANEL : HT_MSIDDOUT, -1 };
    return true;
}

static Hit msi_hit(f32 x, f32 y)
{
    bool busy = g_task_busy != 0;
    if (!busy) {
        if (MP.apply.w && in_b(&MP.apply, x, y)) return (Hit){ HT_MSIAPPLY, -1 };
        if (MP.discard.w && in_b(&MP.discard, x, y)) return (Hit){ HT_MSIDISCARD, -1 };
        if (MP.reboot.w && in_b(&MP.reboot, x, y)) return (Hit){ HT_MSIREBOOT, -1 };
        if (MP.resetall.w && in_b(&MP.resetall, x, y)) return (Hit){ HT_MSIRESET, -1 };
    }
    if (!in_b(&MP.rows, x, y)) return (Hit){ HT_NONE, -1 };
    for (int i = 0; i < MS.n; i++) {
        if (msi_capable(&MS.d[i]) && in_b(&MP.tog_box[i], x, y)) return (Hit){ HT_MSITOG, i };
        if (in_b(&MP.prio_box[i], x, y)) return (Hit){ HT_MSIPRIO, i };
    }
    return (Hit){ HT_NONE, -1 };
}

static void msi_reboot(void)
{
    if (!reboot_now()) {
        MP.note_bad = true;
        wcopy(MP.note, countof(MP.note), TR("Не удалось перезагрузить ПК, перезагрузите его сами", "Couldn't restart the PC, please restart it yourself"));
    }
}

static void msi_toggle(int i, int v)
{
    msi_set_msi(&MS.d[i], v);
    MP.note[0] = 0;
}

static void msi_click(Hit h)
{
    switch (h.type) {
    case HT_MSITOG: {
        MsiDev *m = &MS.d[h.idx];
        int v = msi_shown_on(m) ? 0 : 1;
        // a disk controller that fails to start can keep Windows from booting: ask first
        if (m->group == MG_DISK) {
            MP.pend_dev = h.idx;
            MP.pend_val = v;
            modal_open_mode(MM_MSIDISK, 0);
        } else {
            msi_toggle(h.idx, v);
        }
        break;
    }
    case HT_MSIPRIO: MP.dd = MP.dd == h.idx ? -1 : h.idx; break;
    case HT_MSIDDITEM:
        if (MP.dd >= 0) msi_set_prio(&MS.d[MP.dd], k_prio_vals[h.idx]);
        MP.note[0] = 0;
        MP.dd = -1;
        break;
    case HT_MSIDDOUT: MP.dd = -1; break;
    case HT_MSIDISCARD:
        for (int i = 0; i < MS.n; i++) MS.d[i].new_msi = MS.d[i].msi, MS.d[i].new_prio = MS.d[i].prio;
        break;
    case HT_MSIAPPLY:
        if (!g_task_busy && msi_dirty_count()) msi_task_start(A.hwnd, MS.d, MS.n);
        break;
    case HT_MSIRESET:
        if (!g_task_busy) msi_task_start(A.hwnd, NULL, 0);
        break;
    case HT_MSIREBOOT: modal_open_mode(MM_REBOOT, 0); break;
    }
}

static int msi_modal_items(ModalItem *it)
{
    if (A.mmode == MM_REBOOT) {
        int n = 0;
        if (A.rb_n) it[n++] = (ModalItem){ 0xE777, TR("Что применится", "What it applies"), A.rb_text };
        it[n++] = (ModalItem){ 0xE74E, TR("Сохраните открытые файлы", "Save your open files"),
                               TR("ПК перезагрузится сразу. Несохранённое в открытых программах пропадёт.",
                                  "The PC restarts right away. Anything unsaved in open apps is lost.") };
        return n;
    }
    it[0] = (ModalItem){ 0xEDA2, TR("Windows может не загрузиться", "Windows may not boot"),
                         TR("Если драйвер контроллера не справится с новым режимом, система не сможет прочитать диск при загрузке. Тогда "
                            "настройку придётся откатывать из среды восстановления.",
                            "If the controller's driver can't handle the new mode, Windows can't read the disk at boot, and the setting has "
                            "to be undone from the recovery environment.") };
    it[1] = (ModalItem){ 0xE946, TR("Обычно его лучше не трогать", "Usually best left alone"),
                         TR("Контроллеры NVMe и так работают в MSI-X. Меняйте, только если точно знаете, зачем.",
                            "NVMe controllers already run on MSI-X. Change it only if you know why.") };
    return 2;
}

static void msi_modal_close(bool ok)
{
    if (A.mmode == MM_REBOOT) {
        if (ok) msi_reboot();
        return;
    }
    if (ok && MP.pend_dev >= 0 && MP.pend_dev < MS.n) msi_toggle(MP.pend_dev, MP.pend_val);
    MP.pend_dev = -1;
}

// Apply or reset finished: read the devices again.
static void msi_done(bool ok)
{
    MP.applied = true;
    msi_rescan();
    MP.note_bad = !ok;
    wcopy(MP.note, countof(MP.note), ok ? L"" : TR("Не удалось запустить службу, подробности в log.txt", "Couldn't start the service, see log.txt"));
}
