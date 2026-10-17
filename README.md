<h1><img src="res/logo.png" alt="" width="34" align="top"> MixBooster</h1>

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

The ranking: every metric as the core's distance from the median core (capped at 15%), weighted: 1% low 22, 0.1% low 18,
1% percentile 12, average 12, 0.01% low 10, 0.1% percentile 10, STDEV 10, min 4, max 2. CPU 0 is never suggested, and
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
the old values back. The service stays installed while it keeps either the GPU core or MSI settings, and is removed
when neither is left, whichever is reset last.

When the MixBoost playbook's `MixBoost MSI Mode` task is there, the first *Apply* adopts the values it set and removes the
task, so only one keeper is left; *Reset all* registers the task again with the playbook's `setMsiMode.ps1`.

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
