# How MixBooster works

PC tuning for Windows 10 and 11. One native exe in C, no frameworks; the same renderer as MixLauncher
(Direct3D 11, DirectComposition, DirectWrite, Mica).

## GPU Affinity

Finds the CPU core that handles the GPU's interrupts best and keeps the GPU on it.

1. **Test.** Pick 1 to 8 runs and press *Start test*. For every logical processor the GPU interrupts are pinned to it,
   the driver is restarted, the liblava triangle demo runs fullscreen (30 s warm-up) and PresentMon records 90 s of
   frame times. This is the AutoGpuAffinity method; the screen flickers on each driver restart.
2. **Results.** The table has an *AVG* tab (the mean over all runs) and a tab per run: average, max and min FPS,
   1% / 0.1% / 0.01% lows, 1% / 0.1% percentiles and the frame time STDEV. The best value of each column is green.
3. **Bind.** The two best cores are shown next to the test controls, the best one selected. *Bind the GPU* pins the
   GPU (applied at once, by restarting its driver) and the PCI bridge it is attached to (applied after a reboot) to that
   core, and installs the **MixBooster GPU Affinity** service.

The service (`MixBooster.exe --service`, copied to `Program Files\MixBooster`) checks the binding at startup and every
time a display adapter arrives. A driver install or update recreates the device's registry key; the service writes the
policy back and restarts the GPU driver. *Reset* removes the service and the policy.

The ranking: in every run on its own, every metric as the core's distance from that run's median core (capped at 15%), weighted: 1% low 22, 0.1% low 18,
1% percentile 12, average 12, 0.01% low 10, 0.1% percentile 10, STDEV 10, min 4, max 2. A core's score is the median of its run
scores minus their spread, so a core that is good in every run beats one that is the best only now and then. CPU 0 is never suggested, and
binding to it by hand asks first: Windows handles most of its own interrupts there.

Data lives in `%ProgramData%\MixBooster` (`gpu-affinity.txt`, `log.txt`, the unpacked tools in `bin`).
The binding is in `HKLM\SOFTWARE\MixBooster` (`GpuMask`).

## MSI Mode

The PCI devices with an interrupt, grouped like MSI_util v3: what each supports (MSI-X, MSI, line-based IRQ), what it
runs on now, a switch for MSI (`MessageSignaledInterruptProperties\MSISupported`) and the interrupt priority
(`Affinity Policy\DevicePriority`). Edits wait for *Apply* and take effect after a restart; changing a disk controller
asks first, since a controller that fails to start can keep Windows from booting.

What *Apply* changes is kept in `HKLM\SOFTWARE\MixBooster\Msi` with the values that were there before. The same service
writes the wanted values back when a driver install drops them (it watches every device arrival), and *Reset all* puts
the old values back. The service stays installed while it keeps the GPU core, MSI settings or a game fix, and is
removed when none is left, whichever is reset last.

When the MixBoost playbook's `MixBoost MSI Mode` task is there, the first *Apply* adopts the values it set and removes the
task, so only one keeper is left; *Reset all* registers the task again with the playbook's `setMsiMode.ps1`.

## Core parking

What ParkControl does. The power plans, with a button to turn one on; for the chosen plan, core parking (min cores
kept unparked, `CPMINCORES`, and on hybrid CPUs the P-cores' `CPMINCORES1` apart), the minimum and maximum
processor state, plugged in and on battery. Changes go straight into the plan through powrprof and apply at once
on the active one. Under them, every logical processor live: asleep or not, load, clock (performance counters).

Where the MixBoost playbook's plan, *MixBoost Low Latency (HP)* (`11111111-1111-1111-1111-111111111111`), is
missing, *Add the MixBoost plan* makes it the way `setPowerPlan.ps1` does: a copy of Ultimate Performance (else
High performance, else Balanced) under that GUID with Hybred's Low Latency values, skipping the ones the Windows build
doesn't have, and turns it on.

## Graphics

Switches for HAGS (`GraphicsDrivers\HwSchMode`, with what the GPU supports and runs now from D3DKMT), MPO
(`Dwm\OverlayTestMode = 5` and, for 24H2, `GraphicsDrivers\DisableOverlays`), variable refresh rate and windowed-game
optimizations (`DirectXUserGlobalSettings`, as Settings writes it) and fullscreen optimizations
(`GameConfigStore\GameDVR_*`), and core isolation: VBS with memory integrity (`DeviceGuard\EnableVirtualizationBasedSecurity`
and `Scenarios\HypervisorEnforcedCodeIntegrity\Enabled`, both 0 or both 1, after a restart; whether it runs now comes
from `NtQuerySystemInformation`, and a group policy or a UEFI lock leaves the switch off). They apply on a click; MPO
and FSO keep what was there before in `HKLM\SOFTWARE\MixBooster\Gfx`, so turning them back on restores it.

## Game fixes

Fixes per game exe. The games are found in the Steam libraries, the Epic manifests, the installed programs (Riot,
Battle.net, BSG) and among the running processes; *Add a game* takes any other exe. A click on a game opens a dialog
with a switch per fix; a fix for a particular CPU shows only on that CPU.

- **High CPU priority.** Writes `Image File Execution Options\<exe>\PerfOptions\CpuPriorityClass = 3`, so
  Windows starts the game with High priority.
- **High GPU priority.** The service sets the game's GPU scheduling priority to High
  (`D3DKMTSetProcessSchedulingPriorityClass`).
- **CPU 0 off.** The service takes CPU 0 out of the game's affinity. On a CPU with 4 threads or fewer the switch warns.
- **P-cores only** (hybrid Intel CPUs). The game runs only on the cores of the highest efficiency class.
- **3D V-Cache CCD only** (Ryzen X3D with two CCDs). The game runs only on the CCD with the bigger L3.
- **No power throttling (EcoQoS)** (hybrid CPUs and laptops). `SetProcessInformation(ProcessPowerThrottling)` with
  execution speed throttling off, so Windows doesn't slow the game down when it loses focus.

The service looks every 3 seconds, once per process; a game already running gets a change at once. The affinity
fixes add up (P-cores without CPU 0, for example).

Where Process Lasso is installed, it keeps the priority and the affinity instead of the service: the rules go into
its `prolasso.ini` (`DefaultPriorities`, `DefaultAffinitiesEx`, `DefaultGPUPriorities`) and Lasso keeps them, running
or from its next start. EcoQoS, and an affinity that isn't one run of CPUs, stay with the service. A rule the user had
there for that exe is kept and put back when the fix goes off. If Lasso is removed later, the service takes the games
back.

What is on lives in `HKLM\SOFTWARE\MixBooster\Games` (`Fix`, the bits of the fixes on), with what IFEO and Process
Lasso had before.

## Restart needed

A card in the sidebar, on every page, while something waits for a restart: MSI Mode, the GPU's PCIe port, HAGS, MPO,
VBS, or Windows Update. Each change leaves a mark in `HKLM\SOFTWARE\MixBooster\Reboot` with the boot it was made in; a
mark from an earlier boot is dropped. The card opens the restart dialog.

## PC health

Drives (NVMe health log, SMART, free space on the system volume: a warning under 80 GB, or under a quarter of a
smaller one), battery, memory, drivers, stability over 30 days and device problems. Memory: the
speed against the modules' rating, read from the part number for the gaming brands (an XMP / EXPO kit far below it
has its profile off), single channel, mixed modules, and the last Windows Memory Diagnostic result. Drivers: each
GPU driver's age (a warning after 6 months, 12 for an integrated GPU), and whether the chipset has AMD's or Intel's
driver or only Windows' own.

## Build

```
build.bat          release -> build\MixBooster.exe (asks for administrator rights)
build.bat dev      no elevation prompt, for working on the UI
build.bat debug
```

MSVC (found through vswhere) or MinGW-w64 gcc/clang. `tools/make-icon.py` draws the icon (needs Pillow).

## Third-party

Packed into the exe and unpacked on first use:

- [liblava](https://github.com/liblava/liblava) `lava-triangle.exe`, MIT, `third_party/liblava/LICENSE.txt`
- [PresentMon](https://github.com/GameTechDev/PresentMon) 1.8.0, MIT, `third_party/presentmon/LICENSE.txt`
