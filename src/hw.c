// What the PC is made of, for the "PC components" page. Read straight from Windows on a worker thread: SMBIOS (memory
// modules, the CPU socket), the registry (board, BIOS, Windows), SetupAPI (GPU driver and PCIe link), storage IOCTLs,
// the display configuration and the network adapters. No WMI.

#include <winioctl.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi")
#endif

#define HW_ROWS 14

typedef struct HwRow {
    WCHAR k[96];
    WCHAR v[160];
    bool item;              // a list entry (a module, a drive): the name is the bright part
} HwRow;

typedef struct HwCard {
    u32 glyph;
    WCHAR cat[48];
    WCHAR head[160];
    WCHAR sub[160];
    HwRow rows[HW_ROWS];
    int nrows;
} HwCard;

enum { HWC_CPU, HWC_GPU, HWC_RAM, HWC_BOARD, HWC_DISK, HWC_DISPLAY, HWC_NET, HWC_OS, HWC__COUNT };

typedef struct HwStat {
    u32 glyph;
    WCHAR value[24], unit[24], label[48];
} HwStat;

static struct {
    volatile LONG ready, busy, again;  // again: the language changed while reading, read once more
    HwCard card[HWC__COUNT];
    HwStat stat[4];
} HW;

static const WCHAR *k_gb, *k_tb;

static void row_add(HwCard *c, bool item, const WCHAR *k, const WCHAR *fmt, ...)
{
    if (c->nrows >= HW_ROWS) return;
    HwRow *r = &c->rows[c->nrows];
    wcopy(r->k, countof(r->k), k);
    va_list ap;
    va_start(ap, fmt);
#ifdef _MSC_VER
    _vsnwprintf_s(r->v, countof(r->v), _TRUNCATE, fmt, ap);
#else
    _vsnwprintf(r->v, countof(r->v), fmt, ap);
    r->v[countof(r->v) - 1] = 0;
#endif
    va_end(ap);
    r->item = item;
    if (r->k[0] || r->v[0]) c->nrows++;
}

// Binary units, as Windows shows them: "476 ГБ", "1.82 ТБ".
static void fmt_size(WCHAR *out, int cap, u64 bytes)
{
    f64 gb = (f64)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1000.0) wfmt(out, cap, L"%.2f %s", gb / 1024.0, k_tb);
    else if (gb >= 10.0 || gb == floor(gb)) wfmt(out, cap, L"%.0f %s", gb, k_gb);
    else wfmt(out, cap, L"%.1f %s", gb, k_gb);
}

static void trim(WCHAR *s)
{
    int a = 0, b = wlen(s);
    while (s[a] == ' ') a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    memmove(s, s + a, (size_t)(b - a) * sizeof(WCHAR));
    s[b - a] = 0;
}

static bool reg_str(HKEY root, const WCHAR *key, const WCHAR *name, WCHAR *out, int cap)
{
    DWORD sz = (DWORD)(cap * (int)sizeof(WCHAR));
    out[0] = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, NULL, out, &sz) != ERROR_SUCCESS) return false;
    trim(out);
    return out[0] != 0;
}

static DWORD reg_dw(HKEY root, const WCHAR *key, const WCHAR *name, DWORD def)
{
    DWORD v = def, sz = sizeof v;
    RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v;
}

// Firmware placeholders that say nothing.
static bool junk(const WCHAR *s)
{
    static const WCHAR *bad[] = { L"To Be Filled By O.E.M.", L"Default string", L"System Product Name", L"System manufacturer",
                                  L"Not Applicable", L"None", L"O.E.M.", L"x.x", L"Undefined" };
    if (!s[0]) return true;
    for (int i = 0; i < countof(bad); i++)
        if (!_wcsicmp(s, bad[i])) return true;
    return false;
}

// The short names people know vendors by.
static void vendor_short(WCHAR *s, int cap)
{
    static const struct { const WCHAR *prefix, *name; } k[] = {
        { L"Micro-Star", L"MSI" }, { L"ASUSTeK", L"ASUS" }, { L"Gigabyte", L"Gigabyte" }, { L"ASRock", L"ASRock" },
        { L"American Megatrends", L"AMI" }, { L"Hewlett-Packard", L"HP" }, { L"HP ", L"HP" }, { L"Dell", L"Dell" },
        { L"LENOVO", L"Lenovo" }, { L"Acer", L"Acer" }, { L"Insyde", L"Insyde" }, { L"Phoenix", L"Phoenix" }, { L"BIOSTAR", L"Biostar" },
        { L"EVGA", L"EVGA" }, { L"NZXT", L"NZXT" }, { L"Intel Corporation", L"Intel" }, { L"Apple", L"Apple" }, { L"Microsoft", L"Microsoft" },
    };
    for (int i = 0; i < countof(k); i++)
        if (wstarts_with_i(s, k[i].prefix)) {
            wcopy(s, cap, k[i].name);
            return;
        }
}

// Firmware and driver dates come as M/D/YYYY or M-D-YYYY; show them as DD.MM.YYYY.
static void date_dmy(WCHAR *s, int cap)
{
    int m, d, y;
    if (swscanf(s, L"%d%*[/-]%d%*[/-]%d", &m, &d, &y) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31) wfmt(s, cap, L"%02d.%02d.%d", d, m, y);
}

// --- SMBIOS --------------------------------------------------------------------------------------------------------

typedef struct SmbMem {
    WCHAR loc[48], maker[48], part[64];
    u64 bytes;
    int speed, conf, type;
} SmbMem;

static struct {
    SmbMem mem[16];
    int nmem, slots;
    WCHAR socket[48];
} SMB;

static void smb_string(const u8 *s, int idx, WCHAR *out, int cap)
{
    out[0] = 0;
    if (!idx) return;
    for (int i = 1; *s; i++) {
        int n = (int)strlen((const char *)s);
        if (i == idx) {
            int r = MultiByteToWideChar(CP_ACP, 0, (const char *)s, n, out, cap - 1);
            out[MAX(0, r)] = 0;
            trim(out);
            return;
        }
        s += n + 1;
    }
}

#define SMBIOS_PROVIDER 0x52534D42u  // 'RSMB'

static void smbios_read(void)
{
    UINT size = GetSystemFirmwareTable(SMBIOS_PROVIDER, 0, NULL, 0);
    u8 *buf = size ? (u8 *)malloc(size) : NULL;
    if (!buf || GetSystemFirmwareTable(SMBIOS_PROVIDER, 0, buf, size) != size || size < 8) {
        free(buf);
        return;
    }
    u32 len = *(u32 *)(buf + 4);
    const u8 *p = buf + 8, *end = buf + 8 + MIN(len, size - 8);
    while (p + 4 <= end) {
        u8 type = p[0], hl = p[1];
        if (hl < 4 || p + hl > end) break;
        const u8 *str = p + hl;
        if (type == 4 && hl > 0x04 && !SMB.socket[0]) smb_string(str, p[0x04], SMB.socket, countof(SMB.socket));
        if (type == 17 && hl >= 0x15) {
            SMB.slots++;
            u16 sz = *(const u16 *)(p + 0x0C);
            u64 bytes = 0;
            if (sz == 0x7FFF && hl >= 0x20) bytes = (u64)*(const u32 *)(p + 0x1C) << 20;
            else if (sz != 0xFFFF && sz) bytes = (sz & 0x8000) ? (u64)(sz & 0x7FFF) << 10 : (u64)sz << 20;
            if (bytes && SMB.nmem < countof(SMB.mem)) {
                SmbMem *m = &SMB.mem[SMB.nmem++];
                m->bytes = bytes;
                smb_string(str, p[0x10], m->loc, countof(m->loc));
                m->type = p[0x12];
                m->speed = *(const u16 *)(p + 0x15);
                if (hl >= 0x1B) {
                    smb_string(str, p[0x17], m->maker, countof(m->maker));
                    smb_string(str, p[0x1A], m->part, countof(m->part));
                }
                m->conf = hl >= 0x22 ? *(const u16 *)(p + 0x20) : 0;
            }
        }
        // the strings end with a double zero
        const u8 *q = str;
        while (q + 1 < end && (q[0] || q[1])) q++;
        p = q + 2;
        if (type == 127) break;
    }
    free(buf);
}

static const WCHAR *mem_type(int t)
{
    switch (t) {
    case 0x18: return L"DDR3";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    }
    return L"";
}

// Some boards give the maker as a JEDEC code.
static void mem_maker(WCHAR *s)
{
    static const struct { const WCHAR *code, *name; } k[] = {
        { L"CE", L"Samsung" }, { L"AD", L"SK hynix" }, { L"2C", L"Micron" }, { L"CD", L"G.Skill" }, { L"9E", L"Corsair" },
        { L"98", L"Kingston" }, { L"9B", L"Crucial" }, { L"EF", L"Team Group" }, { L"51", L"Qimonda" }, { L"F1", L"Patriot" },
        { L"B0", L"ADATA" }, { L"94", L"Mushkin" },
    };
    bool hex = wlen(s) >= 4;
    for (int i = 0; s[i] && hex; i++) hex = iswxdigit(s[i]) != 0;
    if (!hex) return;
    for (int i = 0; i < countof(k); i++)
        if (!_wcsnicmp(s + 2, k[i].code, 2) || !_wcsnicmp(s, k[i].code, 2)) {
            wcopy(s, 48, k[i].name);
            return;
        }
}

// --- the cards -----------------------------------------------------------------------------------------------------

static void hw_cpu(const CpuInfo *ci)
{
    HwCard *c = &HW.card[HWC_CPU];
    c->glyph = 0xEEA1;
    wcopy(c->cat, countof(c->cat), TR("Процессор", "Processor"));
    wcopy(c->head, countof(c->head), ci->name[0] ? ci->name : L"CPU");
    wcopy(SCORE.cpu_name, countof(SCORE.cpu_name), ci->name);
    SCORE.threads = ci->count;
    int cores = 0, pcores = 0;
    for (int i = 0; i < ci->count; i++) cores = MAX(cores, ci->core[i] + 1);
    if (ci->hybrid) {
        u8 top = 0;
        for (int i = 0; i < ci->count; i++) top = MAX(top, ci->eff[i]);
        bool seen[MAX_CPUS] = { 0 };
        for (int i = 0; i < ci->count; i++)
            if (ci->eff[i] == top && !seen[ci->core[i]]) {
                seen[ci->core[i]] = true;
                pcores++;
            }
    }
    wfmt(c->sub, countof(c->sub), TR("%d ядер · %d потоков", "%d cores · %d threads"), cores, ci->count);
    const WCHAR *cpu_key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    DWORD mhz = reg_dw(HKEY_LOCAL_MACHINE, cpu_key, L"~MHz", 0);
    if (mhz) row_add(c, false, TR("Базовая частота", "Base clock"), L"%.1f %s", mhz / 1000.0, TR("ГГц", "GHz"));
    row_add(c, false, TR("Ядра / потоки", "Cores / threads"), L"%d / %d", cores, ci->count);
    if (ci->hybrid) row_add(c, false, TR("P / E-ядра", "P / E cores"), L"%d / %d", pcores, cores - pcores);

    u64 l2 = 0, l3 = 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
    u8 *buf = len ? (u8 *)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
            off += e->Size;
            if (e->Cache.Level == 2) l2 += e->Cache.CacheSize;
            if (e->Cache.Level == 3) l3 += e->Cache.CacheSize;
        }
    }
    free(buf);
    const WCHAR *mb = TR("МБ", "MB");
    if (l2) row_add(c, false, TR("Кэш L2", "L2 cache"), L"%g %s", (f64)l2 / (1024 * 1024), mb);
    if (l3) row_add(c, false, TR("Кэш L3", "L3 cache"), L"%g %s", (f64)l3 / (1024 * 1024), mb);
    if (SMB.socket[0] && !junk(SMB.socket)) row_add(c, false, TR("Сокет", "Socket"), L"%s", SMB.socket);
    WCHAR vendor[32];
    if (reg_str(HKEY_LOCAL_MACHINE, cpu_key, L"VendorIdentifier", vendor, countof(vendor)))
        row_add(c, false, TR("Производитель", "Vendor"), L"%s",
                !wcscmp(vendor, L"AuthenticAMD") ? L"AMD" : !wcscmp(vendor, L"GenuineIntel") ? L"Intel" : vendor);

    wfmt(HW.stat[0].value, countof(HW.stat[0].value), L"%d", ci->count);
    wcopy(HW.stat[0].unit, countof(HW.stat[0].unit), L"");
    wcopy(HW.stat[0].label, countof(HW.stat[0].label), TR("потоков процессора", "CPU threads"));
    HW.stat[0].glyph = 0xEEA1;
}

static const DEVPROPKEY k_pci_speed = { { 0x3ab22e31, 0x8264, 0x4b4e, { 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62 } }, 9 };
static const DEVPROPKEY k_pci_width = { { 0x3ab22e31, 0x8264, 0x4b4e, { 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62 } }, 10 };
static const DEVPROPKEY k_pci_max_width = { { 0x3ab22e31, 0x8264, 0x4b4e, { 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62 } }, 12 };

static DWORD dev_u32(HDEVINFO set, SP_DEVINFO_DATA *d, const DEVPROPKEY *key)
{
    DEVPROPTYPE type;
    DWORD v = 0;
    if (!SetupDiGetDevicePropertyW(set, d, key, &type, (BYTE *)&v, sizeof v, NULL, 0)) return 0;
    return v;
}

typedef struct GpuInfo {
    WCHAR id[200], name[128], ver[64], date[32], amd[48];
    u64 vram;
    DWORD speed, width, max_width;
    bool integrated;
} GpuInfo;

static const WCHAR *gpu_vendor(const WCHAR *id)
{
    return wcsstr(id, L"VEN_10DE") ? L"NVIDIA" : wcsstr(id, L"VEN_1002") ? L"AMD" : wcsstr(id, L"VEN_8086") ? L"Intel" : L"";
}

// The driver version as each vendor shows it: NVIDIA's own number hides in the last digits of the Windows one
// (32.0.15.6094 -> 560.94), AMD writes its Adrenalin version next to the driver, Intel uses the Windows one as is.
static void gpu_driver_text(const GpuInfo *g, WCHAR *out, int cap)
{
    int a, b, c, d;
    if (!wcscmp(gpu_vendor(g->id), L"NVIDIA") && swscanf(g->ver, L"%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        int nv = (c % 10) * 10000 + d;
        wfmt(out, cap, L"%d.%02d  (%s)", nv / 100, nv % 100, g->ver);
    } else if (g->amd[0]) {
        wfmt(out, cap, L"Adrenalin %s  (%s)", g->amd, g->ver);
    } else {
        wcopy(out, cap, g->ver);
    }
}

static void hw_gpu(void)
{
    HwCard *c = &HW.card[HWC_GPU];
    c->glyph = 0xF211;
    wcopy(c->cat, countof(c->cat), TR("Видеокарта", "Graphics"));
    GpuInfo gi[MAX_GPUS];
    int n = 0;
    HDEVINFO set = SetupDiGetClassDevsW(&k_class_display, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA d = { sizeof d };
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && n < MAX_GPUS && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        GpuInfo *g = &gi[n];
        memset(g, 0, sizeof *g);
        if (!SetupDiGetDeviceInstanceIdW(set, &d, g->id, countof(g->id), NULL) || !wstarts_with_i(g->id, L"PCI\\")) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)g->name, sizeof g->name - 2, NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)g->name, sizeof g->name - 2, NULL))
            continue;
        HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            DWORD sz = sizeof g->ver;
            RegGetValueW(k, NULL, L"DriverVersion", RRF_RT_REG_SZ, NULL, g->ver, &sz);
            sz = sizeof g->date;
            RegGetValueW(k, NULL, L"DriverDate", RRF_RT_REG_SZ, NULL, g->date, &sz);
            sz = sizeof g->amd;
            RegGetValueW(k, NULL, L"RadeonSoftwareVersion", RRF_RT_REG_SZ, NULL, g->amd, &sz);
            sz = sizeof g->vram;
            if (RegGetValueW(k, NULL, L"HardwareInformation.qwMemorySize", RRF_RT_REG_QWORD | RRF_RT_REG_BINARY, NULL, &g->vram, &sz) != ERROR_SUCCESS) {
                DWORD v32 = 0;
                sz = sizeof v32;
                if (RegGetValueW(k, NULL, L"HardwareInformation.MemorySize", RRF_RT_REG_DWORD | RRF_RT_REG_BINARY, NULL, &v32, &sz) == ERROR_SUCCESS)
                    g->vram = v32;
            }
            RegCloseKey(k);
        }
        date_dmy(g->date, countof(g->date));
        g->speed = dev_u32(set, &d, &k_pci_speed);
        g->width = dev_u32(set, &d, &k_pci_width);
        g->max_width = dev_u32(set, &d, &k_pci_max_width);
        g->integrated = gpu_is_integrated(g->id);
        n++;
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if (!n) {
        wcopy(c->head, countof(c->head), TR("Не найдена", "Not found"));
        return;
    }
    // The main one: a discrete card before an iGPU, then the one with the most memory.
    int m = 0;
    for (int i = 1; i < n; i++)
        if ((gi[m].integrated && !gi[i].integrated) || (gi[i].integrated == gi[m].integrated && gi[i].vram > gi[m].vram)) m = i;
    GpuInfo *g = &gi[m];
    wcopy(c->head, countof(c->head), g->name);
    wcopy(SCORE.gpu_name, countof(SCORE.gpu_name), g->name);
    SCORE.vram = g->vram;
    SCORE.gpu_integrated = g->integrated;
    WCHAR vs[32] = L"";
    if (g->vram) fmt_size(vs, countof(vs), g->vram);
    const WCHAR *vend = gpu_vendor(g->id);
    wfmt(c->sub, countof(c->sub), L"%s%s%s%s", vend, vend[0] && vs[0] ? L" · " : L"", vs,
         g->integrated ? TR(" · встроенная", " · integrated") : L"");
    if (g->vram) row_add(c, false, g->integrated ? TR("Выделенная память", "Dedicated memory") : TR("Видеопамять", "Video memory"), L"%s", vs);
    WCHAR dv[128];
    gpu_driver_text(g, dv, countof(dv));
    if (dv[0]) row_add(c, false, TR("Драйвер", "Driver"), L"%s", dv);
    if (g->date[0]) row_add(c, false, TR("Дата драйвера", "Driver date"), L"%s", g->date);
    if (g->speed && g->width) {
        if (g->max_width && g->max_width != g->width)
            row_add(c, false, TR("Шина", "Bus"), L"PCIe %d.0 x%lu (%s x%lu)", (int)g->speed, g->width, TR("макс.", "max"), g->max_width);
        else
            row_add(c, false, TR("Шина", "Bus"), L"PCIe %d.0 x%lu", (int)g->speed, g->width);
    } else if (g->integrated) {
        row_add(c, false, TR("Шина", "Bus"), L"%s", TR("встроена в процессор", "inside the CPU"));
    }
    // where its interrupts go: the cores from the GPU Affinity page, another tool's mask, or Windows' choice
    u64 mask = affinity_get(g->id);
    WCHAR mt[96];
    mask_text(mask, mt, countof(mt));
    row_add(c, false, TR("Прерывания", "Interrupts"), L"%s", mask ? mt : TR("Windows по умолчанию", "Windows default"));
    for (int i = 0; i < n; i++)
        if (i != m) row_add(c, true, gi[i].name, L"%s", gi[i].integrated ? TR("встроенная", "integrated") : TR("ещё одна видеокарта", "another GPU"));

    HwStat *s = &HW.stat[2];
    s->glyph = 0xF211;
    if (g->vram) {
        f64 gb = (f64)g->vram / (1024.0 * 1024 * 1024);
        wfmt(s->value, countof(s->value), gb == floor(gb) || gb >= 10 ? L"%.0f" : L"%.1f", gb);
    } else {
        wcopy(s->value, countof(s->value), L"-");
    }
    wcopy(s->unit, countof(s->unit), k_gb);
    wcopy(s->label, countof(s->label), TR("видеопамяти", "video memory"));
}

static void hw_ram(void)
{
    HwCard *c = &HW.card[HWC_RAM];
    c->glyph = 0xE964;
    wcopy(c->cat, countof(c->cat), TR("Оперативная память", "Memory"));
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    u64 installed = 0;
    for (int i = 0; i < SMB.nmem; i++) installed += SMB.mem[i].bytes;
    ULONGLONG kb = 0;
    if (!installed && GetPhysicallyInstalledSystemMemory(&kb)) installed = kb * 1024;
    if (!installed) installed = ms.ullTotalPhys;
    WCHAR total[32];
    fmt_size(total, countof(total), installed);
    const WCHAR *type = SMB.nmem ? mem_type(SMB.mem[0].type) : L"";
    wfmt(c->head, countof(c->head), L"%s%s%s", total, type[0] ? L" " : L"", type);
    int speed = SMB.nmem ? (SMB.mem[0].conf ? SMB.mem[0].conf : SMB.mem[0].speed) : 0;
    SCORE.ram = installed;
    SCORE.ram_type = SMB.nmem ? SMB.mem[0].type : 0;
    SCORE.ram_speed = speed;
    bool same = SMB.nmem > 0;
    for (int i = 1; i < SMB.nmem; i++) same &= SMB.mem[i].bytes == SMB.mem[0].bytes;
    WCHAR each[32];
    fmt_size(each, countof(each), SMB.nmem ? SMB.mem[0].bytes : 0);
    if (SMB.nmem && same && speed) wfmt(c->sub, countof(c->sub), TR("%d × %s · %d МТ/с", "%d × %s · %d MT/s"), SMB.nmem, each, speed);
    else if (speed) wfmt(c->sub, countof(c->sub), TR("%d МТ/с", "%d MT/s"), speed);
    for (int i = 0; i < SMB.nmem; i++) {
        SmbMem *m = &SMB.mem[i];
        WCHAR sz[32], maker[48];
        fmt_size(sz, countof(sz), m->bytes);
        wcopy(maker, countof(maker), m->maker);
        mem_maker(maker);
        WCHAR name[128];
        bool mk = maker[0] && !junk(maker) && _wcsicmp(maker, L"Unknown"), pt = m->part[0] && !junk(m->part);
        wfmt(name, countof(name), L"%s%s%s", mk ? maker : L"", mk && pt ? L" " : L"", pt ? m->part : L"");
        if (!name[0]) wcopy(name, countof(name), m->loc[0] ? m->loc : TR("Модуль", "Module"));
        int sp = m->conf ? m->conf : m->speed;
        if (m->speed && m->conf && m->conf != m->speed)
            row_add(c, true, name, TR("%s · %d МТ/с (макс. %d)", "%s · %d MT/s (max %d)"), sz, m->conf, m->speed);
        else if (sp)
            row_add(c, true, name, TR("%s · %d МТ/с", "%s · %d MT/s"), sz, sp);
        else
            row_add(c, true, name, L"%s", sz);
    }
    if (SMB.slots) row_add(c, false, TR("Занято слотов", "Slots used"), TR("%d из %d", "%d of %d"), SMB.nmem, SMB.slots);
    WCHAR used[32], avail[32];
    fmt_size(used, countof(used), ms.ullTotalPhys - ms.ullAvailPhys);
    fmt_size(avail, countof(avail), ms.ullTotalPhys);
    row_add(c, false, TR("Используется", "In use"), TR("%s из %s (%lu%%)", "%s of %s (%lu%%)"), used, avail, ms.dwMemoryLoad);

    HwStat *s = &HW.stat[1];
    s->glyph = 0xE964;
    f64 gb = (f64)installed / (1024.0 * 1024 * 1024);
    wfmt(s->value, countof(s->value), gb == floor(gb) ? L"%.0f" : L"%.1f", gb);
    wcopy(s->unit, countof(s->unit), k_gb);
    wcopy(s->label, countof(s->label), TR("оперативной памяти", "of memory"));
}

static void hw_board(void)
{
    HwCard *c = &HW.card[HWC_BOARD];
    c->glyph = 0xE950;
    wcopy(c->cat, countof(c->cat), TR("Материнская плата", "Motherboard"));
    const WCHAR *bk = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    WCHAR bm[96], bp[96], sm[96], sp[96], bv[96], bver[96], bdate[48];
    reg_str(HKEY_LOCAL_MACHINE, bk, L"BaseBoardManufacturer", bm, countof(bm));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"BaseBoardProduct", bp, countof(bp));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"SystemManufacturer", sm, countof(sm));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"SystemProductName", sp, countof(sp));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"BIOSVendor", bv, countof(bv));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"BIOSVersion", bver, countof(bver));
    reg_str(HKEY_LOCAL_MACHINE, bk, L"BIOSReleaseDate", bdate, countof(bdate));
    WCHAR bms[96], bvs[96], sms[96];
    wcopy(bms, countof(bms), bm);
    vendor_short(bms, countof(bms));
    wcopy(bvs, countof(bvs), bv);
    vendor_short(bvs, countof(bvs));
    wcopy(sms, countof(sms), sm);
    vendor_short(sms, countof(sms));
    date_dmy(bdate, countof(bdate));
    if (!junk(bp)) wfmt(c->head, countof(c->head), L"%s%s%s", junk(bm) ? L"" : bms, junk(bm) ? L"" : L" ", bp);
    else wcopy(c->head, countof(c->head), TR("Неизвестна", "Unknown"));
    if (!junk(bver)) wfmt(c->sub, countof(c->sub), L"BIOS %s%s%s", bver, junk(bdate) ? L"" : L" · ", junk(bdate) ? L"" : bdate);
    if (!junk(bm)) row_add(c, false, TR("Производитель", "Manufacturer"), L"%s", bm);
    if (!junk(bp)) row_add(c, false, TR("Модель", "Model"), L"%s", bp);
    // A prebuilt (a laptop, an OEM desktop) names the whole system; a board's own "MS-7C95" says nothing new.
    if (!junk(sp) && wcscmp(sp, bp) && !wcsstr(bp, sp)) row_add(c, false, TR("Система", "System"), L"%s%s%s", junk(sm) ? L"" : sms, junk(sm) ? L"" : L" ", sp);
    if (!junk(bver)) row_add(c, false, L"BIOS", L"%s%s%s", junk(bv) ? L"" : bvs, junk(bv) ? L"" : L" ", bver);
    if (!junk(bdate)) row_add(c, false, TR("Дата BIOS", "BIOS date"), L"%s", bdate);
    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    GetFirmwareType(&ft);
    row_add(c, false, TR("Режим загрузки", "Boot mode"), L"%s", ft == FirmwareTypeUefi ? L"UEFI" : ft == FirmwareTypeBios ? L"Legacy BIOS" : L"-");
    if (ft == FirmwareTypeUefi) {
        DWORD sb = reg_dw(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", 0);
        row_add(c, false, L"Secure Boot", L"%s", sb ? TR("включён", "on") : TR("выключен", "off"));
    }
}

static void hw_disks(void)
{
    HwCard *c = &HW.card[HWC_DISK];
    c->glyph = 0xEDA2;
    wcopy(c->cat, countof(c->cat), TR("Накопители", "Storage"));
    // which letters live on which disk
    WCHAR letters[32][24];
    memset(letters, 0, sizeof letters);
    DWORD drives = GetLogicalDrives();
    for (int l = 2; l < 26; l++) {
        if (!(drives & (1u << l))) continue;
        WCHAR root[8], vol[16];
        wfmt(root, countof(root), L"%c:\\", L'A' + l);
        if (GetDriveTypeW(root) != DRIVE_FIXED && GetDriveTypeW(root) != DRIVE_REMOVABLE) continue;
        wfmt(vol, countof(vol), L"\\\\.\\%c:", L'A' + l);
        HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        u8 buf[256];
        DWORD got;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &got, NULL)) {
            VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)buf;
            DWORD disk = ext->NumberOfDiskExtents ? ext->Extents[0].DiskNumber : 99;
            if (disk < 32) {
                int n = wlen(letters[disk]);
                if (n < 20) wfmt(letters[disk] + n, 24 - n, n ? L", %c:" : L"%c:", L'A' + l);
            }
        }
        CloseHandle(h);
    }
    u64 total = 0;
    int count = 0;
    for (int i = 0; i < 32; i++) {
        WCHAR path[32];
        wfmt(path, countof(path), L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
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
        CloseHandle(h);
        if (!size) continue;
        total += size;
        count++;
        WCHAR sz[32];
        fmt_size(sz, countof(sz), size);
        const WCHAR *bt = bus == BusTypeNvme ? L"NVMe" : bus == BusTypeSata ? L"SATA" : bus == BusTypeUsb ? L"USB" : bus == BusTypeRAID ? L"RAID"
                          : bus == BusTypeSas ? L"SAS" : L"";
        WCHAR v[120];
        wfmt(v, countof(v), L"%s%s%s · %s%s%s", bt, bt[0] ? L" " : L"", ssd ? L"SSD" : L"HDD", sz, letters[i][0] ? L" · " : L"", letters[i]);
        row_add(c, true, model[0] ? model : path + 4, L"%s", v);
        // the drive Windows lives on decides the score's drive part
        WCHAR sys[4] = L"C:";
        GetEnvironmentVariableW(L"SystemDrive", sys, countof(sys));
        if (wcsstr(letters[i], sys)) SCORE.sys_disk = !ssd ? 1 : bus == BusTypeNvme ? 3 : 2;
        if (count == 1) wcopy(c->head, countof(c->head), model);
    }
    WCHAR ts[32];
    fmt_size(ts, countof(ts), total);
    if (count > 1) {
        wfmt(c->head, countof(c->head), TR("%d накопителя · %s", "%d drives · %s"), count, ts);
        if (count >= 5 && g_lang_ru) wfmt(c->head, countof(c->head), L"%d накопителей · %s", count, ts);
    }
    // one drive, one display, one adapter: the header already says it all
    if (count == 1) {
        wcopy(c->sub, countof(c->sub), c->rows[0].v);
        c->nrows = 0;
    }
    else wfmt(c->sub, countof(c->sub), TR("Всего %s", "%s in total"), ts);

    HwStat *s = &HW.stat[3];
    s->glyph = 0xEDA2;
    f64 gb = (f64)total / (1024.0 * 1024 * 1024);
    if (gb >= 1000) {
        wfmt(s->value, countof(s->value), L"%.1f", gb / 1024.0);
        wcopy(s->unit, countof(s->unit), k_tb);
    } else {
        wfmt(s->value, countof(s->value), L"%.0f", gb);
        wcopy(s->unit, countof(s->unit), k_gb);
    }
    wcopy(s->label, countof(s->label), TR("на накопителях", "of storage"));
}

static void hw_displays(void)
{
    HwCard *c = &HW.card[HWC_DISPLAY];
    c->glyph = 0xE7F4;
    wcopy(c->cat, countof(c->cat), TR("Мониторы", "Displays"));
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm, sizeof *modes);
    int n = 0;
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_PATH_INFO *p = &paths[i];
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            memset(&tn, 0, sizeof tn);
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof tn;
            tn.header.adapterId = p->targetInfo.adapterId;
            tn.header.id = p->targetInfo.id;
            DisplayConfigGetDeviceInfo(&tn.header);
            WCHAR name[96];
            wcopy(name, countof(name), tn.monitorFriendlyDeviceName[0] ? tn.monitorFriendlyDeviceName : TR("Монитор", "Display"));
            int w = 0, h = 0;
            UINT32 si = p->sourceInfo.modeInfoIdx;
            if (si < nm && modes[si].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                w = (int)modes[si].sourceMode.width;
                h = (int)modes[si].sourceMode.height;
            }
            f64 hz = p->targetInfo.refreshRate.Denominator ? (f64)p->targetInfo.refreshRate.Numerator / p->targetInfo.refreshRate.Denominator : 0;
            const WCHAR *conn = L"";
            switch (p->targetInfo.outputTechnology) {
            case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: conn = L" · HDMI"; break;
            case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: conn = L" · DisplayPort"; break;
            case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: conn = L" · DVI"; break;
            case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
            case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: conn = TR(" · встроенный", " · built-in"); break;
            default: break;
            }
            WCHAR v[96];
            wfmt(v, countof(v), TR("%d × %d · %.0f Гц%s", "%d × %d · %.0f Hz%s"), w, h, hz + 0.01, conn);
            row_add(c, true, name, L"%s", v);
            if (n++ == 0) {
                wcopy(c->head, countof(c->head), name);
                wcopy(c->sub, countof(c->sub), v);
            }
        }
    }
    free(paths);
    free(modes);
    if (n > 1) wfmt(c->sub, countof(c->sub), TR("%d монитора", "%d displays"), n);
    else c->nrows = 0;
}

static void hw_net(void)
{
    HwCard *c = &HW.card[HWC_NET];
    c->glyph = 0xE839;
    wcopy(c->cat, countof(c->cat), TR("Сеть", "Network"));
    ULONG len = 32 * 1024;
    IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(len);
    if (aa && GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &len) ==
                  ERROR_BUFFER_OVERFLOW) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)malloc(len);
        if (aa && GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &len) != NO_ERROR) {
            free(aa);
            aa = NULL;
        }
    }
    int n = 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        if (a->OperStatus != IfOperStatusUp) continue;
        // virtual adapters (Hyper-V, VPNs, VirtualBox) are not hardware
        if (wcsstr(a->Description, L"Virtual") || wcsstr(a->Description, L"Hyper-V") || wcsstr(a->Description, L"VPN") ||
            wcsstr(a->Description, L"TAP-") || wcsstr(a->Description, L"WireGuard"))
            continue;
        u64 bps = a->TransmitLinkSpeed;
        WCHAR sp[32] = L"";
        if (bps >= 1000000000ull) wfmt(sp, countof(sp), TR(" · %g Гбит/с", " · %g Gbit/s"), (f64)bps / 1e9);
        else if (bps) wfmt(sp, countof(sp), TR(" · %llu Мбит/с", " · %llu Mbit/s"), (unsigned long long)(bps / 1000000));
        WCHAR v[64];
        wfmt(v, countof(v), L"%s%s", a->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi" : L"Ethernet", sp);
        row_add(c, true, a->Description, L"%s", v);
        if (n++ == 0) {
            wcopy(c->head, countof(c->head), a->Description);
            wcopy(c->sub, countof(c->sub), v);
        }
    }
    free(aa);
    if (!n) wcopy(c->head, countof(c->head), TR("Нет подключения", "Not connected"));
    if (n == 1) c->nrows = 0;
}

static void hw_os(void)
{
    HwCard *c = &HW.card[HWC_OS];
    c->glyph = 0xE770;
    wcopy(c->cat, countof(c->cat), TR("Система", "System"));
    const WCHAR *vk = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    WCHAR prod[96], disp[32], build[32];
    reg_str(HKEY_LOCAL_MACHINE, vk, L"ProductName", prod, countof(prod));
    reg_str(HKEY_LOCAL_MACHINE, vk, L"DisplayVersion", disp, countof(disp));
    reg_str(HKEY_LOCAL_MACHINE, vk, L"CurrentBuildNumber", build, countof(build));
    DWORD ubr = reg_dw(HKEY_LOCAL_MACHINE, vk, L"UBR", 0);
    // Windows 11 still calls itself Windows 10 in ProductName.
    if (_wtoi(build) >= 22000 && !wcsncmp(prod, L"Windows 10", 10)) prod[9] = L'1';
    wcopy(c->head, countof(c->head), prod[0] ? prod : L"Windows");
    wfmt(c->sub, countof(c->sub), TR("%s%sсборка %s.%lu", "%s%sbuild %s.%lu"), disp, disp[0] ? L" · " : L"", build, ubr);
    WCHAR pc[64];
    DWORD n = countof(pc);
    if (GetComputerNameW(pc, &n)) row_add(c, false, TR("Имя ПК", "PC name"), L"%s", pc);
    if (disp[0]) row_add(c, false, TR("Версия", "Version"), L"%s", disp);
    row_add(c, false, TR("Сборка", "Build"), L"%s.%lu", build, ubr);
    DWORD inst = reg_dw(HKEY_LOCAL_MACHINE, vk, L"InstallDate", 0);
    if (inst) {
        FILETIME ft;
        i64 t = ((i64)inst + 11644473600LL) * 10000000LL;
        ft.dwLowDateTime = (DWORD)t;
        ft.dwHighDateTime = (DWORD)(t >> 32);
        SYSTEMTIME st, lt;
        FileTimeToSystemTime(&ft, &st);
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
        row_add(c, false, TR("Установлена", "Installed"), L"%02d.%02d.%d", lt.wDay, lt.wMonth, lt.wYear);
    }
    u64 up = GetTickCount64() / 60000;
    if (up >= 1440) row_add(c, false, TR("Работает", "Uptime"), TR("%llu д %llu ч", "%llu d %llu h"), up / 1440, (up % 1440) / 60);
    else row_add(c, false, TR("Работает", "Uptime"), TR("%llu ч %llu мин", "%llu h %llu min"), up / 60, up % 60);
}

static DWORD WINAPI hw_thread(void *arg)
{
    HWND notify = (HWND)arg;
again:
    // everything is rebuilt: the texts are in the current language
    memset(HW.card, 0, sizeof HW.card);
    memset(HW.stat, 0, sizeof HW.stat);
    memset(&SMB, 0, sizeof SMB);
    memset(&SCORE, 0, sizeof SCORE);
    k_gb = TR("ГБ", "GB");
    k_tb = TR("ТБ", "TB");
    CpuInfo ci;
    cpu_info(&ci);
    smbios_read();
    hw_cpu(&ci);
    hw_gpu();
    hw_ram();
    hw_board();
    hw_disks();
    hw_displays();
    hw_net();
    hw_os();
    score_compute();
    if (InterlockedExchange(&HW.again, 0)) goto again;
    InterlockedExchange(&HW.ready, 1);
    InterlockedExchange(&HW.busy, 0);
    PostMessageW(notify, WM_APP_HW, 0, 0);
    return 0;
}

static void hw_start(HWND notify)
{
    InterlockedExchange(&HW.ready, 0);
    if (InterlockedExchange(&HW.busy, 1)) {
        InterlockedExchange(&HW.again, 1);
        return;
    }
    HANDLE t = CreateThread(NULL, 0, hw_thread, notify, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&HW.busy, 0);
}
