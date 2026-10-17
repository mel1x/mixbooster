// "PC health": how the parts are doing, read on a worker thread.
//   drives     NVMe: the drive's own health log; SATA SSD / HDD: SMART attributes and thresholds
//   battery    worn capacity and charge cycles (laptops only)
//   stability  the last 30 days of the System event log: blue screens, WHEA hardware errors, unexpected
//              power-offs, disk errors
//   devices    devices whose driver is in trouble (not the ones switched off on purpose, which the playbook does)
// No sensors that need a kernel driver (CPU temperature, fans): reading those safely isn't possible without one.

#include <winevt.h>
#ifdef _MSC_VER
#pragma comment(lib, "wevtapi")
#endif

#define HL_CARDS 12

static struct {
    volatile LONG ready, busy, again;
    HwCard card[HL_CARDS];
    int ncards;
    HwStat stat[4];
    u8 state;                       // the worst of all: HS_GOOD, HS_WARN or HS_BAD
    int disks, disks_ok, bsod, whea, devices;
} HL;

static HwCard *hl_card(u32 glyph, const WCHAR *cat)
{
    if (HL.ncards >= HL_CARDS) return NULL;
    HwCard *c = &HL.card[HL.ncards++];
    memset(c, 0, sizeof *c);
    c->glyph = glyph;
    wcopy(c->cat, countof(c->cat), cat);
    return c;
}

static void hl_badge(HwCard *c, u8 state, const WCHAR *extra)
{
    c->state = state;
    const WCHAR *s = state == HS_BAD ? TR("Плохо", "Bad") : state == HS_WARN ? TR("Внимание", "Caution") : TR("Хорошо", "Good");
    wfmt(c->badge, countof(c->badge), L"%s%s%s", s, extra && extra[0] ? L" · " : L"", extra ? extra : L"");
    if (state > HL.state) HL.state = state;
}

static void row_state(HwCard *c, u8 state)
{
    if (c->nrows) c->rows[c->nrows - 1].state = state;
}

static u64 le64(const u8 *p)
{
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void fmt_hours(WCHAR *out, int cap, u64 h)
{
    if (h >= 24 * 365) wfmt(out, cap, TR("%llu ч (%.1f г)", "%llu h (%.1f y)"), (unsigned long long)h, (f64)h / (24 * 365));
    else if (h >= 48) wfmt(out, cap, TR("%llu ч (%llu дн)", "%llu h (%llu d)"), (unsigned long long)h, (unsigned long long)(h / 24));
    else wfmt(out, cap, TR("%llu ч", "%llu h"), (unsigned long long)h);
}

// --- drives --------------------------------------------------------------------------------------------------------

// The NVMe SMART / Health Information log page (0x02), 512 bytes.
static bool nvme_log(HANDLE h, u8 *log)
{
    DWORD size = (DWORD)(FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512);
    u8 *buf = (u8 *)calloc(1, size);
    if (!buf) return false;
    STORAGE_PROPERTY_QUERY *q = (STORAGE_PROPERTY_QUERY *)buf;
    STORAGE_PROTOCOL_SPECIFIC_DATA *pd = (STORAGE_PROTOCOL_SPECIFIC_DATA *)q->AdditionalParameters;
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    pd->ProtocolType = ProtocolTypeNvme;
    pd->DataType = NVMeDataTypeLogPage;
    pd->ProtocolDataRequestValue = 2;  // NVME_LOG_PAGE_HEALTH_INFO
    pd->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    pd->ProtocolDataLength = 512;
    DWORD got = 0;
    bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, size, buf, size, &got, NULL) != 0;
    if (ok) {
        STORAGE_PROTOCOL_DATA_DESCRIPTOR *dd = (STORAGE_PROTOCOL_DATA_DESCRIPTOR *)buf;
        STORAGE_PROTOCOL_SPECIFIC_DATA *r = &dd->ProtocolSpecificData;
        ok = r->ProtocolDataOffset >= sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) && r->ProtocolDataLength >= 512 &&
             (u8 *)r + r->ProtocolDataOffset + 512 <= buf + size;
        if (ok) memcpy(log, (u8 *)r + r->ProtocolDataOffset, 512);
    }
    free(buf);
    return ok;
}

static void nvme_rows(HwCard *c, const u8 *log)
{
    u8 crit = log[0], spare = log[3], spare_thr = log[4], used = log[5];
    int temp = (int)(log[1] | (log[2] << 8)) - 273;
    u64 written = le64(log + 48) * 512000ull, cycles = le64(log + 112), hours = le64(log + 128), unsafe = le64(log + 144),
        media = le64(log + 160);
    // NVMe's own verdict: the critical warning bits, the spare blocks, and the wear
    u8 st = HS_GOOD;
    if ((crit & (0x01 | 0x04 | 0x08)) || used >= 100 || (spare_thr && spare < spare_thr)) st = HS_BAD;
    else if ((crit & 0x02) || used >= 90 || media > 0) st = HS_WARN;
    int life = MAX(0, 100 - (int)used);
    WCHAR v[64];
    wfmt(v, countof(v), L"%d%%", life);
    hl_badge(c, st, v);
    row_add(c, false, TR("Ресурс", "Life left"), TR("%d%% (износ %d%%)", "%d%% (%d%% worn)"), life, (int)used);
    row_state(c, used >= 100 ? HS_BAD : used >= 90 ? HS_WARN : HS_GOOD);
    row_add(c, false, TR("Резервные блоки", "Spare blocks"), L"%d%%", (int)spare);
    row_state(c, spare_thr && spare < spare_thr ? HS_BAD : HS_NONE);
    if (temp > -50 && temp < 150) {
        row_add(c, false, TR("Температура", "Temperature"), L"%d °C", temp);
        row_state(c, temp >= 70 ? HS_WARN : HS_NONE);
    }
    fmt_hours(v, countof(v), hours);
    row_add(c, false, TR("Отработал", "Powered on"), L"%s", v);
    fmt_size(v, countof(v), written);
    row_add(c, false, TR("Записано", "Written"), L"%s", v);
    row_add(c, false, TR("Включений", "Power cycles"), L"%llu", (unsigned long long)cycles);
    row_add(c, false, TR("Аварийных выключений", "Unsafe shutdowns"), L"%llu", (unsigned long long)unsafe);
    row_add(c, false, TR("Ошибки носителя", "Media errors"), L"%llu", (unsigned long long)media);
    row_state(c, media ? HS_BAD : HS_GOOD);
}

// SMART through the disk driver: the attribute table and the thresholds, 512 bytes each.
static bool smart_read(HANDLE h, u8 feature, u8 *out)
{
    SENDCMDINPARAMS in;
    memset(&in, 0, sizeof in);
    in.cBufferSize = READ_ATTRIBUTE_BUFFER_SIZE;
    in.irDriveRegs.bFeaturesReg = feature;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = SMART_CMD;
    u8 buf[sizeof(SENDCMDOUTPARAMS) + READ_ATTRIBUTE_BUFFER_SIZE];
    memset(buf, 0, sizeof buf);
    DWORD got = 0;
    if (!DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof in - 1, buf, sizeof buf, &got, NULL)) return false;
    memcpy(out, ((SENDCMDOUTPARAMS *)buf)->bBuffer, 512);
    return true;
}

typedef struct SmartAttr {
    u8 cur, thr;
    u64 raw;
    bool have;
} SmartAttr;

static void sata_rows(HwCard *c, HANDLE h, bool ssd)
{
    u8 val[512], thr[512];
    if (!smart_read(h, READ_ATTRIBUTES, val)) {
        row_add(c, false, L"SMART", L"%s", TR("нет данных", "not available"));
        return;
    }
    bool have_thr = smart_read(h, READ_THRESHOLDS, thr);
    SmartAttr a[256];
    memset(a, 0, sizeof a);
    bool failing = false;
    for (int i = 0; i < 30; i++) {
        const u8 *e = val + 2 + i * 12;
        if (!e[0]) continue;
        SmartAttr *s = &a[e[0]];
        s->have = true;
        s->cur = e[3];
        s->raw = (u64)e[5] | ((u64)e[6] << 8) | ((u64)e[7] << 16) | ((u64)e[8] << 24) | ((u64)e[9] << 32) | ((u64)e[10] << 40);
        if (have_thr)
            for (int j = 0; j < 30; j++) {
                const u8 *t = thr + 2 + j * 12;
                if (t[0] == e[0]) s->thr = t[1];
            }
        // the drive's own pass/fail: a value at or under its threshold
        if (s->thr && s->cur && s->cur <= s->thr) failing = true;
    }
    // SSD life: whichever of the vendors' wear attributes is there (all count down from 100)
    int life = -1;
    static const u8 wear_ids[] = { 231, 169, 177, 233, 202 };
    for (int i = 0; ssd && life < 0 && i < countof(wear_ids); i++)
        if (a[wear_ids[i]].have && a[wear_ids[i]].cur <= 100) life = a[wear_ids[i]].cur;
    u64 realloc_ = a[5].raw & 0xFFFFFFFF, pending = a[197].raw & 0xFFFFFFFF, uncorr = a[198].raw & 0xFFFFFFFF, reported = a[187].raw & 0xFFFF;
    u8 st = HS_GOOD;
    if (failing) st = HS_BAD;
    else if (realloc_ || pending || uncorr || reported || (life >= 0 && life <= 10)) st = HS_WARN;
    WCHAR v[64] = L"";
    if (life >= 0) wfmt(v, countof(v), L"%d%%", life);
    hl_badge(c, st, v);
    if (life >= 0) {
        row_add(c, false, TR("Ресурс", "Life left"), L"%d%%", life);
        row_state(c, life <= 10 ? HS_WARN : HS_GOOD);
    }
    int temp = a[194].have ? (int)(a[194].raw & 0xFF) : a[190].have ? (int)(a[190].raw & 0xFF) : -1;
    if (temp > 0 && temp < 100) {
        row_add(c, false, TR("Температура", "Temperature"), L"%d °C", temp);
        row_state(c, temp >= (ssd ? 70 : 55) ? HS_WARN : HS_NONE);
    }
    if (a[9].have) {
        fmt_hours(v, countof(v), a[9].raw & 0xFFFFFFFF);
        row_add(c, false, TR("Отработал", "Powered on"), L"%s", v);
    }
    if (a[12].have) row_add(c, false, TR("Включений", "Power cycles"), L"%llu", (unsigned long long)(a[12].raw & 0xFFFFFFFF));
    if (a[5].have) {
        row_add(c, false, TR("Переназначенные сектора", "Reallocated sectors"), L"%llu", (unsigned long long)realloc_);
        row_state(c, realloc_ ? HS_WARN : HS_GOOD);
    }
    if (a[197].have) {
        row_add(c, false, TR("Нестабильные сектора", "Pending sectors"), L"%llu", (unsigned long long)pending);
        row_state(c, pending ? HS_WARN : HS_GOOD);
    }
    if (a[198].have) {
        row_add(c, false, TR("Неисправимые ошибки", "Uncorrectable errors"), L"%llu", (unsigned long long)uncorr);
        row_state(c, uncorr ? HS_WARN : HS_GOOD);
    }
    if (a[199].have && (a[199].raw & 0xFFFFFFFF)) {
        // usually a loose or bad SATA cable, not the drive
        row_add(c, false, TR("Ошибки кабеля (CRC)", "Cable errors (CRC)"), L"%llu", (unsigned long long)(a[199].raw & 0xFFFFFFFF));
        row_state(c, HS_WARN);
    }
    if (failing) row_add(c, false, L"SMART", L"%s", TR("диск сообщает о сбое", "the drive reports a failure"));
    if (failing) row_state(c, HS_BAD);
}

static void hl_disks(void)
{
    WCHAR letters[32][24];
    disk_letters(letters);
    for (int i = 0; i < 32; i++) {
        WCHAR path[32];
        wfmt(path, countof(path), L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        bool rw = h != INVALID_HANDLE_VALUE;
        if (!rw) h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery, { 0 } };
        u8 buf[1024];
        DWORD got;
        WCHAR model[96] = L"";
        int bus = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *dd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            char tmp[128] = "";
            if (dd->VendorIdOffset && dd->VendorIdOffset < got) snprintf(tmp, sizeof tmp, "%s ", (const char *)buf + dd->VendorIdOffset);
            if (dd->ProductIdOffset && dd->ProductIdOffset < got) strncat(tmp, (const char *)buf + dd->ProductIdOffset, sizeof tmp - strlen(tmp) - 1);
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, model, countof(model));
            trim(model);
            bus = dd->BusType;
        }
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp = { 0 };
        bool ssd = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &got, NULL) ? !sp.IncursSeekPenalty : bus == BusTypeNvme;
        DISK_GEOMETRY_EX geo;
        u64 size = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof geo, &got, NULL) ? (u64)geo.DiskSize.QuadPart : 0;
        if (!size) {
            CloseHandle(h);
            continue;
        }
        HwCard *c = hl_card(0xEDA2, TR("Накопитель", "Drive"));
        if (!c) {
            CloseHandle(h);
            break;
        }
        wcopy(c->head, countof(c->head), model[0] ? model : path + 4);
        WCHAR sz[32];
        fmt_size(sz, countof(sz), size);
        const WCHAR *bt = bus == BusTypeNvme ? L"NVMe" : bus == BusTypeSata ? L"SATA" : bus == BusTypeUsb ? L"USB" : bus == BusTypeRAID ? L"RAID" : L"";
        wfmt(c->sub, countof(c->sub), L"%s%s%s · %s%s%s", bt, bt[0] ? L" " : L"", ssd ? L"SSD" : L"HDD", sz, letters[i][0] ? L" · " : L"", letters[i]);
        u8 log[512];
        if (bus == BusTypeNvme && nvme_log(h, log)) nvme_rows(c, log);
        else if (rw && (bus == BusTypeSata || bus == BusTypeAta)) sata_rows(c, h, ssd);
        else {
            // USB enclosures and RAID hide the drive's own data
            c->state = HS_NONE;
            row_add(c, false, TR("Состояние", "Status"), L"%s",
                    rw ? TR("диск не отдаёт данные о здоровье", "the drive doesn't report its health")
                       : TR("нужны права администратора", "needs administrator rights"));
        }
        CloseHandle(h);
        HL.disks++;
        if (c->state == HS_GOOD) HL.disks_ok++;
    }
}

// --- battery -------------------------------------------------------------------------------------------------------

static const GUID k_iface_battery = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };
#define MB_IOCTL_BATTERY_QUERY_TAG 0x294040
#define MB_IOCTL_BATTERY_QUERY_INFORMATION 0x294044

typedef struct MbBatteryQuery {
    ULONG tag;
    int level;          // 0 information, 4 device name, 6 manufacturer
    LONG at_rate;
} MbBatteryQuery;

typedef struct MbBatteryInfo {
    ULONG capabilities;
    UCHAR technology, reserved[3], chemistry[4];
    ULONG designed, full, alert1, alert2, critical_bias, cycles;
} MbBatteryInfo;

static void hl_battery(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(&k_iface_battery, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; i < 4 && SetupDiEnumDeviceInterfaces(set, NULL, &k_iface_battery, i, &ifd); i++) {
        u8 dbuf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)dbuf;
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof dbuf, NULL, NULL)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD wait = 0, got;
        MbBatteryQuery q = { 0 };
        MbBatteryInfo bi;
        memset(&bi, 0, sizeof bi);
        if (DeviceIoControl(h, MB_IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &q.tag, sizeof q.tag, &got, NULL) && q.tag &&
            DeviceIoControl(h, MB_IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &bi, sizeof bi, &got, NULL) && bi.designed && bi.full &&
            bi.designed != 0xFFFFFFFF) {
            HwCard *c = hl_card(0xE83F, TR("Батарея", "Battery"));
            if (c) {
                WCHAR name[64] = L"", maker[64] = L"";
                q.level = 4;
                DeviceIoControl(h, MB_IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, name, sizeof name - 2, &got, NULL);
                q.level = 6;
                DeviceIoControl(h, MB_IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, maker, sizeof maker - 2, &got, NULL);
                wfmt(c->head, countof(c->head), L"%s%s%s", maker, maker[0] && name[0] ? L" " : L"", name);
                if (!c->head[0]) wcopy(c->head, countof(c->head), TR("Батарея ноутбука", "Laptop battery"));
                int health = (int)MIN(100, (u64)bi.full * 100 / bi.designed);
                u8 st = health < 60 ? HS_BAD : health < 80 ? HS_WARN : HS_GOOD;
                WCHAR v[32];
                wfmt(v, countof(v), L"%d%%", health);
                hl_badge(c, st, v);
                wfmt(c->sub, countof(c->sub), TR("Ёмкость %d%% от заводской", "%d%% of the original capacity"), health);
                row_add(c, false, TR("Износ", "Wear"), L"%d%%", 100 - health);
                row_state(c, st);
                row_add(c, false, TR("Заводская ёмкость", "Design capacity"), TR("%.1f Вт·ч", "%.1f Wh"), bi.designed / 1000.0);
                row_add(c, false, TR("Полная ёмкость сейчас", "Full charge now"), TR("%.1f Вт·ч", "%.1f Wh"), bi.full / 1000.0);
                if (bi.cycles) row_add(c, false, TR("Циклов зарядки", "Charge cycles"), L"%lu", bi.cycles);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// --- stability -----------------------------------------------------------------------------------------------------

// How many System log events match the XPath filter in the last 30 days, and when the latest was.
static int ev_count(const WCHAR *filter, FILETIME *last)
{
    WCHAR q[512];
    wfmt(q, countof(q), L"*[System[%s and TimeCreated[timediff(@SystemTime) <= 2592000000]]]", filter);
    EVT_HANDLE h = EvtQuery(NULL, L"System", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!h) return 0;
    int n = 0;
    EVT_HANDLE ev[64];
    DWORD got = 0;
    if (last) memset(last, 0, sizeof *last);
    while (n < 9999 && EvtNext(h, countof(ev), ev, 0, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            if (n == 0 && last) {
                // the newest one comes first: its time
                EVT_HANDLE ctx = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
                u8 vb[2048];
                DWORD used = 0, cnt = 0;
                if (ctx && EvtRender(ctx, ev[i], EvtRenderEventValues, sizeof vb, vb, &used, &cnt)) {
                    EVT_VARIANT *v = (EVT_VARIANT *)vb;
                    if (v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) {
                        last->dwLowDateTime = (DWORD)v[EvtSystemTimeCreated].FileTimeVal;
                        last->dwHighDateTime = (DWORD)(v[EvtSystemTimeCreated].FileTimeVal >> 32);
                    }
                }
                if (ctx) EvtClose(ctx);
            }
            EvtClose(ev[i]);
            n++;
        }
    }
    EvtClose(h);
    return n;
}

static void ev_row(HwCard *c, const WCHAR *name, int n, const FILETIME *last, u8 bad_from, u8 st_when_some)
{
    if (!n) {
        row_add(c, false, name, L"%s", TR("нет", "none"));
        row_state(c, HS_GOOD);
        return;
    }
    SYSTEMTIME st, lt;
    FileTimeToSystemTime(last, &st);
    SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
    if (last->dwHighDateTime) row_add(c, false, name, TR("%d · последнее %02d.%02d", "%d · last %02d.%02d"), n, lt.wDay, lt.wMonth);
    else row_add(c, false, name, L"%d", n);
    u8 s = bad_from && n >= bad_from ? HS_BAD : st_when_some;
    row_state(c, s);
    if (s > c->state) c->state = s;
}

static void hl_stability(void)
{
    HwCard *c = hl_card(0xE9D9, TR("Стабильность за 30 дней", "Stability, last 30 days"));
    if (!c) return;
    c->state = HS_GOOD;
    FILETIME lb, lw, lwf, ls, ld;
    HL.bsod = ev_count(L"Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001", &lb);
    HL.whea = ev_count(L"Provider[@Name='Microsoft-Windows-WHEA-Logger']", &lw);
    int whea_fatal = ev_count(L"Provider[@Name='Microsoft-Windows-WHEA-Logger'] and (Level=1 or Level=2)", &lwf);
    int shut = ev_count(L"Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41", &ls);
    int disk = ev_count(L"Provider[@Name='disk'] and (EventID=7 or EventID=11 or EventID=51 or EventID=153)", &ld);
    ev_row(c, TR("Синие экраны", "Blue screens"), HL.bsod, &lb, 3, HS_WARN);
    ev_row(c, TR("Аппаратные ошибки WHEA", "WHEA hardware errors"), HL.whea, &lw, 0, whea_fatal ? HS_BAD : HS_WARN);
    ev_row(c, TR("Внезапные выключения", "Unexpected power-offs"), shut, &ls, 5, HS_WARN);
    ev_row(c, TR("Ошибки дисков", "Disk errors"), disk, &ld, 0, HS_BAD);
    if (c->state == HS_GOOD) {
        wcopy(c->head, countof(c->head), TR("Сбоев не было", "No crashes"));
        wcopy(c->sub, countof(c->sub), TR("Система работает стабильно", "The system runs stable"));
    } else {
        int total = HL.bsod + HL.whea + shut + disk;
        wfmt(c->head, countof(c->head), TR("Событий: %d", "Events: %d"), total);
        wcopy(c->sub, countof(c->sub),
              HL.whea ? TR("Ошибки WHEA - частый признак нестабильного разгона или сбоев памяти", "WHEA errors often mean an unstable overclock or memory faults")
                      : TR("Подробности - в журнале событий Windows, раздел «Система»", "Details are in the Windows Event Viewer, System log"));
    }
    hl_badge(c, c->state, NULL);
}

// --- devices -------------------------------------------------------------------------------------------------------

static const WCHAR *problem_text(ULONG p)
{
    switch (p) {
    case 1: return TR("неправильная настройка", "not configured correctly");
    case 3: return TR("драйвер повреждён", "driver corrupted");
    case 10: return TR("не запускается", "cannot start");
    case 12: return TR("не хватает ресурсов", "not enough resources");
    case 14: return TR("нужна перезагрузка", "needs a restart");
    case 18: return TR("переустановите драйвер", "reinstall the driver");
    case 19: return TR("ошибка реестра", "registry problem");
    case 21: return TR("удаляется", "being removed");
    case 24: return TR("не найдено или не работает", "not present or not working");
    case 28: return TR("драйвер не установлен", "no driver installed");
    case 31: return TR("драйвер не загружается", "driver doesn't load");
    case 32: return TR("служба драйвера отключена", "driver service disabled");
    case 37: return TR("драйвер не запустился", "driver failed to start");
    case 39: return TR("драйвер повреждён или отсутствует", "driver corrupted or missing");
    case 43: return TR("Windows остановила устройство", "Windows stopped the device");
    case 45: return TR("отключено от ПК", "not connected");
    case 48: return TR("драйвер заблокирован", "driver blocked");
    case 52: return TR("нет цифровой подписи драйвера", "driver isn't signed");
    }
    return L"";
}

static void hl_devices(void)
{
    HwCard *c = hl_card(0xE772, TR("Устройства", "Devices"));
    if (!c) return;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA d = { sizeof d };
    int n = 0;
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, d.DevInst, 0) != CR_SUCCESS || !(st & DN_HAS_PROBLEM)) continue;
        // switched off on purpose (by the user, the playbook or the firmware): not a fault
        if (prob == CM_PROB_DISABLED || prob == CM_PROB_HARDWARE_DISABLED || prob == CM_PROB_DISABLED_SERVICE) continue;
        WCHAR name[128];
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof name - 2, NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof name - 2, NULL))
            wcopy(name, countof(name), TR("Неизвестное устройство", "Unknown device"));
        const WCHAR *why = problem_text(prob);
        if (why[0]) row_add(c, true, name, L"%s", why);
        else row_add(c, true, name, TR("код %lu", "code %lu"), prob);
        row_state(c, HS_WARN);
        n++;
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    HL.devices = n;
    if (!n) {
        wcopy(c->head, countof(c->head), TR("Ошибок нет", "No problems"));
        wcopy(c->sub, countof(c->sub), TR("Все драйверы работают", "Every driver works"));
        hl_badge(c, HS_GOOD, NULL);
    } else {
        wfmt(c->head, countof(c->head), TR("С ошибками: %d", "With problems: %d"), n);
        wcopy(c->sub, countof(c->sub), TR("Как жёлтые значки в Диспетчере устройств", "Like the yellow marks in Device Manager"));
        hl_badge(c, HS_WARN, NULL);
    }
}

static void hl_stats(void)
{
    HwStat *s = HL.stat;
    s[0].glyph = 0xEDA2;
    wfmt(s[0].value, countof(s[0].value), L"%d/%d", HL.disks_ok, HL.disks);
    wcopy(s[0].label, countof(s[0].label), TR("дисков в порядке", "drives healthy"));
    s[1].glyph = 0xEA39;
    wfmt(s[1].value, countof(s[1].value), L"%d", HL.bsod);
    wcopy(s[1].label, countof(s[1].label), TR("синих экранов за 30 дней", "blue screens, 30 days"));
    s[2].glyph = 0xE9D9;
    wfmt(s[2].value, countof(s[2].value), L"%d", HL.whea);
    wcopy(s[2].label, countof(s[2].label), TR("аппаратных ошибок WHEA", "WHEA hardware errors"));
    s[3].glyph = 0xE772;
    wfmt(s[3].value, countof(s[3].value), L"%d", HL.devices);
    wcopy(s[3].label, countof(s[3].label), TR("устройств с ошибками", "devices with problems"));
}

static DWORD WINAPI hl_thread(void *arg)
{
    HWND notify = (HWND)arg;
again:
    memset(HL.card, 0, sizeof HL.card);
    memset(HL.stat, 0, sizeof HL.stat);
    HL.ncards = 0;
    HL.state = HS_GOOD;
    HL.disks = HL.disks_ok = HL.bsod = HL.whea = HL.devices = 0;
    k_gb = TR("ГБ", "GB");
    k_tb = TR("ТБ", "TB");
    hl_disks();
    hl_battery();
    hl_stability();
    hl_devices();
    hl_stats();
    if (InterlockedExchange(&HL.again, 0)) goto again;
    InterlockedExchange(&HL.ready, 1);
    InterlockedExchange(&HL.busy, 0);
    PostMessageW(notify, WM_APP_HW, 0, 0);
    return 0;
}

static void health_start(HWND notify)
{
    InterlockedExchange(&HL.ready, 0);
    if (InterlockedExchange(&HL.busy, 1)) {
        InterlockedExchange(&HL.again, 1);
        return;
    }
    HANDLE t = CreateThread(NULL, 0, hl_thread, notify, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&HL.busy, 0);
}
