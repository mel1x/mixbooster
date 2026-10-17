// The PC score, 0..100: how strong the PC is for games, 100 being a top gaming PC of today (RTX 5090, a Ryzen 9 / Core
// Ultra 9, 64 GB of DDR5, an NVMe system drive). A rough estimate from names and sizes, not a benchmark:
//   GPU 40%     a table of models, relative to the RTX 5090; laptop parts a quarter lower; unknown ones by VRAM
//   CPU 30%     threads, scaled by the generation read from the model name
//   memory 15%  size, then type and speed
//   drive 15%   what Windows lives on: NVMe, SATA SSD or HDD

enum { SC_GPU, SC_CPU, SC_RAM, SC_DISK, SC__COUNT };

static struct {
    // filled while the parts are read
    WCHAR cpu_name[96], gpu_name[128];
    int threads;
    u64 vram, ram;
    int ram_type, ram_speed;      // SMBIOS memory type, MT/s
    int sys_disk;                 // 0 unknown, 1 HDD, 2 SATA SSD, 3 NVMe
    bool gpu_integrated;
    // the result
    int part[SC__COUNT], total;
} SCORE;

static bool has_i(const WCHAR *s, const WCHAR *sub)
{
    int n = wlen(sub);
    for (; *s; s++)
        if (!_wcsnicmp(s, sub, n)) return true;
    return false;
}

static int score_gpu(void)
{
    // Most specific first: "4070 Ti SUPER" before "4070 Ti" before "4070".
    static const struct { const WCHAR *name; int score; } k[] = {
        { L"5090", 100 }, { L"4090", 92 }, { L"5080", 80 }, { L"4080 SUPER", 74 }, { L"4080", 72 }, { L"5070 Ti", 70 },
        { L"4070 Ti SUPER", 66 }, { L"4070 Ti", 62 }, { L"3090 Ti", 60 }, { L"5070", 60 }, { L"4070 SUPER", 58 }, { L"3090", 56 },
        { L"3080 Ti", 54 }, { L"4070", 52 }, { L"3080", 50 }, { L"5060 Ti", 48 }, { L"3070 Ti", 44 }, { L"4060 Ti", 40 }, { L"5060", 40 },
        { L"3070", 40 }, { L"2080 Ti", 38 }, { L"3060 Ti", 36 }, { L"5050", 32 }, { L"4060", 33 }, { L"2080 SUPER", 34 }, { L"2080", 32 },
        { L"2070 SUPER", 30 }, { L"3060", 28 }, { L"2070", 28 }, { L"1080 Ti", 28 }, { L"2060 SUPER", 26 }, { L"2060", 22 }, { L"1080", 22 },
        { L"1660 Ti", 18 }, { L"1660 SUPER", 18 }, { L"3050", 18 }, { L"1070 Ti", 20 }, { L"1070", 18 }, { L"1660", 16 }, { L"1060", 12 },
        { L"1650", 11 }, { L"1050 Ti", 8 }, { L"1050", 6 }, { L"GT 1030", 3 },
        { L"7900 XTX", 80 }, { L"9070 XT", 72 }, { L"7900 XT", 70 }, { L"9070", 66 }, { L"7900 GRE", 60 }, { L"6950 XT", 56 }, { L"6900 XT", 52 },
        { L"7800 XT", 52 }, { L"6800 XT", 50 }, { L"9060 XT", 44 }, { L"7700 XT", 44 }, { L"6800", 42 }, { L"6750 XT", 38 }, { L"6700 XT", 36 },
        { L"7600 XT", 32 }, { L"7600", 30 }, { L"6650 XT", 30 }, { L"6600 XT", 28 }, { L"6600", 26 }, { L"5700 XT", 24 }, { L"5700", 21 },
        { L"5600 XT", 19 }, { L"6500 XT", 10 }, { L"5500 XT", 12 }, { L"590", 11 }, { L"580", 10 }, { L"570", 8 },
        { L"B580", 32 }, { L"A770", 28 }, { L"B570", 28 }, { L"A750", 25 }, { L"A580", 20 }, { L"A380", 8 },
        { L"890M", 14 }, { L"880M", 12 }, { L"780M", 12 }, { L"760M", 10 }, { L"680M", 10 }, { L"Arc(TM) Graphics", 8 }, { L"Iris", 4 },
        { L"UHD", 3 },
    };
    const WCHAR *n = SCORE.gpu_name;
    int s = -1;
    for (int i = 0; i < countof(k) && s < 0; i++)
        if (has_i(n, k[i].name)) s = k[i].score;
    if (s < 0) {
        f64 gb = (f64)SCORE.vram / (1024.0 * 1024 * 1024);
        s = SCORE.gpu_integrated ? 5 : gb >= 16 ? 50 : gb >= 12 ? 40 : gb >= 8 ? 30 : gb >= 6 ? 20 : gb >= 4 ? 12 : 6;
    }
    if (has_i(n, L"Laptop")) s = s * 3 / 4;
    return CLAMP(s, 1, 100);
}

static int score_cpu(void)
{
    // threads: 4 -> 20, 8 -> 40, 12 -> 55, 16 -> 65, 24 -> 78, 32 -> 88
    static const int k_t[][2] = { { 1, 6 }, { 2, 10 }, { 4, 20 }, { 6, 30 }, { 8, 40 }, { 12, 55 }, { 16, 65 }, { 20, 72 }, { 24, 78 }, { 32, 88 } };
    int t = MIN(SCORE.threads, 32), base = 6;
    for (int i = 0; i < countof(k_t); i++) {
        if (t >= k_t[i][0]) base = k_t[i][1];
        if (i + 1 < countof(k_t) && t > k_t[i][0] && t < k_t[i + 1][0])
            base = k_t[i][1] + (k_t[i + 1][1] - k_t[i][1]) * (t - k_t[i][0]) / (k_t[i + 1][0] - k_t[i][0]);
    }
    // the generation, from the model number after the brand
    const WCHAR *n = SCORE.cpu_name;
    f32 gen = 0.9f;
    const WCHAR *p = NULL;
    if (has_i(n, L"Core Ultra")) gen = 1.12f;
    else if ((p = wcsstr(n, L"Ryzen")) != NULL) {
        while (*p && !(iswdigit(p[0]) && iswdigit(p[1]) && iswdigit(p[2]))) p++;
        int model = _wtoi(p), series = model >= 1000 ? model / 1000 : 9;  // Ryzen AI 300: three digits, the newest
        gen = series >= 9 ? 1.18f : series >= 7 ? 1.12f : series == 5 ? 1.0f : series == 3 ? 0.85f : 0.72f;
        if (has_i(n, L"X3D")) gen += 0.1f;
    } else if ((p = wcsstr(n, L"i3-")) || (p = wcsstr(n, L"i5-")) || (p = wcsstr(n, L"i7-")) || (p = wcsstr(n, L"i9-"))) {
        // 12400 -> 12, 9400 -> 9; laptop parts carry the generation in two digits of four: 1165G7 -> 11
        int model = _wtoi(p + 3), g = model >= 10000 ? model / 1000 : model < 2000 ? model / 100 : model / 1000;
        gen =g >= 13 ? 1.1f : g == 12 ? 1.02f : g >= 10 ? 0.84f : g >= 8 ? 0.74f : 0.62f;
    }
    // laptop chips: U parts are the low-power ones
    int len = wlen(n);
    while (len > 0 && n[len - 1] == ' ') len--;
    if (len > 0 && (n[len - 1] == 'U' || has_i(n, L"U Processor") || has_i(n, L"U with"))) gen *= 0.75f;
    return CLAMP((int)((f32)base * gen + 0.5f), 1, 100);
}

static int score_ram(void)
{
    f64 gb = (f64)SCORE.ram / (1024.0 * 1024 * 1024);
    int s = gb >= 60 ? 100 : gb >= 30 ? 90 : gb >= 15 ? 65 : gb >= 7.5 ? 35 : 15;
    if (SCORE.ram_type == 0x22 || SCORE.ram_type == 0x23) s += 6;                 // DDR5 / LPDDR5
    else if (SCORE.ram_type == 0x1A && SCORE.ram_speed >= 3600) s += 3;            // fast DDR4
    else if (SCORE.ram_type && SCORE.ram_type < 0x1A) s -= 10;                     // DDR3 and older
    return CLAMP(s, 1, 100);
}

static int score_disk(void)
{
    static const int k[] = { 50, 25, 70, 100 };
    return k[CLAMP(SCORE.sys_disk, 0, 3)];
}

static void score_compute(void)
{
    SCORE.part[SC_GPU] = score_gpu();
    SCORE.part[SC_CPU] = score_cpu();
    SCORE.part[SC_RAM] = score_ram();
    SCORE.part[SC_DISK] = score_disk();
    static const int w[SC__COUNT] = { 40, 30, 15, 15 };
    int t = 0;
    for (int i = 0; i < SC__COUNT; i++) t += SCORE.part[i] * w[i];
    SCORE.total = CLAMP((t + 50) / 100, 1, 100);
}

static const WCHAR *score_tier(int s)
{
    return s >= 90 ? TR("Топовый", "Top tier") : s >= 75 ? TR("Мощный", "Powerful") : s >= 55 ? TR("Хороший", "Good")
         : s >= 35 ? TR("Средний", "Average") : TR("Базовый", "Basic");
}
