// "PC health": how the parts are doing, read on a worker thread.
//   drives     NVMe: the drive's own health log; SATA SSD / HDD: SMART attributes and thresholds
//   battery    worn capacity and charge cycles (laptops only)
//   memory     the speed against the modules' rating (XMP / EXPO), the channels, the Windows memory test
//   drivers    how old the GPU drivers are, the chipset's own driver or only Windows'
//   stability  the last 30 days of the System event log: blue screens, WHEA hardware errors, unexpected
//              power-offs, disk errors
//   devices    devices whose driver is in trouble (not the ones switched off on purpose, which the playbook does)
// No sensors that need a kernel driver (CPU temperature, fans): reading those safely isn't possible without one.

#include <winevt.h>
#ifdef _MSC_VER
#pragma comment(lib, "wevtapi")
#endif

#define HL_CARDS 14

typedef struct HlData {
    HwCard card[HL_CARDS];
    int ncards;
    HwStat stat[4];
    u8 state;                       // the worst of all: HS_GOOD, HS_WARN or HS_BAD
    int disks, disks_ok, bsod, whea, devices, batteries;
    int disk_err_gone;   // disk errors from drives no longer in the PC
} HlData;

// Built in HLT off-screen and swapped into HL.d whole, like the parts page.
static struct {
    volatile LONG ready, busy, again;
    SRWLOCK lock;
    HlData d;
} HL = { 0, 0, 0, SRWLOCK_INIT };
static HlData HLT;

static HwCard *hl_card(u32 glyph, const WCHAR *cat)
{
    if (HLT.ncards >= HL_CARDS) return NULL;
    HwCard *c = &HLT.card[HLT.ncards++];
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
    if (state > HLT.state) HLT.state = state;
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

#define DISK_EVENTS L"Provider[@Name='disk'] and (EventID=7 or EventID=11 or EventID=51 or EventID=153)"

// Disk errors in the System log over 30 days, per drive. The events name "\Device\Harddisk0\DR0", but that number is
// handed out at boot and moves when a drive is added or taken out: the errors of a drive pulled yesterday would land on
// the one that got its number today. So each error is pinned to a serial number: Windows logs every drive it brings
// up (Microsoft-Windows-Partition/Diagnostic, event 1006, with the number and the serial), and an error belongs to the
// last drive that came up under its number before the error. Errors with no such record keep the number.

typedef struct DiskArrival {
    u64 t;
    int num;
    WCHAR serial[64];
} DiskArrival;

static struct {
    WCHAR serial[32][64];
    int by_serial[32], nserial;
    int by_num[32];        // errors with no drive on record for them
    int total;
} DE;

// Letters and digits only, upper case: the log and the drive itself pad and punctuate the serial differently.
static void serial_norm(WCHAR *out, int cap, const WCHAR *s)
{
    int n = 0;
    for (; s && *s && n < cap - 1; s++)
        if (iswalnum(*s)) out[n++] = towupper(*s);
    out[n] = 0;
}

static u64 ev_variant_u64(const EVT_VARIANT *v)
{
    switch (v->Type) {
    case EvtVarTypeFileTime: return v->FileTimeVal;
    case EvtVarTypeUInt32: return v->UInt32Val;
    case EvtVarTypeUInt16: return v->UInt16Val;
    case EvtVarTypeUInt64: return v->UInt64Val;
    case EvtVarTypeInt32: return (u64)v->Int32Val;
    case EvtVarTypeString: return v->StringVal ? (u64)wcstoull(v->StringVal, NULL, 10) : 0;
    }
    return 0;
}

static int disk_arrivals(DiskArrival *out, int cap)
{
    LPCWSTR paths[3] = { L"Event/System/TimeCreated/@SystemTime", L"Event/EventData/Data[@Name='DiskNumber']",
                         L"Event/EventData/Data[@Name='SerialNumber']" };
    EVT_HANDLE ctx = EvtCreateRenderContext(3, paths, EvtRenderContextValues);
    if (!ctx) return 0;
    // 40 days: the boot before the 30-day window named the drives of its first errors
    EVT_HANDLE h = EvtQuery(NULL, L"Microsoft-Windows-Partition/Diagnostic", L"*[System[EventID=1006 and TimeCreated[timediff(@SystemTime) <= 3456000000]]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    int n = 0;
    if (h) {
        EVT_HANDLE ev[64];
        DWORD got = 0;
        static EVT_VARIANT vals[512];
        while (n < cap && EvtNext(h, countof(ev), ev, 0, 0, &got)) {
            for (DWORD i = 0; i < got; i++) {
                DWORD used = 0, cnt = 0;
                if (n < cap && EvtRender(ctx, ev[i], EvtRenderEventValues, sizeof vals, vals, &used, &cnt) && cnt >= 3 &&
                    vals[2].Type == EvtVarTypeString && vals[2].StringVal) {
                    out[n].t = ev_variant_u64(&vals[0]);
                    out[n].num = (int)ev_variant_u64(&vals[1]);
                    serial_norm(out[n].serial, countof(out[n].serial), vals[2].StringVal);
                    if (out[n].serial[0]) n++;
                }
                EvtClose(ev[i]);
            }
        }
        EvtClose(h);
    }
    EvtClose(ctx);
    return n;
}

static void disk_event_counts(void)
{
    memset(&DE, 0, sizeof DE);
    static DiskArrival arr[4096];
    int narr = disk_arrivals(arr, countof(arr));
    LPCWSTR tpath[1] = { L"Event/System/TimeCreated/@SystemTime" };
    EVT_HANDLE tctx = EvtCreateRenderContext(1, tpath, EvtRenderContextValues);
    WCHAR q[512];
    wfmt(q, countof(q), L"*[System[%s and TimeCreated[timediff(@SystemTime) <= 2592000000]]]", DISK_EVENTS);
    EVT_HANDLE h = EvtQuery(NULL, L"System", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!h) {
        if (tctx) EvtClose(tctx);
        return;
    }
    EVT_HANDLE ev[64];
    DWORD got = 0;
    static WCHAR xml[16384];
    static EVT_VARIANT tv[8];
    while (DE.total < 9999 && EvtNext(h, countof(ev), ev, 0, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, cnt = 0;
            if (EvtRender(NULL, ev[i], EvtRenderEventXml, sizeof xml - sizeof(WCHAR), xml, &used, &cnt)) {
                xml[MIN(used / sizeof(WCHAR), countof(xml) - 1)] = 0;
                const WCHAR *p = wcsstr(xml, L"Harddisk");
                long num = p ? wcstol(p + 8, NULL, 10) : -1;
                u64 t = 0;
                if (tctx && EvtRender(tctx, ev[i], EvtRenderEventValues, sizeof tv, tv, &used, &cnt) && cnt >= 1) t = ev_variant_u64(&tv[0]);
                // the last drive that came up under this number before the error
                int best = -1;
                for (int k = 0; k < narr; k++)
                    if (arr[k].num == num && arr[k].t <= t && (best < 0 || arr[k].t > arr[best].t)) best = k;
                if (best >= 0) {
                    int s = 0;
                    while (s < DE.nserial && wcscmp(DE.serial[s], arr[best].serial)) s++;
                    if (s == DE.nserial && s < countof(DE.serial)) wcopy(DE.serial[DE.nserial++], countof(DE.serial[0]), arr[best].serial);
                    if (s < DE.nserial) DE.by_serial[s]++;
                } else if (num >= 0 && num < 32) {
                    DE.by_num[num]++;
                }
                DE.total++;
            }
            EvtClose(ev[i]);
        }
    }
    EvtClose(h);
    if (tctx) EvtClose(tctx);
}

// The errors of the drive that is PhysicalDrive num now, with this serial.
static int disk_errors_for(int num, const WCHAR *serial)
{
    WCHAR sn[64];
    serial_norm(sn, countof(sn), serial);
    int n = num >= 0 && num < 32 ? DE.by_num[num] : 0;
    for (int s = 0; sn[0] && s < DE.nserial; s++)
        if (!wcscmp(DE.serial[s], sn)) n += DE.by_serial[s];
    return n;
}

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

// Free space on the system volume, if it is one of this drive's ("C:, D:"): Windows needs room there for updates,
// the page file and shader caches. 80 GB is enough on any drive (a percentage asks too much of a big one); on a
// smaller volume a quarter of it.
static void hl_free_space(HwCard *c, const WCHAR *letters)
{
    WCHAR win[MAX_PATH];
    if (!GetWindowsDirectoryW(win, MAX_PATH) || win[1] != L':') return;
    for (const WCHAR *p = letters; *p; p++) {
        if (p[1] != L':' || towupper(p[0]) != towupper(win[0])) continue;
        WCHAR root[4] = { p[0], L':', L'\\', 0 };
        ULARGE_INTEGER avail, total;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, NULL) || !total.QuadPart) continue;
        u64 want = MIN(80ull << 30, (u64)total.QuadPart / 4);
        WCHAR k[48], fs[32], ts[32];
        fmt_size(fs, countof(fs), avail.QuadPart);
        fmt_size(ts, countof(ts), total.QuadPart);
        wfmt(k, countof(k), TR("Свободно на %c:", "Free on %c:"), p[0]);
        bool low = avail.QuadPart < want;
        row_add(c, false, k, low ? TR("%s из %s - мало, игры и обновления не поместятся", "%s of %s - low, games and updates won't fit") : TR("%s из %s", "%s of %s"), fs, ts);
        row_state(c, low ? HS_WARN : HS_GOOD);
        if (low && c->state < HS_WARN) {
            WCHAR keep[48];
            const WCHAR *dot = wcsstr(c->badge, L" · ");
            wcopy(keep, countof(keep), dot ? dot + 3 : L"");
            hl_badge(c, HS_WARN, keep);
        }
    }
}

static void hl_disks(void)
{
    WCHAR letters[32][24];
    disk_letters(letters);
    disk_event_counts();
    int shown = 0;
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
        WCHAR model[96] = L"", serial[96] = L"";
        int bus = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *dd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            char tmp[128] = "";
            if (dd->VendorIdOffset && dd->VendorIdOffset < got) snprintf(tmp, sizeof tmp, "%s ", (const char *)buf + dd->VendorIdOffset);
            if (dd->ProductIdOffset && dd->ProductIdOffset < got) strncat(tmp, (const char *)buf + dd->ProductIdOffset, sizeof tmp - strlen(tmp) - 1);
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, model, countof(model));
            trim(model);
            if (dd->SerialNumberOffset && dd->SerialNumberOffset < got)
                MultiByteToWideChar(CP_ACP, 0, (const char *)buf + dd->SerialNumberOffset, -1, serial, countof(serial));
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
        hl_free_space(c, letters[i]);
        // Windows logged read errors (bad blocks, retries) on this disk: worth a backup and a closer look
        int errs = disk_errors_for(i, serial);
        shown += errs;
        if (errs) {
            row_add(c, false, TR("Ошибки в журнале Windows", "Errors in the Windows log"), TR("%d за 30 дней", "%d in 30 days"), errs);
            row_state(c, HS_BAD);
            if (c->state < HS_WARN) {
                WCHAR keep[48];
                const WCHAR *dot = wcsstr(c->badge, L" · ");
                wcopy(keep, countof(keep), dot ? dot + 3 : L"");
                hl_badge(c, HS_WARN, keep);
            }
        }
        HLT.disks++;
        if (c->state == HS_GOOD) HLT.disks_ok++;
    }
    // what is left belongs to drives taken out since
    HLT.disk_err_gone = MAX(0, DE.total - shown);
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
            HLT.batteries++;
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

// --- memory --------------------------------------------------------------------------------------------------------
// The speed the modules run at against the speed they are sold for, the channels, and the Windows memory test.
// Firmware reports no XMP or EXPO state, and reading the modules' SPD takes a driver; but the part number carries
// the rated speed for the gaming brands (F4-3600C16, CMK16GX4M2B3200C16, KF436C17, BL8G36C16U4B, ...), and a kit
// running far below it has its profile off.

// The rated MT/s from a part number, 0 when it doesn't say.
static int part_rated(const WCHAR *part)
{
    WCHAR s[64];
    int n = 0;
    for (const WCHAR *p = part; *p && n < countof(s) - 1; p++) s[n++] = towupper(*p);
    s[n] = 0;
    // Kingston FURY and HyperX: the DDR generation, then the hundreds (KF436C17 is DDR4-3600, KF560C40 DDR5-6000);
    // ValueRAM: KVR32N22
    if ((wstarts_with_i(s, L"KF") || wstarts_with_i(s, L"HX")) && (s[2] == '3' || s[2] == '4' || s[2] == '5') && iswdigit(s[3]) && iswdigit(s[4]) &&
        !iswdigit(s[5]))
        return ((s[3] - '0') * 10 + (s[4] - '0')) * 100;
    if (wstarts_with_i(s, L"KVR") && iswdigit(s[3]) && iswdigit(s[4]) && !iswdigit(s[5])) return _wtoi(s + 3) * 100;
    // four digits that look like a speed, between letters or dashes: F4-3600C16, CMK16GX4M2B3200C16, TF3D416G3600HC18, AX4U32008G16
    for (int i = 1; i + 4 <= n; i++) {
        if (iswdigit(s[i - 1]) || !iswdigit(s[i]) || !iswdigit(s[i + 1]) || !iswdigit(s[i + 2]) || !iswdigit(s[i + 3])) continue;
        int v = (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
        if ((s[i - 1] == '-' || iswalpha(s[i - 1])) && v >= 1600 && v <= 9600 && v % 100 <= 66) return v;
    }
    // two digits after the size, the hundreds: BL8G36C16U4B, CP16G60C36U5B, CT16G48C40U5
    for (int i = 1; i + 3 < n; i++)
        if (s[i - 1] == 'G' && iswdigit(s[i - 2]) && iswdigit(s[i]) && iswdigit(s[i + 1]) && s[i + 2] == 'C') {
            int v = ((s[i] - '0') * 10 + (s[i + 1] - '0')) * 100;
            if (v >= 1600 && v <= 9600) return v;
        }
    // three digits after the size, the tens: PVS416G320C6K
    for (int i = 1; i + 4 < n; i++)
        if (s[i - 1] == 'G' && iswdigit(s[i - 2]) && iswdigit(s[i]) && iswdigit(s[i + 1]) && iswdigit(s[i + 2]) && s[i + 3] == 'C') {
            int v = ((s[i] - '0') * 100 + (s[i + 1] - '0') * 10 + (s[i + 2] - '0')) * 10;
            if (v >= 1600 && v <= 9600) return v;
        }
    return 0;
}

static bool lpddr(int type) { return type == 0x1B || type == 0x1C || type == 0x1D || type == 0x1E || type == 0x23; }

// The latest result of the Windows Memory Diagnostic (mdsched): 1 no errors, 2 errors found, 0 never run.
static int mem_diag(FILETIME *when)
{
    memset(when, 0, sizeof *when);
    EVT_HANDLE h = EvtQuery(NULL, L"System", L"*[System[Provider[@Name='Microsoft-Windows-MemoryDiagnostics-Results']]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!h) return 0;
    int res = 0;
    EVT_HANDLE ev;
    DWORD got = 0;
    while (!res && EvtNext(h, 1, &ev, 0, 0, &got) && got) {
        EVT_HANDLE ctx = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
        static u8 vb[4096];
        DWORD used = 0, cnt = 0;
        if (ctx && EvtRender(ctx, ev, EvtRenderEventValues, sizeof vb, vb, &used, &cnt)) {
            EVT_VARIANT *v = (EVT_VARIANT *)vb;
            int id = v[EvtSystemEventID].Type == EvtVarTypeUInt16 ? v[EvtSystemEventID].UInt16Val : 0;
            // 1101 / 1201: no errors; 1102 / 1202: hardware errors found. Other events (scheduled, cancelled) are skipped.
            if (id == 1101 || id == 1201) res = 1;
            else if (id == 1102 || id == 1202) res = 2;
            if (res && v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) {
                when->dwLowDateTime = (DWORD)v[EvtSystemTimeCreated].FileTimeVal;
                when->dwHighDateTime = (DWORD)(v[EvtSystemTimeCreated].FileTimeVal >> 32);
            }
        }
        if (ctx) EvtClose(ctx);
        EvtClose(ev);
    }
    EvtClose(h);
    return res;
}

static void fmt_date_ft(WCHAR *out, int cap, const FILETIME *ft)
{
    SYSTEMTIME st, lt;
    FileTimeToSystemTime(ft, &st);
    SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
    wfmt(out, cap, L"%02d.%02d.%d", lt.wDay, lt.wMonth, lt.wYear);
}

static void hl_memory(bool laptop)
{
    static SmbData sm;
    smbios_read(&sm);
    if (!sm.nmem) return;
    HwCard *c = hl_card(0xE964, TR("Оперативная память", "Memory"));
    if (!c) return;
    u64 total = 0;
    for (int i = 0; i < sm.nmem; i++) total += sm.mem[i].bytes;
    const SmbMem *m0 = &sm.mem[0];
    int conf = m0->conf ? m0->conf : m0->speed, rated = 0, max_spd = 0;
    bool mixed = false, dimm = m0->form == 0x09 && !laptop && !lpddr(m0->type);
    for (int i = 0; i < sm.nmem; i++) {
        int r = part_rated(sm.mem[i].part);
        if (r && (!rated || r < rated)) rated = r;  // a mixed kit runs at its slowest
        if (sm.mem[i].speed && (!max_spd || sm.mem[i].speed < max_spd)) max_spd = sm.mem[i].speed;
        mixed |= sm.mem[i].bytes != m0->bytes || _wcsicmp(sm.mem[i].part, m0->part) != 0;
    }
    WCHAR sz[32];
    fmt_size(sz, countof(sz), total);
    const WCHAR *type = mem_type(m0->type);
    wfmt(c->head, countof(c->head), L"%s%s%s", sz, type[0] ? L" " : L"", type);
    if (conf) wfmt(c->sub, countof(c->sub), TR("%d МТ/с · модулей: %d", "%d MT/s · %d module(s)"), conf, sm.nmem);
    u8 st = HS_GOOD;

    // the speed against the rating: the part number's, else what the module itself reports as its maximum
    int want = rated > conf ? rated : max_spd > conf ? max_spd : 0;
    if (conf && want > conf + 100) {
        row_add(c, false, TR("Частота", "Speed"), TR("%d из %d МТ/с", "%d of %d MT/s"), conf, want);
        row_state(c, dimm ? HS_WARN : HS_NONE);
        row_add(c, false, L"XMP / EXPO", L"%s", dimm ? TR("выключен - включите в BIOS", "off - turn it on in the BIOS") : TR("память может работать быстрее", "the memory could run faster"));
        row_state(c, dimm ? HS_WARN : HS_NONE);
        if (dimm) st = HS_WARN;
    } else if (conf) {
        row_add(c, false, TR("Частота", "Speed"), TR("%d МТ/с", "%d MT/s"), conf);
        if (rated) {
            row_add(c, false, L"XMP / EXPO", L"%s", rated > 3200 && m0->type == 0x1A ? TR("включён", "on") : rated > 5600 && m0->type == 0x22 ? TR("включён", "on") : TR("на заявленной частоте", "at the rated speed"));
            row_state(c, HS_GOOD);
        } else if (dimm) {
            row_add(c, false, L"XMP / EXPO", L"%s", TR("не определить по модели", "can't tell from the model"));
        }
    }

    // channels: one stick in a desktop with two or more slots halves the bandwidth
    if (sm.nmem == 1 && sm.slots >= 2 && !lpddr(m0->type)) {
        row_add(c, false, TR("Каналы", "Channels"), L"%s", TR("один - добавьте второй модуль", "single - add a second module"));
        row_state(c, HS_WARN);
        st = HS_WARN;
    } else if (sm.nmem >= 2 && !lpddr(m0->type)) {
        row_add(c, false, TR("Каналы", "Channels"), TR("%d модуля из %d слотов", "%d modules in %d slots"), sm.nmem, MAX(sm.slots, sm.nmem));
        row_state(c, sm.nmem % 2 ? HS_NONE : HS_GOOD);
    }
    if (mixed) {
        row_add(c, false, TR("Модули", "Modules"), L"%s", TR("разные: возможны сбои и сброс частоты", "different: may be unstable or slowed down"));
        row_state(c, HS_WARN);
        st = MAX(st, HS_WARN);
    }

    FILETIME when;
    WCHAR d[32];
    int diag = mem_diag(&when);
    if (diag) fmt_date_ft(d, countof(d), &when);
    if (diag == 2) {
        row_add(c, false, TR("Проверка памяти Windows", "Windows memory test"), TR("найдены ошибки · %s", "errors found · %s"), d);
        row_state(c, HS_BAD);
        st = HS_BAD;
    } else if (diag == 1) {
        row_add(c, false, TR("Проверка памяти Windows", "Windows memory test"), TR("без ошибок · %s", "no errors · %s"), d);
        row_state(c, HS_GOOD);
    } else {
        row_add(c, false, TR("Проверка памяти Windows", "Windows memory test"), L"%s", TR("не запускалась (mdsched)", "never run (mdsched)"));
    }
    hl_badge(c, st, NULL);
}

// --- drivers -------------------------------------------------------------------------------------------------------
// How old the GPU drivers are, and whether the chipset has its maker's driver or only Windows' own.

static const DEVPROPKEY k_drv_date = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 2 };
static const DEVPROPKEY k_drv_ver = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 3 };
static const DEVPROPKEY k_drv_provider = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 9 };

static u64 ft_u64(const FILETIME *f) { return ((u64)f->dwHighDateTime << 32) | f->dwLowDateTime; }

// Days from a FILETIME to now; -1 for no date or a placeholder (Intel dates its chipset INFs 1968).
static int days_since(const FILETIME *f)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    u64 t = ft_u64(f), n = ft_u64(&now);
    if (!t || t > n || t < 125911584000000000ull /* 2000-01-01 */) return -1;
    return (int)((n - t) / (864000000000ull));
}

static void fmt_age(WCHAR *out, int cap, int days)
{
    if (days < 0) out[0] = 0;
    else if (days < 31) wcopy(out, cap, TR("свежий", "recent"));
    else if (days < 365) wfmt(out, cap, TR("%d мес.", "%d mo"), days / 30);
    else wfmt(out, cap, TR("%.1f г.", "%.1f y"), (f64)days / 365.0);
}

static void hl_drivers(void)
{
    HwCard *c = hl_card(0xE895, TR("Драйверы", "Drivers"));
    if (!c) return;
    u8 st = HS_GOOD;
    int ngpu = 0, oldest = -1;
    HDEVINFO set = SetupDiGetClassDevsW(&k_class_display, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA d = { sizeof d };
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        GpuInfo g;
        memset(&g, 0, sizeof g);
        if (!SetupDiGetDeviceInstanceIdW(set, &d, g.id, countof(g.id), NULL) || !wstarts_with_i(g.id, L"PCI\\")) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)g.name, sizeof g.name - 2, NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)g.name, sizeof g.name - 2, NULL))
            continue;
        DEVPROPTYPE type;
        SetupDiGetDevicePropertyW(set, &d, &k_drv_ver, &type, (BYTE *)g.ver, sizeof g.ver - 2, NULL, 0);
        HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            DWORD sz = sizeof g.amd;
            RegGetValueW(k, NULL, L"RadeonSoftwareVersion", RRF_RT_REG_SZ, NULL, g.amd, &sz);
            RegCloseKey(k);
        }
        FILETIME ft = { 0 };
        SetupDiGetDevicePropertyW(set, &d, &k_drv_date, &type, (BYTE *)&ft, sizeof ft, NULL, 0);
        int days = days_since(&ft);
        bool integ = gpu_is_integrated(g.id);
        // GPU makers ship a driver a month or two; an integrated one gets fewer, often through the laptop's maker
        int limit = integ ? 365 : 180;
        WCHAR ver[128], age[32], date[32] = L"";
        gpu_driver_text(&g, ver, countof(ver));
        WCHAR *paren = wcsstr(ver, L"  (");
        if (paren) *paren = 0;
        fmt_age(age, countof(age), days);
        if (days >= 0) fmt_date_ft(date, countof(date), &ft);
        row_add(c, true, g.name, L"%s%s%s%s%s", ver, date[0] ? L" · " : L"", date, age[0] ? L" · " : L"", age);
        row_state(c, days > limit ? HS_WARN : days >= 0 ? HS_GOOD : HS_NONE);
        if (days > limit) st = HS_WARN;
        if (days > oldest) oldest = days;
        ngpu++;
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);

    // the chipset: the platform's own System devices (SMBus, PSP, the Management Engine) and who their driver is from
    CpuInfo ci;
    cpu_info(&ci);
    bool amd = wcsstr(ci.name, L"AMD") != NULL, intel = wcsstr(ci.name, L"Intel") != NULL;
    if (amd || intel) {
        const WCHAR *ven = amd ? L"VEN_1022" : L"VEN_8086";
        int vendor_drv = 0, total = 0, best_days = -1;
        WCHAR best_ver[64] = L"";
        FILETIME best_ft = { 0 };
        set = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &d); i++) {
            WCHAR id[200], cls[64] = L"", prov[96] = L"", ver[64] = L"";
            if (!SetupDiGetDeviceInstanceIdW(set, &d, id, countof(id), NULL) || !wcsstr(id, ven)) continue;
            SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_CLASS, NULL, (BYTE *)cls, sizeof cls - 2, NULL);
            if (_wcsicmp(cls, L"System") && _wcsicmp(cls, L"SecurityDevices")) continue;
            DEVPROPTYPE type;
            SetupDiGetDevicePropertyW(set, &d, &k_drv_provider, &type, (BYTE *)prov, sizeof prov - 2, NULL, 0);
            total++;
            if (!prov[0] || wstarts_with_i(prov, L"Microsoft") || wstarts_with_i(prov, L"(Standard")) continue;
            vendor_drv++;
            FILETIME ft = { 0 };
            SetupDiGetDevicePropertyW(set, &d, &k_drv_date, &type, (BYTE *)&ft, sizeof ft, NULL, 0);
            SetupDiGetDevicePropertyW(set, &d, &k_drv_ver, &type, (BYTE *)ver, sizeof ver - 2, NULL, 0);
            int days = days_since(&ft);
            if (best_ver[0] == 0 || (days >= 0 && (best_days < 0 || days < best_days))) {
                best_days = days;
                best_ft = ft;
                wcopy(best_ver, countof(best_ver), ver);
            }
        }
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        const WCHAR *name = amd ? TR("Чипсет AMD", "AMD chipset") : TR("Чипсет Intel", "Intel chipset");
        if (vendor_drv) {
            WCHAR date[32] = L"";
            if (best_days >= 0) fmt_date_ft(date, countof(date), &best_ft);
            row_add(c, true, name, L"%s%s%s", best_ver[0] ? best_ver : TR("установлен", "installed"), date[0] ? L" · " : L"", date);
            row_state(c, HS_GOOD);
        } else if (total) {
            row_add(c, true, name, L"%s", TR("не установлен: стоят драйверы Windows", "not installed: Windows' own drivers"));
            row_state(c, HS_WARN);
            st = HS_WARN;
        }
    }

    if (!ngpu && !c->nrows) {
        wcopy(c->head, countof(c->head), TR("Нет данных", "No data"));
    } else if (st == HS_GOOD) {
        wcopy(c->head, countof(c->head), TR("Драйверы в порядке", "Drivers are fine"));
        WCHAR age[32];
        fmt_age(age, countof(age), oldest);
        if (oldest >= 0) wfmt(c->sub, countof(c->sub), TR("Драйвер видеокарты: %s", "GPU driver: %s"), age);
    } else {
        wcopy(c->head, countof(c->head), TR("Пора обновить", "Time to update"));
        wcopy(c->sub, countof(c->sub), TR("Скачайте свежие с сайта NVIDIA, AMD или Intel", "Get fresh ones from NVIDIA, AMD or Intel"));
    }
    hl_badge(c, st, NULL);
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
    HLT.bsod = ev_count(L"Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001", &lb);
    HLT.whea = ev_count(L"Provider[@Name='Microsoft-Windows-WHEA-Logger']", &lw);
    int whea_fatal = ev_count(L"Provider[@Name='Microsoft-Windows-WHEA-Logger'] and (Level=1 or Level=2)", &lwf);
    int shut = ev_count(L"Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41", &ls);
    // only the drives still in the PC: a pulled drive's errors are no news about this one
    int disk = MAX(0, ev_count(DISK_EVENTS, &ld) - HLT.disk_err_gone);
    ev_row(c, TR("Синие экраны", "Blue screens"), HLT.bsod, &lb, 3, HS_WARN);
    ev_row(c, TR("Аппаратные ошибки WHEA", "WHEA hardware errors"), HLT.whea, &lw, 0, whea_fatal ? HS_BAD : HS_WARN);
    ev_row(c, TR("Внезапные выключения", "Unexpected power-offs"), shut, &ls, 5, HS_WARN);
    ev_row(c, TR("Ошибки дисков", "Disk errors"), disk, &ld, 0, HS_WARN);
    if (c->state == HS_GOOD) {
        wcopy(c->head, countof(c->head), TR("Сбоев не было", "No crashes"));
        wcopy(c->sub, countof(c->sub), TR("Система работает стабильно", "The system runs stable"));
    } else {
        int total = HLT.bsod + HLT.whea + shut + disk;
        wfmt(c->head, countof(c->head), TR("Событий: %d", "Events: %d"), total);
        wcopy(c->sub, countof(c->sub),
              HLT.whea ? TR("WHEA - часто нестабильный разгон или память", "WHEA: often an unstable overclock or memory")
              : disk  ? TR("Какой диск - видно в его карточке", "The drive's card shows which one")
                      : TR("Подробности - в журнале «Система»", "Details: Event Viewer, System log"));
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
    HLT.devices = n;
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
    HwStat *s = HLT.stat;
    s[0].glyph = 0xEDA2;
    wfmt(s[0].value, countof(s[0].value), L"%d/%d", HLT.disks_ok, HLT.disks);
    wcopy(s[0].label, countof(s[0].label), TR("дисков в норме", "drives healthy"));
    s[1].glyph = 0xEA39;
    wfmt(s[1].value, countof(s[1].value), L"%d", HLT.bsod);
    wcopy(s[1].label, countof(s[1].label), TR("синих экранов", "blue screens"));
    s[2].glyph = 0xE9D9;
    wfmt(s[2].value, countof(s[2].value), L"%d", HLT.whea);
    wcopy(s[2].label, countof(s[2].label), TR("ошибок WHEA", "WHEA errors"));
    s[3].glyph = 0xE772;
    wfmt(s[3].value, countof(s[3].value), L"%d", HLT.devices);
    wcopy(s[3].label, countof(s[3].label), TR("сбоев устройств", "device problems"));
}

static DWORD WINAPI hl_thread(void *arg)
{
    HWND notify = (HWND)arg;
again:
    memset(&HLT, 0, sizeof HLT);
    HLT.state = HS_GOOD;
    k_gb = TR("ГБ", "GB");
    k_tb = TR("ТБ", "TB");
    hl_disks();
    hl_battery();
    hl_memory(HLT.batteries > 0);
    hl_drivers();
    hl_stability();
    hl_devices();
    hl_stats();
    if (InterlockedExchange(&HL.again, 0)) goto again;
    AcquireSRWLockExclusive(&HL.lock);
    HL.d = HLT;
    ReleaseSRWLockExclusive(&HL.lock);
    InterlockedExchange(&HL.ready, 1);
    InterlockedExchange(&HL.busy, 0);
    PostMessageW(notify, WM_APP_HEALTH, 0, 0);
    return 0;
}

static void health_start(HWND notify)
{
    if (InterlockedExchange(&HL.busy, 1)) {
        InterlockedExchange(&HL.again, 1);
        return;
    }
    HANDLE t = CreateThread(NULL, 0, hl_thread, notify, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&HL.busy, 0);
}
