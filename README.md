<p align="center">
  <img src="docs/screens/components.png" alt="MixBooster" width="860">
</p>

<h1 align="center"><img src="docs/logo.png" alt="" width="34" align="top"> MixBooster</h1>

<p align="center">
  PC tuning for gaming on Windows 10 and 11.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0b0b0d?style=flat-square" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/one%20exe-~4.9%20MB-0b0b0d?style=flat-square" alt="One exe, ~4.9 MB">
  <img src="https://img.shields.io/badge/C-no%20frameworks-0b0b0d?style=flat-square" alt="C, no frameworks">
  <img src="https://img.shields.io/badge/no%20telemetry-0b0b0d?style=flat-square" alt="No telemetry">
</p>

## Features

- **GPU Affinity.** Tests every CPU core with the GPU's interrupts on it and binds the GPU to the steadiest one.
- **MSI Mode.** MSI and interrupt priority for every PCI device, in one table.
- **Core parking.** Power plans, parking and processor state, with every core shown live; adds the MixBoost Low Latency plan.
- **Graphics.** HAGS, MPO, VRR, windowed and fullscreen optimizations in one place.
- **Game fixes.** CPU 0 off with High CPU and GPU priority for each game; finds Steam, Epic, Riot and Battle.net games and works with Process Lasso.
- **PC components and health.** A gaming score for the PC, drive health, memory, drivers, BSODs and WHEA errors.
- **Kept after driver updates.** A small service puts the GPU core, the MSI settings and the game fixes back.
- **No AI, no telemetry.**

## Screenshots

<table>
  <tr>
    <td align="center"><img src="docs/screens/affinity.png" alt="GPU Affinity" width="420"><br>GPU Affinity</td>
    <td align="center"><img src="docs/screens/games.png" alt="Game fixes" width="420"><br>Game fixes</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screens/msi.png" alt="MSI Mode" width="420"><br>MSI Mode</td>
    <td align="center"><img src="docs/screens/parking.png" alt="Core parking" width="420"><br>Core parking</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screens/graphics.png" alt="Graphics" width="420"><br>Graphics</td>
    <td align="center"><img src="docs/screens/health.png" alt="PC health" width="420"><br>PC health</td>
  </tr>
</table>

## Build

```
build.bat          release -> build\MixBooster.exe (runs as administrator)
build.bat dev      no elevation prompt, for working on the UI
```

MSVC or MinGW-w64. How each page works and what it changes: [docs/how-it-works.md](docs/how-it-works.md).

Bundled: [liblava](https://github.com/liblava/liblava) and [PresentMon](https://github.com/GameTechDev/PresentMon), both MIT.
