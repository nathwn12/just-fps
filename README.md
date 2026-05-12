# justFPS

A lean FPS overlay for Windows. ETW-powered frame capture, LibreHardwareMonitor hardware polling, ImGui overlay rendering. One exe, no services, no telemetry.

```
justFPS.exe
├── ETW trace session    → Present event stream (D3D9-12, OpenGL, Vulkan)
├── LHWM poller          → GPU/CPU sensors (NVIDIA, AMD, Intel)
├── Win32 layered window → Transparent click-through overlay
└── config.ini           → Auto-created, persists everything
```

## Requirements

- Windows 10 or 11
- Run as **Administrator** (ETW kernel tracing requires elevation)
- Game set to **Borderless** or **Borderless Windowed** (true fullscreen blocks overlays)

## Quick start

```bash
git clone https://github.com/nathwn12/just-fps.git
cd just-fps
build-msvc.bat
.\build\justFPS.exe
```

Default toggle: **F12**. Hold **CTRL** + right-click over the overlay for the context menu.

## Overlay presets

Pick a preset or go fully custom:

| Preset | Stats displayed |
|--------|----------------|
| **JustFPS** | Framerate only |
| **FpsDetails** | FPS + 1% / 0.1% lows |
| **FpsDetailsCpuGpuUtil** | FPS, lows, CPU util, GPU util |
| **FpsCpuGpuRamFullDetails** | Everything — CPU/GPU util, temp, freq, power, fan, VRAM, RAM |
| **Custom** | You pick exactly what shows |

Per-stat granularity: CPU util, CPU temp, CPU frequency, GPU util, GPU temp, GPU hotspot, GPU power, GPU fan, GPU frequency, VRAM, RAM, and FPS min/max — all toggleable independently.

## Position presets

Six snap positions: top-left, top-center, top-right, bottom-left, bottom-center, bottom-right. Horizontal single-line layout.

## Controls

| Action | How |
|--------|-----|
| Toggle overlay | **F12** (configurable) |
| Open settings | Configurable hotkey (unset by default) |
| Context menu | Hold **CTRL** + right-click over overlay |
| Temperature unit | Switch between °C and °F in settings |

All hotkeys support Ctrl/Alt/Shift modifiers.

## Auto-start

Enable auto-start in settings to skip the config window and launch the overlay immediately on next run. Lives in the system tray — double-click to reopen settings.

## Why this exists

Existing FPS tools ship with driver suites, recording platforms, OC utilities, accounts, and ads. This one ships with an FPS counter.

- **~6MB** single exe — one file, no installer
- **No background services**, no startup entries, no resident processes
- **Zero telemetry, zero accounts, zero ads, zero social features**
- **Anti-cheat safe** — ETW never touches the game process (no injection, no hooks)

### What I tried and moved on from

AMD Adrenalin (bloated, overlay unreliable), Xbox Game Bar (wouldn't reinstall), NVIDIA App (three background services for a counter), MSI Afterburner (great tool, ships everything but the kitchen sink), NZXT CAM (telemetry + bloat), Steam Overlay (decent, but library isn't on Steam), Overwolf (ads), RivaTuner (the OG, but 90% features unused).

## Graphics API support

| API | Works |
|-----|-------|
| DirectX 12 | ✅ |
| DirectX 11 | ✅ |
| DirectX 10/10.1 | ✅ |
| DirectX 9 | ✅ (via D3D9 ETW provider) |
| OpenGL | ✅ (via DxgKrnl ETW provider) |
| Vulkan | ✅ (via DxgKrnl ETW provider) |

## Building from source

### Requirements

- Windows 10 or 11
- Visual Studio 2022 Build Tools (Desktop development with C++ workload)

### Build

```bash
# In a Visual Studio Developer Command Prompt:
build-msvc.bat
```

Output lands in `build\justFPS.exe` with `lhwm-wrapper.dll` alongside it. `config.ini` is auto-created on first run.

### Project layout

```
just-fps/
├── src/
│   ├── main.cpp          # ~3K lines, everything in one file
│   └── resource.rc       # Version info + icon
├── libs/
│   ├── imgui/            # Dear ImGui (compiled directly via vcxproj)
│   └── lhwm/             # LHWM wrapper DLL + header + lib
├── build/                # Build output
├── icon.ico
├── build-msvc.bat
├── justFPS.sln
├── justFPS.vcxproj
└── README.md
```

## Stack

| Layer | Choice |
|-------|--------|
| Language | C++20 |
| Build | MSVC v143 (VS 2022) |
| Render | DirectX 11 |
| UI | Dear ImGui |
| FPS source | Windows ETW (D3D9, DXGI, DxgKrnl providers) |
| Hardware | LibreHardwareMonitor via lhwm-wrapper (NVIDIA, AMD, Intel) |
| CPU temp fallback | WMI |
| Window | Win32 layered transparent (WS_EX_LAYERED, WS_EX_TRANSPARENT) |
| Security | DLL load hardening, PawnIO hash verification, PID-scoped ETW sessions |

## License

GNU General Public License v3.0 — see [LICENSE.txt](LICENSE.txt).

### Original work

This project is a derivative of **fps-overlay** by [aneeskhan47](https://github.com/aneeskhan47), available at [github.com/aneeskhan47/fps-overlay](https://github.com/aneeskhan47/fps-overlay). Both the original and this derivative are licensed under GPLv3.

Substantive modifications include: rebranding to justFPS with renamed executable, five overlay presets with batch per-stat toggling, granular display flags replacing coarse showCPU/showGPU, addition of GPU hotspot/power/fan speed monitoring, FPS min/max display, text-only frequency display (removed sparkline graphs), redesigned config UI with preset chips and card-based layout, hotkey modifier support (Ctrl/Alt/Shift), six position presets (was four corners), configurable settings key with default unset, auto-start option, `MODE_CONFIG`/`MODE_OVERLAY` state machine with command queue, backward-compatible `SyncLegacyDisplayFlags()` migration, renamed solution/project files, and removal of `imgui_internal.h` dependency.

---

*No bloat. No telemetry. Just stats.*
