// GPUs, the PCI bridge each one hangs off, their interrupt affinity in the registry, driver restarts, and the CPU layout.
//
// Windows reads "Affinity Policy" when a device starts:
//   DevicePolicy          = 4 (IrqPolicySpecifiedProcessors)
//   AssignmentSetOverride = KAFFINITY mask of the allowed logical processors
// This is what AutoGpuAffinity and the Interrupt Affinity Policy tool write. A GPU picks the change up on a driver
// restart; a bridge only on the next boot, since restarting it would take the GPU down with it.

#define MAX_GPUS 4
#define MAX_CPUS 64

typedef struct GpuDev {
    WCHAR id[200];          // PCI\VEN_...
    WCHAR name[128];
    WCHAR bridge[200];      // the parent PCI bridge, or empty
} GpuDev;

static const GUID k_class_display = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
// GUID_DEVINTERFACE_DISPLAY_ADAPTER: arrives again whenever the driver is installed or updated.
static const GUID k_iface_display = { 0x5b45201d, 0xf2f2, 0x4f3b, { 0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x35, 0x99 } };

static int gpu_list(GpuDev *out, int cap)
{
    HDEVINFO set = SetupDiGetClassDevsW(&k_class_display, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA d = { sizeof d };
    for (DWORD i = 0; n < cap && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        GpuDev *g = &out[n];
        memset(g, 0, sizeof *g);
        if (!SetupDiGetDeviceInstanceIdW(set, &d, g->id, countof(g->id), NULL) || !wstarts_with_i(g->id, L"PCI\\")) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)g->name, sizeof g->name - sizeof(WCHAR), NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)g->name, sizeof g->name - sizeof(WCHAR), NULL))
            wcopy(g->name, countof(g->name), g->id);
        DEVINST parent;
        WCHAR pid[200];
        if (CM_Get_Parent(&parent, d.DevInst, 0) == CR_SUCCESS && CM_Get_Device_IDW(parent, pid, countof(pid), 0) == CR_SUCCESS &&
            wstarts_with_i(pid, L"PCI\\"))
            wcopy(g->bridge, countof(g->bridge), pid);
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static void affinity_key(const WCHAR *id, WCHAR *out, int cap)
{
    wfmt(out, cap, L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters\\Interrupt Management\\Affinity Policy", id);
}

// The mask the device is pinned to, 0 when it has no specified-processors policy.
static u64 affinity_get(const WCHAR *id)
{
    WCHAR key[400];
    affinity_key(id, key, countof(key));
    DWORD policy = 0, sz = sizeof policy;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DevicePolicy", RRF_RT_REG_DWORD, NULL, &policy, &sz) != ERROR_SUCCESS || policy != 4) return 0;
    u64 mask = 0;
    sz = sizeof mask;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"AssignmentSetOverride", RRF_RT_REG_BINARY, NULL, &mask, &sz) != ERROR_SUCCESS) return 0;
    return mask;
}

// mask 0 drops the policy and lets Windows choose again. DevicePriority (set by the MSI mode task) is left alone.
static bool affinity_set(const WCHAR *id, u64 mask)
{
    WCHAR key[400];
    affinity_key(id, key, countof(key));
    if (!mask) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return true;
        RegDeleteValueW(k, L"DevicePolicy");
        RegDeleteValueW(k, L"AssignmentSetOverride");
        RegCloseKey(k);
        return true;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        log_msg("affinity: can't open %ls", key);
        return false;
    }
    DWORD policy = 4;
    bool ok = RegSetValueExW(k, L"DevicePolicy", 0, REG_DWORD, (const BYTE *)&policy, sizeof policy) == ERROR_SUCCESS &&
              RegSetValueExW(k, L"AssignmentSetOverride", 0, REG_BINARY, (const BYTE *)&mask, sizeof mask) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

// Restarts a device's driver (what "devcon restart" and CRU's restart64 do). The screen goes black for a moment.
static bool device_restart(const WCHAR *id)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA d = { sizeof d };
    bool ok = false;
    if (SetupDiOpenDeviceInfoW(set, id, NULL, 0, &d)) {
        SP_PROPCHANGE_PARAMS p;
        memset(&p, 0, sizeof p);
        p.ClassInstallHeader.cbSize = sizeof p.ClassInstallHeader;
        p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        p.StateChange = DICS_PROPCHANGE;
        p.Scope = DICS_FLAG_GLOBAL;
        ok = SetupDiSetClassInstallParamsW(set, &d, &p.ClassInstallHeader, sizeof p) && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &d);
        if (!ok) log_msg("restart %ls failed: %lu", id, GetLastError());
        // Some drivers refuse to stop and leave the change for the next boot.
        SP_DEVINSTALL_PARAMS_W ip = { sizeof ip };
        if (ok && SetupDiGetDeviceInstallParamsW(set, &d, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) {
            log_msg("restart %ls needs a reboot", id);
            ok = false;
        }
        DEVINST inst = d.DevInst;
        // Wait until the driver is back.
        for (int i = 0; ok && i < 100; i++) {
            ULONG st = 0, prob = 0;
            if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
                CM_Get_DevNode_Status(&st, &prob, inst, 0) == CR_SUCCESS && (st & DN_STARTED))
                break;
            Sleep(100);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

// Pins every GPU (and, when bridges is set, its PCI bridge) to mask and restarts the GPUs whose mask changed.
// force restarts them even when the registry already matched. Returns the number of GPUs restarted, -1 if one that
// had to restart could not.
static int gpus_apply(u64 mask, bool bridges, bool force)
{
    GpuDev g[MAX_GPUS];
    int n = gpu_list(g, MAX_GPUS), restarted = 0;
    bool failed = false;
    for (int i = 0; i < n; i++) {
        bool changed = affinity_get(g[i].id) != mask;
        if (changed) affinity_set(g[i].id, mask);
        if (bridges && g[i].bridge[0] && affinity_get(g[i].bridge) != mask) {
            affinity_set(g[i].bridge, mask);
            log_msg("bridge %ls -> 0x%llx", g[i].bridge, (unsigned long long)mask);
        }
        if (changed || force) {
            log_msg("gpu %ls -> 0x%llx", g[i].id, (unsigned long long)mask);
            if (device_restart(g[i].id)) restarted++;
            else failed = true;
        }
    }
    if (restarted) Sleep(2500);  // let the display settle before anything draws to it
    return failed ? -1 : restarted;
}

static void bridges_clear(void)
{
    GpuDev g[MAX_GPUS];
    int n = gpu_list(g, MAX_GPUS);
    for (int i = 0; i < n; i++)
        if (g[i].bridge[0]) affinity_set(g[i].bridge, 0);
}

typedef struct CpuInfo {
    int count;              // logical processors in group 0, at most 64
    u8 core[MAX_CPUS];      // physical core of each logical processor
    u8 eff[MAX_CPUS];       // efficiency class: higher is faster on hybrid CPUs
    bool hybrid;
    WCHAR name[96];
} CpuInfo;

static void cpu_info(CpuInfo *ci)
{
    memset(ci, 0, sizeof *ci);
    ci->count = MIN((int)GetActiveProcessorCount(0), MAX_CPUS);
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    u8 *buf = len ? (u8 *)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        int core = 0;
        u8 lo = 255, hi = 0;
        for (DWORD off = 0; off < len; core++) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
            off += e->Size;
            if (e->Processor.GroupMask[0].Group != 0) continue;
            KAFFINITY m = e->Processor.GroupMask[0].Mask;
            for (int c = 0; c < ci->count; c++)
                if (m & ((KAFFINITY)1 << c)) {
                    ci->core[c] = (u8)core;
                    ci->eff[c] = e->Processor.EfficiencyClass;
                }
            lo = MIN(lo, e->Processor.EfficiencyClass);
            hi = MAX(hi, e->Processor.EfficiencyClass);
        }
        ci->hybrid = hi > lo;
    }
    free(buf);
    DWORD sz = sizeof ci->name - sizeof(WCHAR);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, NULL,
                     ci->name, &sz) != ERROR_SUCCESS)
        ci->name[0] = 0;
    // Trim the padding some vendors put in the name.
    int a = 0, b = wlen(ci->name);
    while (ci->name[a] == ' ') a++;
    while (b > a && ci->name[b - 1] == ' ') b--;
    memmove(ci->name, ci->name + a, (size_t)(b - a) * sizeof(WCHAR));
    ci->name[b - a] = 0;
}

static int mask_cpu(u64 mask)
{
    for (int i = 0; i < 64; i++)
        if (mask == (1ull << i)) return i;
    return -1;
}
