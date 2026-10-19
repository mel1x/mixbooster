// "Core parking", what ParkControl does: the power plans, each plan's core parking and processor state, and the cores'
// live state. And the MixBoost playbook's own plan, made here on a PC that doesn't have it.
//
// The values live in the power plans themselves (powrprof), under the processor subgroup:
//   CPMINCORES        min cores kept unparked, % (class 0: every core, or the E-cores on a hybrid CPU)
//   CPMINCORES1       the same for class 1, the P-cores on a hybrid CPU
//   PROCTHROTTLEMIN   minimum processor state, %
//   PROCTHROTTLEMAX   maximum processor state, % (under 100 turns turbo off)
// AC and DC each. 100% min cores is parking off. A change to the active plan applies at once.

#include <powrprof.h>
#include <pdh.h>
#ifdef _MSC_VER
#pragma comment(lib, "powrprof")
#pragma comment(lib, "pdh")
#endif

static const GUID k_sub_processor = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID k_cpmincores = { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83 } };
static const GUID k_cpmincores1 = { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84 } };
static const GUID k_throttle_min = { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
static const GUID k_throttle_max = { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
static const GUID k_plan_ultimate = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
static const GUID k_plan_high = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID k_plan_balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
// The playbook's plan keeps this GUID, so its "Enable Power-saving" still finds it
static const GUID k_plan_mixboost = { 0x11111111, 0x1111, 0x1111, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } };

enum { PS_PARK, PS_PARK1, PS_MIN, PS_MAX, PS__COUNT };
static const GUID *const k_ps_guid[PS__COUNT] = { &k_cpmincores, &k_cpmincores1, &k_throttle_min, &k_throttle_max };

#define PK_MAX_PLANS 16
#define PK_PARK_KEY MB_REG_KEY L"\\Park"

typedef struct PkPlan {
    GUID id;
    WCHAR name[96];
    bool mixboost;            // the playbook's MixBoost Low Latency (HP)
} PkPlan;

static int pk_plans(PkPlan *out, int cap, int *active)
{
    GUID *act = NULL;
    *active = -1;
    PowerGetActiveScheme(NULL, &act);
    int n = 0;
    for (ULONG i = 0; n < cap; i++) {
        GUID g;
        DWORD sz = sizeof g;
        if (PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, i, (UCHAR *)&g, &sz) != ERROR_SUCCESS) break;
        PkPlan *p = &out[n];
        memset(p, 0, sizeof *p);
        p->id = g;
        DWORD nsz = sizeof p->name - sizeof(WCHAR);
        if (PowerReadFriendlyName(NULL, &g, NULL, NULL, (UCHAR *)p->name, &nsz) != ERROR_SUCCESS || !p->name[0]) wcopy(p->name, countof(p->name), TR("Без имени", "Unnamed"));
        p->mixboost = IsEqualGUID(&g, &k_plan_mixboost);
        if (act && IsEqualGUID(&g, act)) *active = n;
        n++;
    }
    if (act) LocalFree(act);
    return n;
}

static DWORD pk_read(const GUID *plan, int setting, bool dc)
{
    DWORD v = 0;
    if (dc) PowerReadDCValueIndex(NULL, plan, &k_sub_processor, k_ps_guid[setting], &v);
    else PowerReadACValueIndex(NULL, plan, &k_sub_processor, k_ps_guid[setting], &v);
    return v;
}

// Writes one value and, when the plan is the active one, applies it.
static void pk_write(const GUID *plan, int setting, bool dc, DWORD v)
{
    DWORD r = dc ? PowerWriteDCValueIndex(NULL, plan, &k_sub_processor, k_ps_guid[setting], v)
                 : PowerWriteACValueIndex(NULL, plan, &k_sub_processor, k_ps_guid[setting], v);
    if (r != ERROR_SUCCESS) log_msg("park: write %d = %lu failed: %lu", setting, v, r);
    GUID *act = NULL;
    if (PowerGetActiveScheme(NULL, &act) == ERROR_SUCCESS) {
        if (IsEqualGUID(act, plan)) PowerSetActiveScheme(NULL, plan);
        LocalFree(act);
    }
}

// The min cores before parking was turned off, to turn it back on with: per plan, setting and AC/DC.
static void pk_saved_name(const GUID *plan, int setting, bool dc, WCHAR *out, int cap)
{
    wfmt(out, cap, L"%08lX-%d-%s", plan->Data1, setting, dc ? L"dc" : L"ac");
}

static void pk_set_parking(const GUID *plan, bool dc, bool on)
{
    for (int s = PS_PARK; s <= PS_PARK1; s++) {
        WCHAR name[64];
        pk_saved_name(plan, s, dc, name, countof(name));
        DWORD cur = pk_read(plan, s, dc);
        if (!on) {
            if (cur < 100) {
                HKEY k;
                if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PK_PARK_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
                    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&cur, sizeof cur);
                    RegCloseKey(k);
                }
            }
            pk_write(plan, s, dc, 100);
        } else if (cur >= 100) {
            DWORD v = 10, sz = sizeof v;
            // Balanced's own value on most PCs is 10%; a saved one wins
            if (RegGetValueW(HKEY_LOCAL_MACHINE, PK_PARK_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS || v >= 100) v = 10;
            pk_write(plan, s, dc, v);
        }
    }
}

// --- the MixBoost plan --------------------------------------------------------------------------------------------
// What the playbook's setPowerPlan.ps1 makes: Ultimate Performance (else High performance, else Balanced) copied
// under the plan's own GUID, then Hybred's Low Latency (HP) values one by one. A value this Windows build doesn't have
// is skipped, as the script does.

typedef struct PkVal {
    const char *sub, *set;
    DWORD ac;
    int dc;                   // -1: only AC, as the script
} PkVal;

static const PkVal k_mixboost[] = {
    { "0012ee47-9041-4b5d-9b77-535fba8b1442", "51dea550-bb38-4bc4-991b-eacf37be5ec8", 100, -1 },
    { "0012ee47-9041-4b5d-9b77-535fba8b1442", "6738e2c4-e8a5-4a42-b16a-e040e769756e", 0, -1 },
    { "0012ee47-9041-4b5d-9b77-535fba8b1442", "d3d55efd-c1ff-424e-9dc3-441be7833010", 60000, -1 },
    { "0012ee47-9041-4b5d-9b77-535fba8b1442", "d639518a-e56d-4345-8af2-b9f32fb26109", 60000, -1 },
    { "0012ee47-9041-4b5d-9b77-535fba8b1442", "dab60367-53fe-4fbc-825e-521d069d2456", 300000, -1 },
    { "0d7dbae2-4294-402a-ba8e-26777e8488cd", "309dce9b-bef4-4119-9921-a851fb12f0f4", 1, -1 },
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "25dfa149-5dd1-4736-b5ab-e8a37b5b8187", 0, -1 },
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "7bc4a2f9-d8fc-4469-b07b-33eb785aaca0", 4294967280u, -1 },
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "94ac6d29-73ce-41a6-809f-6363ba21b47e", 0, -1 },
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "a4b195f5-8225-47d8-8012-9d41369786e2", 1, -1 },
    // sleep allowed: off in the original plan, which made sleeping the PC impossible
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "abfc2519-3608-4c2a-94ea-171b0ed546ab", 1, 1 },
    { "238c9fa8-0aad-41ed-83f4-97be242c8f20", "bd3b718a-0680-4d9d-8ab2-e1d2b4ac806d", 0, -1 },
    { "fea3413e-7e05-4911-9a71-700331f1c294", "245d8541-3943-4422-b025-13a784f679b7", 1, 1 },
    { "2a737441-1930-4402-8d77-b2bebba308a3", "0853a681-27c8-4100-a2fd-82013e970683", 100000, -1 },
    { "2a737441-1930-4402-8d77-b2bebba308a3", "48e6b7a6-50f5-4782-a5d4-53bb8f07e226", 0, -1 },
    { "2a737441-1930-4402-8d77-b2bebba308a3", "498c044a-201b-4631-a522-5c744ed4e678", 1, -1 },
    { "2a737441-1930-4402-8d77-b2bebba308a3", "d4e98f31-5ffe-4ce1-be31-1b38b384c009", 0, -1 },
    { "2e601130-5351-4d9d-8e04-252966bad054", "d502f7ee-1dc7-4efd-a55d-f04b6f5c0545", 0, -1 },
    { "48672f38-7a9a-4bb2-8bf8-3d85be19de4e", "2bfc24f9-5ea2-4801-8213-3dbae01aa39d", 3, -1 },
    { "48672f38-7a9a-4bb2-8bf8-3d85be19de4e", "d6ba4903-386f-4c2c-8adb-5c21b3328d25", 0, -1 },
    { "4f971e89-eebd-4455-a8de-9e59040e7347", "7648efa3-dd9c-4e3e-b566-50f929386280", 3, -1 },
    { "4f971e89-eebd-4455-a8de-9e59040e7347", "96996bc0-ad50-47ec-923b-6f41874dd9eb", 0, -1 },
    { "501a4d13-42af-4429-9fd1-a8218c268e20", "ee12f906-d277-404b-b6da-e5fa1a576df5", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "06cadf0e-64ed-448a-8927-ce7bf90eb35d", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "0cc5b647-c1df-4637-891a-dec35c318583", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "1299023c-bc28-4f0a-81ec-d3295a8d815d", 1, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "12a0ab44-fe28-4fa9-b3bd-4b64f44960a6", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "2ddd5a84-5a71-437e-912a-db0b8c788732", 1, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "3b04d4fd-1cc7-4f23-ab1c-d1337819c4bb", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "4b92d758-5a24-4851-a470-815d78aee119", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "4d2b0152-7d5c-498b-88e2-34345392a2c5", 5000, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "5b33697b-e89d-4d38-aa46-9e7dfb7cd2f9", 1000, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "5d76a2ca-e8c0-402f-a133-2158492d58ad", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "68dd2f27-a4ce-4e11-8487-3794e4135dfa", 90, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "71021b41-c749-4d21-be74-a00f335d582b", 1, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "7b224883-b3cc-4d79-819f-8374152cbe7c", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "7d24baa7-0b84-480f-840c-1b0743c00f5f", 128, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "7d24baa7-0b84-480f-840c-1b0743c00f60", 128, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "7f2492b6-60b1-45e5-ae55-773f8cd5caec", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "8809c2d8-b155-42d4-bcda-0d345651b1db", 1000, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "8f7b45e3-c393-480a-878c-f67ac3d07082", 1, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "943c8cb6-6f93-4227-ad87-e9a3feec08d1", 5, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "9ac18e92-aa3c-4e27-b307-01ae37307129", 1, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "c4581c31-89ab-4597-8e2b-9c9cab440e6b", 200000, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "c7be0679-2817-4d69-9d02-519a537ed0c6", 2, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "cfeda3d0-7697-4566-a922-a9086cd49dfa", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "d8edeb9b-95cf-4f95-a73c-b061973693c8", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "df142941-20f3-4edf-9a4a-9c83d3d717d1", 5, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "dfd10d17-d5eb-45dd-877a-9a34ddd15c82", 100, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "e70867f1-fa2f-4f4e-aea1-4d8a0ba23b20", 0, -1 },
    { "54533251-82be-4824-96c1-47b60b740d00", "f735a673-2066-4f80-a0c5-ddee0cf1bf5d", 0, -1 },
    { "7516b95f-f776-4464-8c53-06167f40cc99", "3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e", 0, -1 },
    { "7516b95f-f776-4464-8c53-06167f40cc99", "aded5e82-b909-4619-9949-f5d71dac0bcb", 100, -1 },
};

static bool guid_parse(const char *s, GUID *g)
{
    WCHAR w[48];
    wfmt(w, countof(w), L"{%S}", s);
    return SUCCEEDED(CLSIDFromString(w, g));
}

static bool pk_has_plan(const GUID *g)
{
    for (ULONG i = 0;; i++) {
        GUID e;
        DWORD sz = sizeof e;
        if (PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, i, (UCHAR *)&e, &sz) != ERROR_SUCCESS) return false;
        if (IsEqualGUID(&e, g)) return true;
    }
}

// Makes the plan and turns it on.
static bool pk_add_mixboost(void)
{
    static const GUID *const bases[] = { &k_plan_ultimate, &k_plan_high, &k_plan_balanced };
    GUID dest = k_plan_mixboost, *pd = &dest;
    DWORD r = ERROR_INVALID_PARAMETER;
    for (int i = 0; i < countof(bases) && r != ERROR_SUCCESS; i++) r = PowerDuplicateScheme(NULL, bases[i], &pd);
    if (r != ERROR_SUCCESS) {
        log_msg("park: can't make the MixBoost plan: %lu", r);
        return false;
    }
    static const WCHAR name[] = L"MixBoost Low Latency (HP)",
                       desc[] = L"Based on Hybred's power plan, for the lowest latency and highest performance.";
    PowerWriteFriendlyName(NULL, &k_plan_mixboost, NULL, NULL, (UCHAR *)name, sizeof name);
    PowerWriteDescription(NULL, &k_plan_mixboost, NULL, NULL, (UCHAR *)desc, sizeof desc);
    int skipped = 0;
    for (int i = 0; i < countof(k_mixboost); i++) {
        const PkVal *v = &k_mixboost[i];
        GUID sub, set;
        if (!guid_parse(v->sub, &sub) || !guid_parse(v->set, &set)) continue;
        // fea3413e-... is NO_SUBGROUP_GUID, the settings outside any subgroup
        const GUID *ps = &sub;
        if (PowerWriteACValueIndex(NULL, &k_plan_mixboost, ps, &set, v->ac) != ERROR_SUCCESS) skipped++;
        if (v->dc >= 0) PowerWriteDCValueIndex(NULL, &k_plan_mixboost, ps, &set, (DWORD)v->dc);
    }
    PowerSetActiveScheme(NULL, &k_plan_mixboost);
    log_msg("park: made the MixBoost plan, %d of %d values (%d don't exist here)", countof(k_mixboost) - skipped, countof(k_mixboost), skipped);
    return true;
}

static bool pk_has_battery(void)
{
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255;
}

// --- the cores now, through performance counters -------------------------------------------------------------------

static struct {
    PDH_HQUERY q;
    PDH_HCOUNTER park, util, freq, perf;
    bool ok, primed;
    int n;
    bool parked[MAX_CPUS];
    f32 load[MAX_CPUS], mhz[MAX_CPUS];
} PM;

static void pm_open(void)
{
    if (PM.q) return;
    if (PdhOpenQueryW(NULL, 0, &PM.q) != ERROR_SUCCESS) {
        PM.q = NULL;
        return;
    }
    // English names: the counters are localized
    bool ok = PdhAddEnglishCounterW(PM.q, L"\\Processor Information(*)\\Parking Status", 0, &PM.park) == ERROR_SUCCESS;
    ok &= PdhAddEnglishCounterW(PM.q, L"\\Processor Information(*)\\% Processor Utility", 0, &PM.util) == ERROR_SUCCESS;
    PdhAddEnglishCounterW(PM.q, L"\\Processor Information(*)\\Processor Frequency", 0, &PM.freq);
    PdhAddEnglishCounterW(PM.q, L"\\Processor Information(*)\\% Processor Performance", 0, &PM.perf);
    PM.ok = ok;
    PdhCollectQueryData(PM.q);
}

// Fills out[cpu] from a counter, for the instances "0,n" (group 0); returns the highest CPU + 1.
static int pm_array(PDH_HCOUNTER c, f32 *out)
{
    if (!c) return 0;
    static u8 buf[64 * 1024];
    DWORD size = sizeof buf, count = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, (PDH_FMT_COUNTERVALUE_ITEM_W *)buf) != ERROR_SUCCESS) return 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf;
    int n = 0;
    for (DWORD i = 0; i < count; i++) {
        const WCHAR *s = it[i].szName;
        if (!s || s[0] != L'0' || s[1] != L',' || !iswdigit(s[2])) continue;
        int cpu = _wtoi(s + 2);
        if (cpu < 0 || cpu >= MAX_CPUS) continue;
        out[cpu] = (f32)it[i].FmtValue.doubleValue;
        n = MAX(n, cpu + 1);
    }
    return n;
}

static void pm_sample(void)
{
    pm_open();
    if (!PM.ok || PdhCollectQueryData(PM.q) != ERROR_SUCCESS) return;
    f32 park[MAX_CPUS] = { 0 }, freq[MAX_CPUS] = { 0 }, perf[MAX_CPUS] = { 0 };
    int n = pm_array(PM.park, park);
    pm_array(PM.util, PM.load);
    int nf = pm_array(PM.freq, freq), np = pm_array(PM.perf, perf);
    for (int i = 0; i < n; i++) {
        PM.parked[i] = park[i] > 0.5f;
        PM.load[i] = CLAMP(PM.load[i], 0.f, 100.f);
        PM.mhz[i] = i < nf ? freq[i] * (i < np && perf[i] > 0 ? perf[i] / 100.f : 1.f) : 0;
    }
    // the rates need two samples: the first one only primes them
    PM.n = PM.primed ? n : 0;
    PM.primed = true;
}

static void pm_close(void)
{
    if (PM.q) PdhCloseQuery(PM.q);
    memset(&PM, 0, sizeof PM);
}
