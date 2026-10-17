// MSI mode and interrupt priority for PCI devices, what MSI_util v3 shows and changes.
//
// Windows reads both from the device's registry key when the device starts:
//   ...\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties
//       MSISupported        = 1 for message-signaled interrupts, 0 for the old line-based ones
//       MessageNumberLimit  = how many messages the device may use (left alone here)
//   ...\Device Parameters\Interrupt Management\Affinity Policy
//       DevicePriority      = 1 low, 2 normal, 3 high; absent or 0 undefined
// A change takes effect on the next boot: restarting a disk or USB controller under a running Windows is not worth
// the risk.
//
// What the user changes is also kept in HKLM\SOFTWARE\MixBooster\Msi, one value per device, together with what was
// there before. The service writes the wanted values back when a driver install drops them, and "Reset all" puts the
// old ones back.

#define MSI_MAX 128
#define MSI_REC_KEY MB_REG_KEY L"\\Msi"

// PCI device properties (pciprop.h): the interrupt kinds the device's capabilities list, and its MSI message maximum.
static const DEVPROPKEY k_pci_irq_support = { { 0x3ab22e31, 0x8264, 0x4b4e, { 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62 } }, 14 };
static const DEVPROPKEY k_pci_irq_max = { { 0x3ab22e31, 0x8264, 0x4b4e, { 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62 } }, 15 };
enum { IRQS_LINE = 1, IRQS_MSI = 2, IRQS_MSIX = 4 };

// The groups the page shows the devices in, top to bottom.
enum { MG_GPU, MG_NET, MG_USB, MG_AUDIO, MG_DISK, MG_OTHER, MG_SYSTEM };

typedef struct MsiDev {
    WCHAR id[200], name[128];
    int group;               // MG_*
    u32 support;             // IRQS_* bits; 0 when Windows doesn't say
    u32 msg_max;
    bool has_irq, irq_msi;   // the interrupts the device has now
    int irq, nirq;           // the first IRQ (line-based) and how many it has
    int msi, prio;           // in the registry now: -1 absent, else the value
    bool managed;            // MixBooster keeps a setting for it
    int gpu_bridge;          // a PCI bridge between a GPU and the CPU: 1 the GPU's own port, 2.. further up; 0 not one
    int new_msi, new_prio;   // the page's edits, equal to msi and prio when untouched
} MsiDev;

static struct {
    MsiDev d[MSI_MAX];
    int n;
    bool ready;
} MS;

static void msi_key(const WCHAR *id, WCHAR *out, int cap)
{
    wfmt(out, cap, L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties", id);
}

static void prio_key(const WCHAR *id, WCHAR *out, int cap)
{
    wfmt(out, cap, L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters\\Interrupt Management\\Affinity Policy", id);
}

// -1 when the value is absent.
static int reg_dword_or(const WCHAR *key, const WCHAR *name)
{
    DWORD v = 0, sz = sizeof v;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? (int)v : -1;
}

// v -1 deletes the value.
static bool reg_dword_put(const WCHAR *key, const WCHAR *name, int v)
{
    HKEY k;
    if (v < 0) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return true;
        RegDeleteValueW(k, name);
        RegCloseKey(k);
        return true;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        log_msg("msi: can't open %ls", key);
        return false;
    }
    DWORD d = (DWORD)v;
    bool ok = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof d) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static int msi_get(const WCHAR *id)
{
    WCHAR key[400];
    msi_key(id, key, countof(key));
    return reg_dword_or(key, L"MSISupported");
}

static int prio_get(const WCHAR *id)
{
    WCHAR key[400];
    prio_key(id, key, countof(key));
    return reg_dword_or(key, L"DevicePriority");
}

static bool msi_put(const WCHAR *id, int v)
{
    WCHAR key[400];
    msi_key(id, key, countof(key));
    return reg_dword_put(key, L"MSISupported", v);
}

static bool prio_put(const WCHAR *id, int v)
{
    WCHAR key[400];
    prio_key(id, key, countof(key));
    return reg_dword_put(key, L"DevicePriority", v);
}

// --- the saved settings ----------------------------------------------------------------------------------------------
// One DWORD per device, a byte per field: the wanted MSI, the wanted priority, the old MSI, the old priority. In each
// byte 0 means "not ours", 1 "absent", v + 2 the value v.

typedef struct MsiRec {
    int want_msi, want_prio, old_msi, old_prio;  // -2 not ours, -1 absent, else the value
} MsiRec;

static int rec_field(DWORD v, int byte) { return (int)((v >> (byte * 8)) & 255) - 2; }
static DWORD rec_byte(int f, int byte) { return (DWORD)(f + 2) << (byte * 8); }

static bool rec_get(const WCHAR *id, MsiRec *r)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, MSI_REC_KEY, id, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) return false;
    *r = (MsiRec){ rec_field(v, 0), rec_field(v, 1), rec_field(v, 2), rec_field(v, 3) };
    return true;
}

static void rec_put(const WCHAR *id, const MsiRec *r)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, MSI_REC_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    DWORD v = rec_byte(r->want_msi, 0) | rec_byte(r->want_prio, 1) | rec_byte(r->old_msi, 2) | rec_byte(r->old_prio, 3);
    RegSetValueExW(k, id, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

static void rec_delete(const WCHAR *id) { RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, MSI_REC_KEY, id); }

// Calls fn for every saved device; returns how many there are.
static int rec_each(void (*fn)(const WCHAR *id, const MsiRec *r))
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MSI_REC_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    // the names first: fn may delete values while we go
    static WCHAR names[MSI_MAX][200];
    int n = 0;
    for (DWORD i = 0; n < MSI_MAX; i++) {
        DWORD len = countof(names[0]);
        if (RegEnumValueW(k, i, names[n], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        n++;
    }
    RegCloseKey(k);
    for (int i = 0; fn && i < n; i++) {
        MsiRec r;
        if (rec_get(names[i], &r)) fn(names[i], &r);
    }
    return n;
}

static bool msi_saved_any(void) { return rec_each(NULL) > 0; }

// --- reading the devices -------------------------------------------------------------------------------------------

// By the PCI class code first (PCI\CC_xxyy in the compatible IDs): an HD Audio controller sits in Windows' "System"
// class, but it is a sound card. The Windows class decides when there is no code.
static int msi_group_cc(const WCHAR *ids)
{
    for (const WCHAR *p = ids; p && *p; p += wcslen(p) + 1) {
        if (!wstarts_with_i(p, L"PCI\\CC_")) continue;
        // CC_xxyy, or CC_xxyyzz with the interface, maybe followed by &DT_1
        WCHAR *end;
        unsigned cc = (unsigned)wcstoul(p + 7, &end, 16);
        int digits = (int)(end - (p + 7));
        if (digits == 6) cc >>= 8;
        else if (digits != 4) continue;
        switch (cc >> 8) {
        case 0x03: return MG_GPU;
        case 0x02: return MG_NET;
        case 0x01: return MG_DISK;
        case 0x04: return MG_AUDIO;
        case 0x06: return MG_SYSTEM;
        case 0x0C: return cc == 0x0C03 ? MG_USB : MG_OTHER;
        }
        return MG_OTHER;
    }
    return -1;
}

static int msi_group(const WCHAR *cls)
{
    if (!_wcsicmp(cls, L"Display")) return MG_GPU;
    if (!_wcsicmp(cls, L"Net")) return MG_NET;
    if (!_wcsicmp(cls, L"USB")) return MG_USB;
    if (!_wcsicmp(cls, L"MEDIA")) return MG_AUDIO;
    if (!_wcsicmp(cls, L"SCSIAdapter") || !_wcsicmp(cls, L"HDC")) return MG_DISK;
    if (!_wcsicmp(cls, L"System")) return MG_SYSTEM;
    return MG_OTHER;
}

static void msi_irqs(DEVINST inst, MsiDev *m)
{
    LOG_CONF lc;
    if (CM_Get_First_Log_Conf(&lc, inst, ALLOC_LOG_CONF) != CR_SUCCESS) return;
    RES_DES rd, next;
    RESOURCEID rid;
    if (CM_Get_Next_Res_Des(&rd, lc, ResType_IRQ, &rid, 0) == CR_SUCCESS) {
        for (;;) {
            ULONG sz = 0;
            if (CM_Get_Res_Des_Data_Size(&sz, rd, 0) == CR_SUCCESS && sz >= sizeof(IRQ_DES)) {
                BYTE buf[256];
                if (sz <= sizeof buf && CM_Get_Res_Des_Data(rd, buf, sz, 0) == CR_SUCCESS) {
                    // an MSI message shows as a "negative" IRQ, the way Device Manager prints 4294967xxx
                    LONG irq = (LONG)((IRQ_DES *)buf)->IRQD_Alloc_Num;
                    if (!m->nirq) {
                        m->irq = (int)irq;
                        m->irq_msi = irq < 0;
                    }
                    m->nirq++;
                    m->has_irq = true;
                }
            }
            if (CM_Get_Next_Res_Des(&next, rd, ResType_IRQ, &rid, 0) != CR_SUCCESS) break;
            CM_Free_Res_Des_Handle(rd);
            rd = next;
        }
        CM_Free_Res_Des_Handle(rd);
    }
    CM_Free_Log_Conf_Handle(lc);
}

static int msi_cmp(const void *a, const void *b)
{
    const MsiDev *x = (const MsiDev *)a, *y = (const MsiDev *)b;
    if (x->group != y->group) return x->group - y->group;
    return _wcsicmp(x->name, y->name);
}

// Every present PCI device that has an interrupt or could have one, grouped and sorted by name.
static int msi_scan(MsiDev *out, int cap)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA d = { sizeof d };
    for (DWORD i = 0; n < cap && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        MsiDev *m = &out[n];
        memset(m, 0, sizeof *m);
        if (!SetupDiGetDeviceInstanceIdW(set, &d, m->id, countof(m->id), NULL)) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)m->name, sizeof m->name - sizeof(WCHAR), NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)m->name, sizeof m->name - sizeof(WCHAR), NULL))
            wcopy(m->name, countof(m->name), m->id);
        WCHAR cls[64] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_CLASS, NULL, (BYTE *)cls, sizeof cls - sizeof(WCHAR), NULL);
        static WCHAR ids[2048];
        memset(ids, 0, sizeof ids);
        SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_COMPATIBLEIDS, NULL, (BYTE *)ids, sizeof ids - 2 * sizeof(WCHAR), NULL);
        m->group = msi_group_cc(ids);
        if (m->group < 0) m->group = msi_group(cls);
        DEVPROPTYPE type;
        u32 v = 0;
        if (SetupDiGetDevicePropertyW(set, &d, &k_pci_irq_support, &type, (BYTE *)&v, sizeof v, NULL, 0)) m->support = v;
        v = 0;
        if (SetupDiGetDevicePropertyW(set, &d, &k_pci_irq_max, &type, (BYTE *)&v, sizeof v, NULL, 0)) m->msg_max = v;
        msi_irqs(d.DevInst, m);
        if (!m->has_irq && !(m->support & (IRQS_MSI | IRQS_MSIX))) continue;
        m->msi = msi_get(m->id);
        m->prio = prio_get(m->id);
        MsiRec r;
        m->managed = rec_get(m->id, &r);
        m->new_msi = m->msi;
        m->new_prio = m->prio;
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    // the bridges up from each GPU, as gpu.c finds them for the affinity
    for (int i = 0; i < n; i++) {
        if (out[i].group != MG_GPU) continue;
        DEVINST cur, parent;
        if (CM_Locate_DevNodeW(&cur, out[i].id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        WCHAR pid[200];
        for (int depth = 1; depth <= 4 && CM_Get_Parent(&parent, cur, 0) == CR_SUCCESS &&
                            CM_Get_Device_IDW(parent, pid, countof(pid), 0) == CR_SUCCESS && wstarts_with_i(pid, L"PCI\\");
             depth++, cur = parent)
            for (int j = 0; j < n; j++)
                if (!_wcsicmp(out[j].id, pid) && (!out[j].gpu_bridge || out[j].gpu_bridge > depth)) out[j].gpu_bridge = depth;
    }
    qsort(out, (size_t)n, sizeof *out, msi_cmp);
    return n;
}

// A device already running on MSI can obviously do it, even when Windows doesn't list its capabilities (PCIe ports).
static bool msi_capable(const MsiDev *m) { return (m->support & (IRQS_MSI | IRQS_MSIX)) != 0 || m->irq_msi; }

// What the MSI switch shows: the registry's choice, or with none there what the device runs on.
static bool msi_shown_on(const MsiDev *m) { return m->new_msi < 0 ? m->irq_msi : m->new_msi == 1; }

static bool msi_dirty(const MsiDev *m) { return m->new_msi != m->msi || m->new_prio != m->prio; }

// Set in the registry but not running yet: the device picks it up on the next boot.
static bool msi_pending(const MsiDev *m) { return m->has_irq && msi_capable(m) && m->msi >= 0 && (m->msi == 1) != m->irq_msi; }

// --- the MixBoost playbook's "MixBoost MSI Mode" task -------------------------------------------------------------
// The playbook turns MSI on for every capable device but disk and audio controllers, sets High priority for the GPUs,
// each GPU's own PCI port and the network adapters, and keeps that with a scheduled task. Two keepers would fight over
// the same values, so the first Apply here adopts what the playbook set and removes its task; "Reset all" registers
// the task again with the playbook's own script, when it is still there.

#define PB_TASK L"MixBoost MSI Mode"

static bool pb_task_present(void)
{
    WCHAR win[MAX_PATH], p[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    wfmt(p, MAX_PATH, L"%s\\System32\\Tasks\\%s", win, PB_TASK);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void pb_script(WCHAR *out)
{
    WCHAR win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    wfmt(out, MAX_PATH, L"%s\\MixBoostModules\\Scripts\\setMsiMode.ps1", win);
}

// Runs a command without a window and waits for it; false when it couldn't start or failed.
static bool run_hidden(const WCHAR *cmd, DWORD wait_ms)
{
    WCHAR line[1024];
    wcopy(line, countof(line), cmd);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_msg("can't run %ls: %lu", cmd, GetLastError());
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// Whether the playbook's values were taken over: they stay kept even where they equal what was there before.
static bool pb_adopted(void)
{
    u64 v = 0;
    return reg_get_u64(L"PlaybookMsiTask", &v) && v;
}

// Takes the playbook's values on as MixBooster's own (so the service keeps them) and removes its task.
static void pb_takeover(const MsiDev *d, int n)
{
    for (int i = 0; i < n; i++) {
        const MsiDev *m = &d[i];
        bool msi = m->group != MG_DISK && m->group != MG_AUDIO && (m->support & (IRQS_MSI | IRQS_MSIX)) && m->msi == 1;
        bool prio = (m->group == MG_GPU || m->group == MG_NET || m->gpu_bridge == 1) && m->prio == 3;
        if (!msi && !prio) continue;
        MsiRec r;
        if (!rec_get(m->id, &r)) r = (MsiRec){ -2, -2, -2, -2 };
        // "before" is the playbook's value too: Reset all leaves it, and the task comes back to keep it
        if (msi && r.want_msi == -2) r.want_msi = r.old_msi = 1;
        if (prio && r.want_prio == -2) r.want_prio = r.old_prio = 3;
        rec_put(m->id, &r);
    }
    reg_set_u64(L"PlaybookMsiTask", 1);
    WCHAR cmd[256];
    wfmt(cmd, countof(cmd), L"schtasks.exe /Delete /TN \"%s\" /F", PB_TASK);
    bool ok = run_hidden(cmd, 20000);
    log_msg("msi: took over the playbook's settings, its task %s", ok ? "removed" : "could not be removed");
}

// Gives the playbook its task back, after Reset all.
static void pb_restore(void)
{
    if (!pb_adopted()) return;
    reg_delete(L"PlaybookMsiTask");
    WCHAR script[MAX_PATH], cmd[MAX_PATH + 128];
    pb_script(script);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        log_msg("msi: the playbook's script is gone, its task is not restored");
        return;
    }
    wfmt(cmd, countof(cmd), L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" -Register", script);
    bool ok = run_hidden(cmd, 120000);
    log_msg("msi: the playbook's task %s", ok ? "registered again" : "could not be registered again");
}

// --- applying, restoring, resetting ----------------------------------------------------------------------------------

// Writes one device's edits and keeps them for the service, with what was there before.
static void msi_apply_one(const MsiDev *m)
{
    MsiRec r;
    if (!rec_get(m->id, &r)) r = (MsiRec){ -2, -2, -2, -2 };
    if (m->new_msi != m->msi) {
        if (r.old_msi == -2) r.old_msi = m->msi;
        msi_put(m->id, m->new_msi);
        r.want_msi = m->new_msi == r.old_msi && !pb_adopted() ? -2 : m->new_msi;
        log_msg("msi: %ls MSISupported -> %d", m->id, m->new_msi);
    }
    if (m->new_prio != m->prio) {
        if (r.old_prio == -2) r.old_prio = m->prio;
        prio_put(m->id, m->new_prio);
        r.want_prio = m->new_prio == r.old_prio && !pb_adopted() ? -2 : m->new_prio;
        log_msg("msi: %ls DevicePriority -> %d", m->id, m->new_prio);
    }
    if (r.want_msi == -2 && r.want_prio == -2) rec_delete(m->id);
    else rec_put(m->id, &r);
}

// The service: puts back what a driver install dropped or changed.
static int g_msi_fixed;

static void msi_restore_one(const WCHAR *id, const MsiRec *r)
{
    // a device that is gone (a card taken out) keeps its record, in case it comes back
    DEVINST inst;
    if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return;
    if (r->want_msi != -2 && msi_get(id) != r->want_msi) {
        msi_put(id, r->want_msi);
        g_msi_fixed++;
        log_msg("service: MSI of %ls back to %d", id, r->want_msi);
    }
    if (r->want_prio != -2 && prio_get(id) != r->want_prio) {
        prio_put(id, r->want_prio);
        g_msi_fixed++;
        log_msg("service: priority of %ls back to %d", id, r->want_prio);
    }
}

static int msi_restore(void)
{
    g_msi_fixed = 0;
    rec_each(msi_restore_one);
    return g_msi_fixed;
}

static void msi_reset_one(const WCHAR *id, const MsiRec *r)
{
    if (r->want_msi != -2 && r->old_msi != -2) msi_put(id, r->old_msi);
    if (r->want_prio != -2 && r->old_prio != -2) prio_put(id, r->old_prio);
    rec_delete(id);
    log_msg("msi: %ls reset", id);
}

// Everything MixBooster changed goes back to how it was.
static void msi_reset_all(void)
{
    rec_each(msi_reset_one);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, MSI_REC_KEY);
}
