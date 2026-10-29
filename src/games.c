// Game fixes for chosen game exes, each on its own switch (FX_*):
//
//   CPU High   HKLM\...\Image File Execution Options\<exe>\PerfOptions  CpuPriorityClass = 3: Windows starts the
//              process with High priority itself; the service sets it again, in case the game lowered it.
//   GPU High   the service sets the process's GPU scheduling priority to High (D3DKMTSetProcessSchedulingPriorityClass).
//   CPU 0      no such value in Windows: the service looks at the running processes every few seconds and takes CPU 0
//              out of a new game process's affinity.
//   P-cores    hybrid Intel CPUs: the game only on the performance cores (the highest efficiency class).
//   V-Cache    Ryzen X3D with two CCDs: the game only on the CCD with 3D V-Cache (the bigger L3).
//   EcoQoS     power throttling off for the game (SetProcessInformation, ProcessPowerThrottling), so Windows doesn't
//              slow it down or send it to the E-cores when it loses focus.
//   Lasso      where Process Lasso is installed the service doesn't keep the priorities and the affinity: the rules go
//              into its prolasso.ini instead ([ProcessDefaults] DefaultPriorities = exe,high, DefaultAffinitiesEx =
//              exe,0,1-N and DefaultGPUPriorities = exe,4); its core engine reads the file at start and when it changes.
//              EcoQoS, and an affinity that isn't one run of CPUs, stay with the service.
//
// What is on lives in HKLM\SOFTWARE\MixBooster\Games, a key per exe (lower case): Fix (FX_* bits; older versions wrote
// only On, which stands for CPU 0 + CPU and GPU High), On, Path, Custom, and what was there before MixBooster (OldIfeo, Lasso,
// LassoPrio, LassoAff, LassoGpu), so turning a fix off puts that back.

#include <tlhelp32.h>

#define GM_KEY MB_REG_KEY L"\\Games"
#define GM_IFEO_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options"
#define GM_MAX 256

typedef struct KnownGame {
    const WCHAR *name, *exe;
    const WCHAR *steam;      // under a Steam library's steamapps\common
    const WCHAR *hint, *rel; // an installed program whose name has hint: rel under its InstallLocation
} KnownGame;

static const KnownGame k_games[] = {
    { L"Counter-Strike 2", L"cs2.exe", L"Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe", NULL, NULL },
    { L"Dota 2", L"dota2.exe", L"dota 2 beta\\game\\bin\\win64\\dota2.exe", NULL, NULL },
    { L"Deadlock", L"deadlock.exe", L"Deadlock\\game\\bin\\win64\\deadlock.exe", NULL, NULL },
    { L"VALORANT", L"VALORANT-Win64-Shipping.exe", NULL, L"VALORANT", L"ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe" },
    { L"League of Legends", L"League of Legends.exe", NULL, L"League of Legends", L"Game\\League of Legends.exe" },
    { L"Fortnite", L"FortniteClient-Win64-Shipping.exe", NULL, NULL, NULL },
    { L"Apex Legends", L"r5apex_dx12.exe", L"Apex Legends\\r5apex_dx12.exe", L"Apex Legends", L"r5apex_dx12.exe" },
    { L"Apex Legends (DX11)", L"r5apex.exe", L"Apex Legends\\r5apex.exe", L"Apex Legends", L"r5apex.exe" },
    { L"PUBG: Battlegrounds", L"TslGame.exe", L"PUBG\\TslGame\\Binaries\\Win64\\TslGame.exe", NULL, NULL },
    { L"Rust", L"RustClient.exe", L"Rust\\RustClient.exe", NULL, NULL },
    { L"Marvel Rivals", L"Marvel-Win64-Shipping.exe", L"MarvelRivals\\MarvelGame\\Marvel\\Binaries\\Win64\\Marvel-Win64-Shipping.exe", NULL, NULL },
    { L"Overwatch 2", L"Overwatch.exe", L"Overwatch\\Overwatch.exe", L"Overwatch", L"_retail_\\Overwatch.exe" },
    { L"Call of Duty", L"cod.exe", L"Call of Duty HQ\\cod.exe", L"Call of Duty", L"cod.exe" },
    { L"Rainbow Six Siege", L"RainbowSix.exe", L"Tom Clancy's Rainbow Six Siege\\RainbowSix.exe", L"Rainbow Six", L"RainbowSix.exe" },
    { L"Escape from Tarkov", L"EscapeFromTarkov.exe", NULL, L"Escape from Tarkov", L"EscapeFromTarkov.exe" },
    { L"THE FINALS", L"Discovery.exe", L"THE FINALS\\Discovery.exe", NULL, NULL },
    { L"Team Fortress 2", L"tf_win64.exe", L"Team Fortress 2\\tf_win64.exe", NULL, NULL },
    { L"Rocket League", L"RocketLeague.exe", L"rocketleague\\Binaries\\Win64\\RocketLeague.exe", NULL, NULL },
    { L"Grand Theft Auto V", L"GTA5.exe", L"Grand Theft Auto V\\GTA5.exe", NULL, NULL },
    { L"Grand Theft Auto V Enhanced", L"GTA5_Enhanced.exe", L"Grand Theft Auto V Enhanced\\GTA5_Enhanced.exe", NULL, NULL },
    { L"War Thunder", L"aces.exe", L"War Thunder\\win64\\aces.exe", L"War Thunder", L"win64\\aces.exe" },
    { L"World of Tanks", L"WorldOfTanks.exe", NULL, L"World of Tanks", L"win64\\WorldOfTanks.exe" },
    { L"Мир танков", L"WorldOfTanks.exe", NULL, L"Мир танков", L"win64\\WorldOfTanks.exe" },
    { L"Battlefield 2042", L"BF2042.exe", L"Battlefield 2042\\BF2042.exe", L"Battlefield 2042", L"BF2042.exe" },
    { L"Battlefield V", L"bfv.exe", L"Battlefield V\\bfv.exe", L"Battlefield V", L"bfv.exe" },
    { L"Halo Infinite", L"HaloInfinite.exe", L"Halo Infinite\\HaloInfinite.exe", NULL, NULL },
    { L"Destiny 2", L"destiny2.exe", L"Destiny 2\\destiny2.exe", NULL, NULL },
    { L"Warframe", L"Warframe.x64.exe", L"Warframe\\Warframe.x64.exe", NULL, NULL },
    { L"Helldivers 2", L"helldivers2.exe", L"Helldivers 2\\bin\\helldivers2.exe", NULL, NULL },
    { L"Delta Force", L"DeltaForceClient-Win64-Shipping.exe", NULL, NULL, NULL },
    { L"Squad", L"SquadGame.exe", L"Squad\\Squad\\Binaries\\Win64\\SquadGame.exe", NULL, NULL },
    { L"Hell Let Loose", L"HLL-Win64-Shipping.exe", L"Hell Let Loose\\HLL\\Binaries\\Win64\\HLL-Win64-Shipping.exe", NULL, NULL },
    { L"Insurgency: Sandstorm", L"InsurgencyClient-Win64-Shipping.exe", L"sandstorm\\Insurgency\\Binaries\\Win64\\InsurgencyClient-Win64-Shipping.exe", NULL, NULL },
    { L"Arma Reforger", L"ArmaReforgerSteam.exe", L"Arma Reforger\\ArmaReforgerSteam.exe", NULL, NULL },
    { L"DayZ", L"DayZ_x64.exe", L"DayZ\\DayZ_x64.exe", NULL, NULL },
    { L"Left 4 Dead 2", L"left4dead2.exe", L"Left 4 Dead 2\\left4dead2.exe", NULL, NULL },
    { L"Dead by Daylight", L"DeadByDaylight-Win64-Shipping.exe", L"Dead by Daylight\\DeadByDaylight\\Binaries\\Win64\\DeadByDaylight-Win64-Shipping.exe", NULL, NULL },
    { L"Brawlhalla", L"Brawlhalla.exe", L"Brawlhalla\\Brawlhalla.exe", NULL, NULL },
    { L"Fall Guys", L"FallGuys_client_game.exe", NULL, NULL, NULL },
    { L"Roblox", L"RobloxPlayerBeta.exe", NULL, NULL, NULL },
    { L"Minecraft for Windows", L"Minecraft.Windows.exe", NULL, NULL, NULL },
    { L"osu!", L"osu!.exe", NULL, NULL, NULL },
    { L"World of Warcraft", L"Wow.exe", NULL, L"World of Warcraft", L"_retail_\\Wow.exe" },
    { L"Diablo IV", L"Diablo IV.exe", L"Diablo IV\\Diablo IV.exe", L"Diablo IV", L"Diablo IV.exe" },
    { L"Lost Ark", L"LOSTARK.exe", L"Lost Ark\\Binaries\\Win64\\LOSTARK.exe", NULL, NULL },
    { L"Genshin Impact", L"GenshinImpact.exe", NULL, L"Genshin Impact", L"Genshin Impact Game\\GenshinImpact.exe" },
    { L"Honkai: Star Rail", L"StarRail.exe", NULL, NULL, NULL },
    { L"Zenless Zone Zero", L"ZenlessZoneZero.exe", NULL, NULL, NULL },
    { L"Elden Ring", L"eldenring.exe", L"ELDEN RING\\Game\\eldenring.exe", NULL, NULL },
    { L"Cyberpunk 2077", L"Cyberpunk2077.exe", L"Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe", NULL, NULL },
    { L"Red Dead Redemption 2", L"RDR2.exe", L"Red Dead Redemption 2\\RDR2.exe", NULL, NULL },
    { L"Baldur's Gate 3", L"bg3_dx11.exe", L"Baldurs Gate 3\\bin\\bg3_dx11.exe", NULL, NULL },
    { L"Baldur's Gate 3 (Vulkan)", L"bg3.exe", L"Baldurs Gate 3\\bin\\bg3.exe", NULL, NULL },
    { L"Monster Hunter Wilds", L"MonsterHunterWilds.exe", L"MonsterHunterWilds\\MonsterHunterWilds.exe", NULL, NULL },
    { L"Palworld", L"Palworld-Win64-Shipping.exe", L"Palworld\\Pal\\Binaries\\Win64\\Palworld-Win64-Shipping.exe", NULL, NULL },
    { L"Valheim", L"valheim.exe", L"Valheim\\valheim.exe", NULL, NULL },
    { L"Forza Horizon 5", L"ForzaHorizon5.exe", L"ForzaHorizon5\\ForzaHorizon5.exe", NULL, NULL },
    { L"Assetto Corsa Competizione", L"AC2-Win64-Shipping.exe", L"Assetto Corsa Competizione\\AC2\\Binaries\\Win64\\AC2-Win64-Shipping.exe", NULL, NULL },
    { L"Euro Truck Simulator 2", L"eurotrucks2.exe", L"Euro Truck Simulator 2\\bin\\win_x64\\eurotrucks2.exe", NULL, NULL },
};

enum { FX_CPU0 = 1, FX_HIGH = 2, FX_PCORES = 4, FX_VCACHE = 8, FX_ECO = 16, FX_GPU = 32 };
#define FX_AFF (FX_CPU0 | FX_PCORES | FX_VCACHE)
#define FX_LEGACY (FX_CPU0 | FX_HIGH | FX_GPU)  // what On = 1 meant before the fixes were split

typedef struct Game {
    WCHAR name[96], exe[64], path[MAX_PATH];
    int fix;  // FX_* on
    bool custom, running;
} Game;

static void exe_lower(WCHAR *out, int cap, const WCHAR *exe)
{
    wcopy(out, cap, exe);
    for (WCHAR *p = out; *p; p++) *p = towlower(*p);
}

static const WCHAR *path_file(const WCHAR *p)
{
    const WCHAR *s = wcsrchr(p, L'\\'), *f = wcsrchr(p, L'/');
    if (f > s) s = f;
    return s ? s + 1 : p;
}

static bool file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// --- the registry --------------------------------------------------------------------------------------------------

static int gm_get_dw(const WCHAR *exe, const WCHAR *name, int def)
{
    WCHAR key[160];
    wfmt(key, countof(key), GM_KEY L"\\%s", exe);
    DWORD v = 0, sz = sizeof v;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? (int)v : def;
}

static bool gm_get_sz(const WCHAR *exe, const WCHAR *name, WCHAR *out, int cap)
{
    WCHAR key[160];
    wfmt(key, countof(key), GM_KEY L"\\%s", exe);
    DWORD sz = (DWORD)(cap * sizeof(WCHAR));
    out[0] = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, NULL, out, &sz) == ERROR_SUCCESS;
}

static void gm_put(const WCHAR *exe, const WCHAR *name, DWORD type, const void *data, DWORD size)
{
    WCHAR key[160];
    wfmt(key, countof(key), GM_KEY L"\\%s", exe);
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (data) RegSetValueExW(k, name, 0, type, (const BYTE *)data, size);
    else RegDeleteValueW(k, name);
    RegCloseKey(k);
}

static void gm_put_dw(const WCHAR *exe, const WCHAR *name, int v)
{
    DWORD d = (DWORD)v;
    gm_put(exe, name, REG_DWORD, &d, sizeof d);
}

static void gm_del(const WCHAR *exe, const WCHAR *name) { gm_put(exe, name, REG_NONE, NULL, 0); }

static void gm_put_sz(const WCHAR *exe, const WCHAR *name, const WCHAR *s)
{
    gm_put(exe, name, REG_SZ, s, (DWORD)((wlen(s) + 1) * sizeof(WCHAR)));
}

static int gm_fix(const WCHAR *exe) { return gm_get_dw(exe, L"Fix", gm_get_dw(exe, L"On", 0) ? FX_LEGACY : 0); }

// --- the CPU -------------------------------------------------------------------------------------------------------

// The CPUs the per-CPU fixes keep a game on, in group 0 as the rest: the P-cores of a hybrid CPU, and the CCD with
// 3D V-Cache of a Ryzen X3D with two CCDs (its L3 is the bigger one). 0 where the CPU has none.
static struct {
    bool init, battery;
    u64 all, pcores, vcache;
} GC;

static void gm_cpu(void)
{
    if (GC.init) return;
    GC.init = true;
    int n = MIN((int)GetActiveProcessorCount(0), 64);
    GC.all = n >= 64 ? ~0ull : (1ull << n) - 1;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    u8 *buf = len ? (u8 *)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        u8 lo = 255, hi = 0;
        DWORD l3_min = 0xFFFFFFFF, l3_max = 0;
        u64 l3_big = 0;
        int nl3 = 0;
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
            off += e->Size;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupMask[0].Group == 0) {
                lo = MIN(lo, e->Processor.EfficiencyClass);
                hi = MAX(hi, e->Processor.EfficiencyClass);
            } else if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0) {
                nl3++;
                l3_min = MIN(l3_min, e->Cache.CacheSize);
                if (e->Cache.CacheSize > l3_max) {
                    l3_max = e->Cache.CacheSize;
                    l3_big = e->Cache.GroupMask.Mask;
                }
            }
        }
        for (DWORD off = 0; hi > lo && off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
            off += e->Size;
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupMask[0].Group == 0 && e->Processor.EfficiencyClass == hi)
                GC.pcores |= e->Processor.GroupMask[0].Mask;
        }
        if (nl3 >= 2 && l3_max > l3_min) GC.vcache = l3_big;
        GC.pcores &= GC.all;
        GC.vcache &= GC.all;
    }
    free(buf);
    SYSTEM_POWER_STATUS ps;
    GC.battery = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
}

// The fixes this PC gets: P-cores and V-Cache only on those CPUs, EcoQoS where Windows uses it on games (hybrid
// CPUs and laptops).
static int fx_avail(void)
{
    gm_cpu();
    return FX_CPU0 | FX_HIGH | FX_GPU | (GC.pcores ? FX_PCORES : 0) | (GC.vcache ? FX_VCACHE : 0) | (GC.pcores || GC.battery ? FX_ECO : 0);
}

// The affinity a game gets out of sys, the CPUs it may run on.
static u64 fx_mask(int fix, u64 sys)
{
    gm_cpu();
    u64 m = sys;
    if ((fix & FX_PCORES) && (m & GC.pcores)) m &= GC.pcores;
    if ((fix & FX_VCACHE) && (m & GC.vcache)) m &= GC.vcache;
    if ((fix & FX_CPU0) && (m & ~1ull)) m &= ~1ull;
    return m;
}

// The affinity as Process Lasso takes it ("0,1-15"); false when there is nothing to change or it isn't one run of CPUs.
static bool lasso_aff(int fix, WCHAR *out, int cap)
{
    u64 m = fx_mask(fix, GC.all);
    if (!(fix & FX_AFF) || !m || m == GC.all) return false;
    int a = 0, b = 63;
    while (!(m >> a & 1)) a++;
    while (!(m >> b & 1)) b--;
    u64 run = (b == 63 ? ~0ull : (2ull << b) - 1) & ~((1ull << a) - 1);
    if (m != run) return false;
    wfmt(out, cap, a == b ? L"0,%d" : L"0,%d-%d", a, b);
    return true;
}

// What Process Lasso keeps of a game's fixes; the service keeps the rest.
static int lasso_part(int fix)
{
    WCHAR aff[64];
    return (fix & (FX_HIGH | FX_GPU)) | (lasso_aff(fix, aff, countof(aff)) ? fix & FX_AFF : 0);
}

// The exes with a fix on, lower case, with their fixes; svc: only the ones the service watches, with what it keeps of
// them (not what Process Lasso does). Read by the service every few seconds.
static int games_on_list(WCHAR (*out)[64], int *fix, int cap, bool svc)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GM_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    int n = 0;
    WCHAR name[64];
    for (DWORD i = 0; n < cap; i++) {
        DWORD nl = countof(name);
        if (RegEnumKeyExW(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        int f = gm_fix(name);
        if (svc && gm_get_dw(name, L"Lasso", 0)) f &= ~lasso_part(f);
        if (!f) continue;
        fix[n] = f;
        wcopy(out[n++], 64, name);
    }
    RegCloseKey(k);
    return n;
}

// Whether a game needs the service: one with a fix that Process Lasso doesn't keep.
static bool games_any(void)
{
    WCHAR one[1][64];
    int fix[1];
    return games_on_list(one, fix, 1, true) > 0;
}

// --- the priority, through Image File Execution Options ------------------------------------------------------------

// True when the value is as wanted.
static bool ifeo_set(const WCHAR *exe, bool on)
{
    WCHAR key[300], perf[320];
    wfmt(key, countof(key), GM_IFEO_KEY L"\\%s", exe);
    wfmt(perf, countof(perf), L"%s\\PerfOptions", key);
    if (on) {
        HKEY k;
        // what was there, saved once: -2 the exe had no IFEO key at all, -1 no CpuPriorityClass
        if (gm_get_dw(exe, L"OldIfeo", -3) == -3) {
            DWORD v = 0, sz = sizeof v;
            bool had_key = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) == ERROR_SUCCESS;
            if (had_key) RegCloseKey(k);
            int old = RegGetValueW(HKEY_LOCAL_MACHINE, perf, L"CpuPriorityClass", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? (int)v : had_key ? -1 : -2;
            gm_put_dw(exe, L"OldIfeo", old);
        }
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, perf, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
            log_msg("games: can't open IFEO for %ls", exe);
            return false;
        }
        DWORD v = 3;  // High
        LSTATUS st = RegSetValueExW(k, L"CpuPriorityClass", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        RegCloseKey(k);
        return st == ERROR_SUCCESS;
    }
    int old = gm_get_dw(exe, L"OldIfeo", -3);
    gm_del(exe, L"OldIfeo");
    if (old >= 0) {
        HKEY k;
        LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, perf, 0, KEY_SET_VALUE, &k);
        if (st == ERROR_SUCCESS) {
            DWORD v = (DWORD)old;
            st = RegSetValueExW(k, L"CpuPriorityClass", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
            RegCloseKey(k);
        }
        return st == ERROR_SUCCESS;
    }
    LSTATUS st = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, perf, L"CpuPriorityClass");
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) return false;
    // the keys go only when nothing else is left in them
    const WCHAR *keys[2] = { perf, key };
    for (int i = 0; i < (old == -2 ? 2 : 1); i++) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_READ, &k) != ERROR_SUCCESS) break;
        DWORD subs = 0, vals = 0;
        RegQueryInfoKeyW(k, NULL, NULL, NULL, &subs, NULL, NULL, &vals, NULL, NULL, NULL, NULL);
        RegCloseKey(k);
        if (subs || vals || RegDeleteKeyW(HKEY_LOCAL_MACHINE, keys[i]) != ERROR_SUCCESS) break;
    }
    return true;
}

// --- running processes ---------------------------------------------------------------------------------------------

// The GPU scheduling priority of a process, D3DKMT_SCHEDULINGPRIORITYCLASS: 2 normal, 4 high. Through gdi32, with the
// privilege it asks for enabled once (the service's SYSTEM token has it, the elevated app's has it off).
typedef LONG(APIENTRY *PFN_MbKmtSetSchedPrio)(HANDLE, UINT);

static bool gpu_prio_set(HANDLE proc, UINT cls)
{
    static PFN_MbKmtSetSchedPrio fn;
    static bool init;
    if (!init) {
        init = true;
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        if (!g) g = LoadLibraryW(L"gdi32.dll");
        fn = g ? (PFN_MbKmtSetSchedPrio)(void *)GetProcAddress(g, "D3DKMTSetProcessSchedulingPriorityClass") : NULL;
        HANDLE tok;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
            TOKEN_PRIVILEGES tp = { 1 };
            if (LookupPrivilegeValueW(NULL, L"SeIncreaseBasePriorityPrivilege", &tp.Privileges[0].Luid)) {
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
            }
            CloseHandle(tok);
        }
    }
    return fn && fn(proc, cls) == 0;
}

// The fixes in touch, as fix has them, for one running process: High CPU and GPU priority or Normal, the affinity,
// power throttling off or Windows' own choice.
static bool game_proc_apply(DWORD pid, int fix, int touch)
{
    HANDLE p = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    bool ok = true;
    if (touch & FX_HIGH) ok &= SetPriorityClass(p, (fix & FX_HIGH) ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS) != 0;
    if ((touch & FX_GPU) && !gpu_prio_set(p, (fix & FX_GPU) ? 4 : 2)) log_msg("games: no GPU priority for pid %lu", pid);
    DWORD_PTR pm = 0, sm = 0;
    if ((touch & FX_AFF) && GetProcessAffinityMask(p, &pm, &sm) && sm) {
        DWORD_PTR want = (DWORD_PTR)fx_mask(fix, sm);
        if (pm != want) ok &= SetProcessAffinityMask(p, want) != 0;
    }
    if (touch & FX_ECO) {
        PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, (fix & FX_ECO) ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0, 0 };
        ok &= SetProcessInformation(p, ProcessPowerThrottling, &pt, sizeof pt) != 0;
    }
    CloseHandle(p);
    return ok;
}

// Calls fn for every process whose exe name is in names (lower case).
static void procs_each(WCHAR (*names)[64], int n, void (*fn)(DWORD pid, int which, void *ctx), void *ctx)
{
    if (!n) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof pe };
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        for (int i = 0; i < n; i++)
            if (!_wcsicmp(pe.szExeFile, names[i])) fn(pe.th32ProcessID, i, ctx);
    CloseHandle(snap);
}

static bool proc_running(const WCHAR *exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof pe };
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) found = !_wcsicmp(pe.szExeFile, exe);
    CloseHandle(snap);
    return found;
}

static bool proc_path(DWORD pid, WCHAR *out)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(p, 0, out, &n) != 0;
    CloseHandle(p);
    return ok;
}

typedef struct ApplyCtx {
    int fix, touch;
} ApplyCtx;

static void apply_cb(DWORD pid, int which, void *ctx)
{
    (void)which;
    const ApplyCtx *a = (const ApplyCtx *)ctx;
    if (!game_proc_apply(pid, a->fix, a->touch)) log_msg("games: can't set pid %lu (%lu)", pid, GetLastError());
}

// --- the service's watcher -----------------------------------------------------------------------------------------

// The processes already set, so one the user changes by hand afterwards is left alone.
static DWORD g_gw_seen[256];
static int g_gw_nseen;

typedef struct GwTick {
    DWORD now[256];
    int nnow;
    const int *fix;
} GwTick;

static void watch_cb(DWORD pid, int which, void *ctx)
{
    GwTick *t = (GwTick *)ctx;
    if (t->nnow < countof(t->now)) t->now[t->nnow++] = pid;
    for (int i = 0; i < g_gw_nseen; i++)
        if (g_gw_seen[i] == pid) return;
    // once per process: one an anti-cheat guards refuses every time, its priority still comes from IFEO
    int f = t->fix[which];
    if (game_proc_apply(pid, f, f)) log_msg("service: game process %lu set (fixes %d)", pid, f);
    else log_msg("service: can't set game process %lu (%lu)", pid, GetLastError());
}

static void games_watch_tick(void)
{
    WCHAR names[GM_MAX][64];
    int fix[GM_MAX];
    int n = games_on_list(names, fix, GM_MAX, true);
    GwTick t;
    t.nnow = 0;
    t.fix = fix;
    procs_each(names, n, watch_cb, &t);
    memcpy(g_gw_seen, t.now, sizeof(DWORD) * (size_t)t.nnow);
    g_gw_nseen = t.nnow;
}

static DWORD WINAPI games_watch_thread(void *stop)
{
    while (WaitForSingleObject((HANDLE)stop, 3000) == WAIT_TIMEOUT) games_watch_tick();
    return 0;
}

// --- Process Lasso -------------------------------------------------------------------------------------------------

static bool lasso_running(void) { return proc_running(L"ProcessGovernor.exe") || proc_running(L"ProcessLasso.exe"); }

// Where its prolasso.ini is: the folder the installer recorded, the per-user and shared defaults, or next to a running
// portable copy.
static bool lasso_ini(WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; i++) {
        DWORD sz = sizeof dir;
        if (RegGetValueW(roots[i], L"SOFTWARE\\ProcessLasso", L"ConfigFolderEx", RRF_RT_REG_SZ, NULL, dir, &sz) == ERROR_SUCCESS) {
            wfmt(out, MAX_PATH, L"%s\\prolasso.ini", dir);
            if (file_exists(out)) return true;
        }
    }
    const WCHAR *envs[2] = { L"APPDATA", L"ProgramData" };
    for (int i = 0; i < 2; i++)
        if (GetEnvironmentVariableW(envs[i], dir, MAX_PATH)) {
            wfmt(out, MAX_PATH, L"%s\\ProcessLasso\\config\\prolasso.ini", dir);
            if (file_exists(out)) return true;
        }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof pe };
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"ProcessGovernor.exe") && _wcsicmp(pe.szExeFile, L"ProcessLasso.exe")) continue;
        if (!proc_path(pe.th32ProcessID, dir)) continue;
        *(WCHAR *)path_file(dir) = 0;
        static const WCHAR *rel[2] = { L"config\\prolasso.ini", L"Data\\ProcessLasso\\config\\prolasso.ini" };
        for (int i = 0; i < 2 && !found; i++) {
            wfmt(out, MAX_PATH, L"%s%s", dir, rel[i]);
            found = file_exists(out);
        }
    }
    CloseHandle(snap);
    return found;
}

// The ini as text: UTF-16 with a BOM, as Process Lasso writes it, or 8-bit.
typedef struct LassoIni {
    WCHAR *text;
    bool utf16;
} LassoIni;

static bool lasso_load(const WCHAR *path, LassoIni *ini)
{
    DWORD n = 0;
    char *raw = read_file(path, &n);
    if (!raw) return false;
    ini->utf16 = n >= 2 && (u8)raw[0] == 0xFF && (u8)raw[1] == 0xFE;
    if (ini->utf16) {
        int wn = (int)(n - 2) / 2;
        ini->text = (WCHAR *)malloc(((size_t)wn + 1) * sizeof(WCHAR));
        if (ini->text) {
            memcpy(ini->text, raw + 2, (size_t)wn * sizeof(WCHAR));
            ini->text[wn] = 0;
        }
    } else {
        int wn = MultiByteToWideChar(CP_ACP, 0, raw, (int)n, NULL, 0);
        ini->text = (WCHAR *)malloc(((size_t)wn + 1) * sizeof(WCHAR));
        if (ini->text) ini->text[MultiByteToWideChar(CP_ACP, 0, raw, (int)n, ini->text, wn)] = 0;
    }
    free(raw);
    return ini->text != NULL;
}

static bool lasso_save(const WCHAR *path, const LassoIni *ini)
{
    int wn = wlen(ini->text);
    bool ok;
    if (ini->utf16) {
        size_t sz = 2 + (size_t)wn * sizeof(WCHAR);
        char *buf = (char *)malloc(sz);
        if (!buf) return false;
        buf[0] = (char)0xFF;
        buf[1] = (char)0xFE;
        memcpy(buf + 2, ini->text, (size_t)wn * sizeof(WCHAR));
        ok = write_file_atomic(path, buf, (DWORD)sz);
        free(buf);
    } else {
        int n = WideCharToMultiByte(CP_ACP, 0, ini->text, wn, NULL, 0, NULL, NULL);
        char *buf = (char *)malloc((size_t)n + 1);
        if (!buf) return false;
        WideCharToMultiByte(CP_ACP, 0, ini->text, wn, buf, n, NULL, NULL);
        ok = write_file_atomic(path, buf, (DWORD)n);
        free(buf);
    }
    return ok;
}

// The value of key= on its own line (the first one), as [start, end) in the text; false when there is no such line.
static bool lasso_find(const WCHAR *text, const WCHAR *key, int *start, int *end)
{
    int kl = wlen(key);
    for (const WCHAR *p = text; *p;) {
        if (!_wcsnicmp(p, key, kl) && p[kl] == L'=') {
            const WCHAR *v = p + kl + 1, *e = v;
            while (*e && *e != L'\r' && *e != L'\n') e++;
            *start = (int)(v - text);
            *end = (int)(e - text);
            return true;
        }
        while (*p && *p != L'\n') p++;
        if (*p) p++;
    }
    return false;
}

// Replaces [start, end) with s; start == end == -1 adds "key=s" under the first [ProcessDefaults].
static bool lasso_splice(LassoIni *ini, int start, int end, const WCHAR *key, const WCHAR *s)
{
    WCHAR line[2200];
    if (start < 0) {
        const WCHAR *sec = wcsstr(ini->text, L"[ProcessDefaults]");
        if (!sec) return false;
        const WCHAR *e = sec;
        while (*e && *e != L'\n') e++;
        if (*e) e++;
        start = end = (int)(e - ini->text);
        wfmt(line, countof(line), L"%s=%s\r\n", key, s);
        s = line;
    }
    int tl = wlen(ini->text), sl = wlen(s);
    WCHAR *t = (WCHAR *)malloc(((size_t)tl - (size_t)(end - start) + (size_t)sl + 1) * sizeof(WCHAR));
    if (!t) return false;
    memcpy(t, ini->text, (size_t)start * sizeof(WCHAR));
    memcpy(t + start, s, (size_t)sl * sizeof(WCHAR));
    memcpy(t + start + sl, ini->text + end, ((size_t)(tl - end) + 1) * sizeof(WCHAR));
    free(ini->text);
    ini->text = t;
    return true;
}

// One rule list (match,value,... with per fields to an item): takes out exe's item, giving back what it had in old
// (the fields after the name, joined by commas), and puts add in its place when add is set.
static bool lasso_rule(LassoIni *ini, const WCHAR *key, int per, const WCHAR *exe, WCHAR *old, int old_cap, const WCHAR *add)
{
    int st = -1, en = -1;
    bool have = lasso_find(ini->text, key, &st, &en);
    int vl = have ? en - st : 0;
    // a user's lists can be long: sized to them
    size_t cap = (size_t)vl + (size_t)wlen(exe) + (size_t)wlen(add) + 8;
    WCHAR *val = (WCHAR *)malloc(((size_t)vl + 1) * sizeof(WCHAR)), *out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!val || !out) {
        free(val);
        free(out);
        return false;
    }
    if (vl) memcpy(val, ini->text + st, (size_t)vl * sizeof(WCHAR));
    val[vl] = 0;
    out[0] = 0;
    if (old) old[0] = 0;
    size_t len = 0;
    WCHAR *p = val;
    for (bool more = vl > 0; more;) {
        // the next item: per fields
        WCHAR *f[3] = { 0 };
        int nf = 0;
        while (nf < per && p) {
            f[nf++] = p;
            WCHAR *c = wcschr(p, L',');
            if (c) *c = 0;
            p = c ? c + 1 : NULL;
        }
        more = p != NULL;
        if (nf < per) break;
        if (!_wcsicmp(f[0], exe)) {
            for (int k = 1; old && k < per; k++) wfmt(old + wlen(old), old_cap - wlen(old), k > 1 ? L",%s" : L"%s", f[k]);
            continue;
        }
        for (int k = 0; k < per; k++) {
            if (len) out[len++] = L',';
            int fl = wlen(f[k]);
            memcpy(out + len, f[k], (size_t)fl * sizeof(WCHAR));
            len += (size_t)fl;
        }
    }
    if (add && add[0]) {
        WCHAR lo[64];
        exe_lower(lo, countof(lo), exe);
        len += (size_t)MAX(0, wfmt(out + len, (int)(cap - len), len ? L",%s,%s" : L"%s,%s", lo, add));
    }
    out[len] = 0;
    bool ok = (!have && !len) || lasso_splice(ini, have ? st : -1, have ? en : -1, key, out);
    free(val);
    free(out);
    return ok;
}

// Writes, changes or takes out one game's rules for the fixes Lasso keeps (lasso_part); true when written. A rule the
// user had for the exe is kept from the first change and put back where no fix needs that key any more.
static bool lasso_set(const WCHAR *exe, int fix)
{
    WCHAR path[MAX_PATH];
    if (!lasso_ini(path)) return false;
    LassoIni ini;
    if (!lasso_load(path, &ini)) return false;
    static const WCHAR *keys[3] = { L"DefaultPriorities", L"DefaultAffinitiesEx", L"DefaultGPUPriorities" };
    static const WCHAR *saved[3] = { L"LassoPrio", L"LassoAff", L"LassoGpu" };
    static const int per[3] = { 2, 3, 2 };
    bool had = gm_get_dw(exe, L"Lasso", 0) != 0;
    WCHAR aff[64], old[3][128], keep[3][128];
    // GPU priority 4 is High, as Lasso's menu writes it
    const WCHAR *want[3] = { fix & FX_HIGH ? L"high" : NULL, lasso_aff(fix, aff, countof(aff)) ? aff : NULL, fix & FX_GPU ? L"4" : NULL };
    bool on = want[0] || want[1] || want[2], ok = true;
    for (int k = 0; k < 3 && ok; k++) {
        keep[k][0] = 0;
        if (had) gm_get_sz(exe, saved[k], keep[k], countof(keep[k]));
        ok = lasso_rule(&ini, keys[k], per[k], exe, old[k], countof(old[k]), on ? want[k] : NULL);
        // not ours to set: what the user had, from the first change or what was there just now
        const WCHAR *back = had ? keep[k] : old[k];
        if (ok && !(on && want[k]) && back[0]) ok = lasso_rule(&ini, keys[k], per[k], exe, NULL, 0, back);
    }
    ok = ok && lasso_save(path, &ini);
    free(ini.text);
    if (ok && on && !had) {
        for (int k = 0; k < 3; k++) gm_put_sz(exe, saved[k], old[k]);
        gm_put_dw(exe, L"Lasso", 1);
    }
    if (ok && !on) {
        for (int k = 0; k < 3; k++) gm_del(exe, saved[k]);
        gm_del(exe, L"Lasso");
    }
    log_msg("games: Process Lasso rules for %ls %s%s", exe, on ? "set" : "removed", ok ? "" : " - failed");
    return ok;
}

// Whether the game is in one of prolasso.ini's rule lists now (the Lasso GUI can save over it).
static bool lasso_has(const WCHAR *exe, const WCHAR *key)
{
    WCHAR path[MAX_PATH];
    LassoIni ini;
    if (!lasso_ini(path) || !lasso_load(path, &ini)) return false;
    int st, en;
    bool has = false;
    if (lasso_find(ini.text, key, &st, &en)) {
        ini.text[en] = 0;
        int el = wlen(exe);
        for (WCHAR *p = ini.text + st; *p && !has; p = wcschr(p, L',') ? wcschr(p, L',') + 1 : p + wlen(p))
            has = !_wcsnicmp(p, exe, el) && (p[el] == L',' || !p[el]);
    }
    free(ini.text);
    return has;
}

// --- turning fixes on and off, from the app ------------------------------------------------------------------------

// Sets a game's fixes: the priority in IFEO, the rules in prolasso.ini where Lasso keeps the game, and what changed on
// its running processes. True when all of it went in.
static bool game_set(const Game *g, int fix)
{
    WCHAR exe[64];
    exe_lower(exe, countof(exe), g->exe);
    int was = gm_fix(exe);
    gm_put_sz(exe, L"Name", g->name);
    gm_put_sz(exe, L"Path", g->path);
    if (g->custom) gm_put_dw(exe, L"Custom", 1);
    gm_put_dw(exe, L"Fix", fix);
    gm_put_dw(exe, L"On", fix ? 1 : 0);
    bool ok = true;
    if ((was ^ fix) & FX_HIGH) ok &= ifeo_set(exe, (fix & FX_HIGH) != 0);
    // with Process Lasso installed it keeps the priority and the affinity, not the service
    WCHAR ini[MAX_PATH];
    if (gm_get_dw(exe, L"Lasso", 0) || (lasso_part(fix) && lasso_ini(ini))) ok &= lasso_set(exe, fix);
    WCHAR names[1][64];
    wcopy(names[0], 64, exe);
    ApplyCtx a = { fix, was ^ fix };
    procs_each(names, 1, apply_cb, &a);
    // a known game with nothing on leaves nothing behind
    if (!fix && !g->custom) {
        WCHAR key[160];
        wfmt(key, countof(key), GM_KEY L"\\%s", exe);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    }
    log_msg("games: %ls fixes %d -> %d%s", exe, was, fix, ok ? "" : " - failed");
    return ok;
}

static bool game_forget(const Game *g)
{
    bool ok = !g->fix || game_set(g, 0);
    WCHAR exe[64], key[160];
    exe_lower(exe, countof(exe), g->exe);
    wfmt(key, countof(key), GM_KEY L"\\%s", exe);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    return ok;
}

// Process Lasso put in after the fixes were turned on, or its GUI saved over the rules: write them again. Lasso
// removed: the service takes its games back. True when a game changed hands between the two.
static bool lasso_sync(void)
{
    WCHAR names[GM_MAX][64], ini[MAX_PATH];
    int fix[GM_MAX];
    int n = games_on_list(names, fix, GM_MAX, false);
    bool have = lasso_ini(ini), moved = false;
    for (int i = 0; i < n; i++) {
        bool was = gm_get_dw(names[i], L"Lasso", 0) != 0;
        int lp = lasso_part(fix[i]);
        if (have && lp && !lasso_has(names[i], (lp & FX_AFF) ? L"DefaultAffinitiesEx" : (lp & FX_HIGH) ? L"DefaultPriorities" : L"DefaultGPUPriorities")) lasso_set(names[i], fix[i]);
        else if (!have && was) gm_del(names[i], L"Lasso");
        moved |= was != (gm_get_dw(names[i], L"Lasso", 0) != 0);
    }
    return moved;
}

// --- finding the games ---------------------------------------------------------------------------------------------

static int games_add(Game *list, int n, const WCHAR *name, const WCHAR *exe, const WCHAR *path, bool custom)
{
    for (int i = 0; i < n; i++)
        if (!_wcsicmp(list[i].exe, exe)) {
            if (!list[i].path[0] && path) wcopy(list[i].path, MAX_PATH, path);
            return n;
        }
    if (n >= GM_MAX) return n;
    Game *g = &list[n];
    memset(g, 0, sizeof *g);
    wcopy(g->name, countof(g->name), name);
    wcopy(g->exe, countof(g->exe), exe);
    if (path) wcopy(g->path, MAX_PATH, path);
    // Riot writes its folders with forward slashes
    for (WCHAR *p = g->path; *p; p++)
        if (*p == L'/') *p = L'\\';
    g->custom = custom;
    return n + 1;
}

// Steam's libraries: its own folder and every "path" in steamapps\libraryfolders.vdf.
static int steam_libs(WCHAR (*out)[MAX_PATH], int cap)
{
    WCHAR root[MAX_PATH];
    DWORD sz = sizeof root;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, root, &sz) != ERROR_SUCCESS) {
        sz = sizeof root;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, NULL, root, &sz) != ERROR_SUCCESS) return 0;
    }
    for (WCHAR *p = root; *p; p++)
        if (*p == L'/') *p = L'\\';
    int n = 0;
    wcopy(out[n++], MAX_PATH, root);
    WCHAR vdf[MAX_PATH];
    wfmt(vdf, MAX_PATH, L"%s\\steamapps\\libraryfolders.vdf", root);
    char *text = read_file(vdf, NULL);
    if (!text) return n;
    char *cur = text, *line;
    while ((line = next_line(&cur)) && n < cap) {
        char *k = strstr(line, "\"path\"");
        if (!k) continue;
        char *v = strchr(k + 6, '"');
        if (!v) continue;
        char path[MAX_PATH * 2];
        int len = 0;
        for (char *p = v + 1; *p && *p != '"' && len < (int)sizeof path - 1; p++) {
            if (*p == '\\' && p[1] == '\\') p++;
            path[len++] = *p;
        }
        path[len] = 0;
        WCHAR w[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH)) continue;
        // the registry has Steam's own folder in lower case, the list in its real case
        int dup = -1;
        for (int i = 0; i < n; i++)
            if (!_wcsicmp(out[i], w)) dup = i;
        wcopy(out[dup >= 0 ? dup : n++], MAX_PATH, w);
    }
    free(text);
    return n;
}

// A string field of the Epic manifest's JSON, unescaped, with / turned into \.
static bool json_str(const char *json, const char *key, WCHAR *out, int cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return false;
    const char *v = strchr(k + strlen(pat), '"');
    if (!v) return false;
    char s[MAX_PATH * 2];
    int len = 0;
    for (const char *p = v + 1; *p && *p != '"' && len < (int)sizeof s - 1; p++) {
        if (*p == '\\' && p[1]) p++;
        s[len++] = *p == '/' ? '\\' : *p;
    }
    s[len] = 0;
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

static int scan_epic(Game *list, int n)
{
    WCHAR pd[MAX_PATH], pat[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) return n;
    wfmt(pat, MAX_PATH, L"%s\\Epic\\EpicGamesLauncher\\Data\\Manifests\\*.item", pd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return n;
    do {
        WCHAR file[MAX_PATH], loc[MAX_PATH], launch[MAX_PATH], full[MAX_PATH];
        wfmt(file, MAX_PATH, L"%s\\Epic\\EpicGamesLauncher\\Data\\Manifests\\%s", pd, fd.cFileName);
        char *json = read_file(file, NULL);
        if (!json) continue;
        if (json_str(json, "InstallLocation", loc, MAX_PATH) && json_str(json, "LaunchExecutable", launch, MAX_PATH)) {
            wfmt(full, MAX_PATH, L"%s\\%s", loc, launch);
            for (int i = 0; i < countof(k_games); i++)
                if (!_wcsicmp(path_file(launch), k_games[i].exe) && file_exists(full)) n = games_add(list, n, k_games[i].name, k_games[i].exe, full, false);
        }
        free(json);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Riot, Battle.net, BSG and the like: the installed programs' InstallLocation.
static int scan_uninstall(Game *list, int n)
{
    static const struct { HKEY root; REGSAM view; } src[3] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY }, { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY }, { HKEY_CURRENT_USER, 0 } };
    for (int s = 0; s < 3; s++) {
        HKEY k;
        if (RegOpenKeyExW(src[s].root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | src[s].view, &k) != ERROR_SUCCESS) continue;
        WCHAR sub[256];
        for (DWORD i = 0;; i++) {
            DWORD sl = countof(sub);
            if (RegEnumKeyExW(k, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            WCHAR dn[256], loc[MAX_PATH], full[MAX_PATH];
            DWORD sz = sizeof dn;
            if (RegGetValueW(k, sub, L"DisplayName", RRF_RT_REG_SZ, NULL, dn, &sz) != ERROR_SUCCESS) continue;
            sz = sizeof loc;
            if (RegGetValueW(k, sub, L"InstallLocation", RRF_RT_REG_SZ, NULL, loc, &sz) != ERROR_SUCCESS || !loc[0]) continue;
            int ll = wlen(loc);
            while (ll > 0 && (loc[ll - 1] == L'\\' || loc[ll - 1] == L'"')) loc[--ll] = 0;
            for (int g = 0; g < countof(k_games); g++) {
                if (!k_games[g].hint || !wcsstr(dn, k_games[g].hint)) continue;
                wfmt(full, MAX_PATH, L"%s\\%s", loc[0] == L'"' ? loc + 1 : loc, k_games[g].rel);
                if (file_exists(full)) n = games_add(list, n, k_games[g].name, k_games[g].exe, full, false);
            }
        }
        RegCloseKey(k);
    }
    return n;
}

static void running_cb(DWORD pid, int which, void *ctx)
{
    Game *g = (Game *)ctx + which;
    g->running = true;
    if (!g->path[0]) proc_path(pid, g->path);
}

// Every game on this PC: what MixBooster keeps (on, or added by hand), then the known games found in Steam, Epic and
// the installed programs, and the known ones running now.
static int games_scan(Game *list)
{
    int n = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GM_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        WCHAR exe[64];
        for (DWORD i = 0; n < GM_MAX; i++) {
            DWORD el = countof(exe);
            if (RegEnumKeyExW(k, i, exe, &el, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            WCHAR name[96], path[MAX_PATH];
            if (!gm_get_sz(exe, L"Name", name, countof(name))) wcopy(name, countof(name), exe);
            gm_get_sz(exe, L"Path", path, countof(path));
            int at = n;
            n = games_add(list, n, name, exe, path, gm_get_dw(exe, L"Custom", 0) != 0);
            if (n > at) list[at].fix = gm_fix(exe);
        }
        RegCloseKey(k);
    }
    // the stored ones show the known game's own spelling of the exe
    for (int i = 0; i < n; i++)
        for (int g = 0; g < countof(k_games); g++)
            if (!_wcsicmp(list[i].exe, k_games[g].exe)) wcopy(list[i].exe, 64, k_games[g].exe);

    WCHAR libs[16][MAX_PATH], full[MAX_PATH];
    int nl = steam_libs(libs, countof(libs));
    for (int l = 0; l < nl; l++)
        for (int g = 0; g < countof(k_games); g++) {
            if (!k_games[g].steam) continue;
            wfmt(full, MAX_PATH, L"%s\\steamapps\\common\\%s", libs[l], k_games[g].steam);
            if (file_exists(full)) n = games_add(list, n, k_games[g].name, k_games[g].exe, full, false);
        }
    n = scan_epic(list, n);
    n = scan_uninstall(list, n);

    // the known ones running now, even where none of the above found them
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof pe };
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
            for (int g = 0; g < countof(k_games); g++)
                if (!_wcsicmp(pe.szExeFile, k_games[g].exe)) {
                    WCHAR p[MAX_PATH] = L"";
                    proc_path(pe.th32ProcessID, p);
                    n = games_add(list, n, k_games[g].name, k_games[g].exe, p, false);
                }
        CloseHandle(snap);
    }
    WCHAR all[GM_MAX][64];
    for (int i = 0; i < n; i++) wcopy(all[i], 64, list[i].exe);
    procs_each(all, n, running_cb, list);
    return n;
}
