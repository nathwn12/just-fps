// justFPS - Lightweight DirectX 11 + ImGui performance monitor
//
// Copyright (C) 2026 nathwn12
//
// Based on fps-overlay (https://github.com/aneeskhan47/fps-overlay)
// Copyright (C) 2026 Anees
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Features:
//   - Real game FPS via ETW (Event Tracing for Windows)
//     Supports: DirectX 9/10/11/12, Vulkan, OpenGL via DXGI + DxgKrnl providers
//   - GPU usage & temperature via LibreHardwareMonitor (supports NVIDIA, AMD, Intel)
//   - CPU / RAM monitoring
//   - Hardware names (CPU model, GPU model)
//   - Custom hotkey binding
//   - System tray integration
//
// Requires: run as Administrator for game FPS capture (ETW needs it)

#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <winhttp.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

// Note: Link with -lwbemuuid -lole32 -loleaut32 for WMI support
// Note: Link with lhwm-cpp-wrapper.lib and mscoree.lib for LibreHardwareMonitor support

#include "imgui.h"
#include "lhwm-cpp-wrapper.h"
#include <tuple>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ═══════════════════════════════════════════════════════════════════════════
// Constants & safety defines
// ═══════════════════════════════════════════════════════════════════════════
#define WM_TRAYICON   (WM_USER + 1)
#define IDM_SETTINGS  1001
#define IDM_EXIT      1002
#define IDM_SHOW      1003
#define IDM_HIDE      1004
#define IDM_UPDATE    1005

// Current version
#define APP_VERSION "v2.2.2"

// PawnIO installer resource ID (embedded executable)
#define IDR_PAWNIO_SETUP 101

#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
#define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
#define PROCESS_TRACE_MODE_REAL_TIME    0x00000100
#endif
#ifndef EVENT_CONTROL_CODE_ENABLE_PROVIDER
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER 1
#endif
#ifndef TRACE_LEVEL_INFORMATION
#define TRACE_LEVEL_INFORMATION 4
#endif

// Modifier flags for hotkey bindings
#define HMOD_NONE  0
#define HMOD_CTRL  1
#define HMOD_ALT   2
#define HMOD_SHIFT 4

// Microsoft-Windows-DXGI provider  {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
static const GUID DXGI_PROVIDER =
    { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };

// Microsoft-Windows-D3D9 provider  {783ACA0A-790E-4D7F-8451-AA850511C6B9}
static const GUID D3D9_PROVIDER =
    { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };

// Microsoft-Windows-DxgKrnl provider  {802EC45A-1E99-4B83-9920-87C98277BA9D}
// This captures presents at the kernel level for ALL graphics APIs: DX9/10/11/12, Vulkan, OpenGL
static const GUID DXGKRNL_PROVIDER =
    { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

// DxgKrnl keywords for Present tracking
static const ULONGLONG DXGKRNL_KEYWORD_PRESENT = 0x8000000;  // Present keyword
static const ULONGLONG DXGKRNL_KEYWORD_BASE    = 0x1;        // Base keyword

// DxgKrnl event IDs for present tracking
static const USHORT DXGKRNL_EVENT_PRESENT_INFO = 0x00B8;  // Present::Info (184)
static const USHORT DXGKRNL_EVENT_FLIP_INFO    = 0x00A8;  // Flip::Info (168)
static const USHORT DXGKRNL_EVENT_BLIT_INFO    = 0x00A6;  // Blit::Info (166)

static const char* GetEtwSessionName() {
    static char name[64] = "";
    if (name[0] == '\0')
        snprintf(name, sizeof(name), "justFPS_ETW_%lu", GetCurrentProcessId());
    return name;
}

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════
enum OverlayPreset {
    JustFPS = 0,
    FpsDetails = 1,
    FpsDetailsCpuGpuUtil = 2,
    FpsCpuGpuRamFullDetails = 3,
    Custom = 4,
};

struct OverlayConfig {
    bool showFPS  = true;
    bool showCPU  = true;
    bool showGPU  = true;
    bool showCpuUtil = true;
    bool showCpuTemp = true;
    bool showGpuUtil = true;
    bool showGpuTemp = true;
    bool showGpuHotspot = true;
    bool showVRAM = false;     // GPU VRAM usage
    bool showGpuPower = false; // GPU power draw
    bool showGpuFan = false;   // GPU fan speed
    bool showRAM  = false;
    bool useFahrenheit = false; // false = Celsius, true = Fahrenheit
    bool autoStart = true;   // skip config window and start overlay immediately
    bool showFpsLowHigh = true;
    bool showCpuFrequency = true;
    bool showGpuFrequency = true;
    int  position = 4;        // 0=TL 1=TR 2=BL 3=BR 4=TC 5=BC
    float edgePadding = 16.0f; // distance from screen edge (px base, scaled by uiScale)
    float uiScale = 1.0f;     // overlay UI scale (1.0 = default)
    float textSaturation = 1.0f; // overlay text saturation/contrast
    float hudBgAlpha = 0.0f;     // HUD pill background opacity (0=off, 1=full)
    int  toggleKey    = VK_F12;
    int  settingsKey  = 0;
    int  toggleMod    = 0;   // modifier flags for toggle key
    int  settingsMod  = 0;   // modifier flags for settings key
    int  selectedGpu  = 0;     // selected GPU index (0 = first GPU)
    OverlayPreset preset = Custom;
};


// ═══════════════════════════════════════════════════════════════════════════
// GPU list (for multi-GPU support via LHWM)
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_GPUS 8
struct GpuInfo {
    char name[256];
    std::string tempPath;      // LHWM sensor path for temperature
    std::string hotspotPath;   // LHWM sensor path for GPU hotspot temperature
    std::string loadPath;      // LHWM sensor path for GPU load
    std::string vramUsedPath;  // LHWM sensor path for VRAM used
    std::string vramTotalPath; // LHWM sensor path for VRAM total
    std::string clockPath;     // LHWM sensor path for GPU core clock
    std::string clockMaxPath;  // LHWM sensor path for GPU boost/max clock
    std::string powerPath;     // LHWM sensor path for GPU power
    std::string fanPath;       // LHWM sensor path for GPU fan
};
static GpuInfo g_gpuList[MAX_GPUS];
static int g_gpuCount = 0;

// Helper to convert Celsius to Fahrenheit
inline float ToDisplayTemp(float celsius, bool useFahrenheit) {
    return useFahrenheit ? (celsius * 9.0f / 5.0f + 32.0f) : celsius;
}

inline ImVec4 AdjustTextSaturation(const ImVec4& color, float saturation) {
    const float luminance = color.x * 0.299f + color.y * 0.587f + color.z * 0.114f;
    return ImVec4(
        std::clamp(luminance + (color.x - luminance) * saturation, 0.0f, 1.0f),
        std::clamp(luminance + (color.y - luminance) * saturation, 0.0f, 1.0f),
        std::clamp(luminance + (color.z - luminance) * saturation, 0.0f, 1.0f),
        color.w
    );
}

inline ImVec4 AdjustNeutralTextContrast(const ImVec4& color, float saturation) {
    float contrastScale;
    if (saturation <= 1.0f) {
        contrastScale = 0.55f + 0.45f * saturation;
    } else {
        contrastScale = 1.0f + 0.25f * ((saturation - 1.0f) / 0.40f);
    }
    return ImVec4(
        std::clamp(color.x * contrastScale, 0.0f, 1.0f),
        std::clamp(color.y * contrastScale, 0.0f, 1.0f),
        std::clamp(color.z * contrastScale, 0.0f, 1.0f),
        color.w
    );
}

// Temperature thresholds (in Celsius) - adjust for F display comparison
inline float GetHighTempThreshold(bool useFahrenheit) { return useFahrenheit ? 185.0f : 85.0f; }
inline float GetMedTempThreshold(bool useFahrenheit) { return useFahrenheit ? 158.0f : 70.0f; }

// ═══════════════════════════════════════════════════════════════════════════
// Configuration file (INI) - saved next to justFPS.exe
// ═══════════════════════════════════════════════════════════════════════════
static char g_configPath[MAX_PATH] = "";

static void InitConfigPath()
{
    if (g_configPath[0] != '\0') return; // already initialized
    
    // Get the directory where the executable is located
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    
    // Remove the executable name to get the directory
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    
    // Append the config filename
    snprintf(g_configPath, MAX_PATH, "%sconfig.ini", exePath);
}

static int ReadIniInt(const char* section, const char* key, int defaultVal)
{
    return GetPrivateProfileIntA(section, key, defaultVal, g_configPath);
}

static float ReadIniFloat(const char* section, const char* key, float defaultVal)
{
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (buf[0] == '\0') return defaultVal;
    return (float)atof(buf);
}

static void WriteIniInt(const char* section, const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}

static void WriteIniFloat(const char* section, const char* key, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}

static bool IniKeyExists(const char* section, const char* key)
{
    char buf[4];
    return GetPrivateProfileStringA(section, key, nullptr, buf, sizeof(buf), g_configPath) > 0;
}

static void SyncLegacyDisplayFlags(OverlayConfig& cfg)
{
    cfg.showCPU = cfg.showCpuUtil || cfg.showCpuTemp;
    cfg.showGPU = cfg.showGpuUtil || cfg.showGpuTemp;
}

static void ApplyPreset(OverlayConfig& cfg, OverlayPreset preset)
{
    cfg.preset = preset;

    switch (preset) {
    case JustFPS:
        cfg.showFPS = true;
        cfg.showFpsLowHigh = false;
        cfg.showCpuUtil = false;
        cfg.showCpuTemp = false;
        cfg.showGpuUtil = false;
        cfg.showGpuTemp = false;
        cfg.showGpuHotspot = false;
        cfg.showGpuPower = false;
        cfg.showGpuFan = false;
        cfg.showCpuFrequency = false;
        cfg.showGpuFrequency = false;
        cfg.showVRAM = false;
        cfg.showRAM = false;
        break;
    case FpsDetails:
        cfg.showFPS = true;
        cfg.showFpsLowHigh = true;
        cfg.showCpuUtil = false;
        cfg.showCpuTemp = false;
        cfg.showGpuUtil = false;
        cfg.showGpuTemp = false;
        cfg.showGpuHotspot = false;
        cfg.showGpuPower = false;
        cfg.showGpuFan = false;
        cfg.showCpuFrequency = false;
        cfg.showGpuFrequency = false;
        cfg.showVRAM = false;
        cfg.showRAM = false;
        break;
    case FpsDetailsCpuGpuUtil:
        cfg.showFPS = true;
        cfg.showFpsLowHigh = true;
        cfg.showCpuUtil = true;
        cfg.showCpuTemp = false;
        cfg.showGpuUtil = true;
        cfg.showGpuTemp = false;
        cfg.showGpuHotspot = false;
        cfg.showGpuPower = false;
        cfg.showGpuFan = false;
        cfg.showCpuFrequency = false;
        cfg.showGpuFrequency = false;
        cfg.showVRAM = false;
        cfg.showRAM = false;
        break;
    case FpsCpuGpuRamFullDetails:
        cfg.showFPS = true;
        cfg.showFpsLowHigh = true;
        cfg.showCpuUtil = true;
        cfg.showCpuTemp = true;
        cfg.showGpuUtil = true;
        cfg.showGpuTemp = true;
        cfg.showGpuHotspot = true;
        cfg.showGpuPower = false;
        cfg.showGpuFan = false;
        cfg.showCpuFrequency = true;
        cfg.showGpuFrequency = true;
        cfg.showVRAM = true;
        cfg.showRAM = true;
        break;
    case Custom:
    default:
        break;
    }

    SyncLegacyDisplayFlags(cfg);
}

static bool SameDisplayFlags(const OverlayConfig& a, const OverlayConfig& b)
{
    return a.showFPS == b.showFPS &&
           a.showFpsLowHigh == b.showFpsLowHigh &&
           a.showCpuUtil == b.showCpuUtil &&
           a.showCpuTemp == b.showCpuTemp &&
           a.showGpuUtil == b.showGpuUtil &&
           a.showGpuTemp == b.showGpuTemp &&
           a.showGpuHotspot == b.showGpuHotspot &&
           a.showGpuPower == b.showGpuPower &&
           a.showGpuFan == b.showGpuFan &&
           a.showCpuFrequency == b.showCpuFrequency &&
           a.showGpuFrequency == b.showGpuFrequency &&
           a.showVRAM == b.showVRAM &&
           a.showRAM == b.showRAM;
}

static OverlayPreset DetectPreset(const OverlayConfig& cfg)
{
    OverlayConfig expected = cfg;

    ApplyPreset(expected, JustFPS);
    if (SameDisplayFlags(cfg, expected)) return JustFPS;

    ApplyPreset(expected, FpsDetails);
    if (SameDisplayFlags(cfg, expected)) return FpsDetails;

    ApplyPreset(expected, FpsDetailsCpuGpuUtil);
    if (SameDisplayFlags(cfg, expected)) return FpsDetailsCpuGpuUtil;

    ApplyPreset(expected, FpsCpuGpuRamFullDetails);
    if (SameDisplayFlags(cfg, expected)) return FpsCpuGpuRamFullDetails;

    return Custom;
}

static const char* GetPresetLabel(OverlayPreset preset) {
    switch (preset) {
    case OverlayPreset::JustFPS:               return "JUST FPS";
    case OverlayPreset::FpsDetails:            return "FPS DETAILS";
    case OverlayPreset::FpsDetailsCpuGpuUtil:  return "FPS DETAILS, CPU & GPU UTILIZATION";
    case OverlayPreset::FpsCpuGpuRamFullDetails: return "FPS, CPU, GPU & RAM FULL DETAILS";
    case OverlayPreset::Custom:                return "Custom";
    default:                                   return "Unknown";
    }
}

static void LoadConfig(OverlayConfig& cfg)
{
    InitConfigPath();

    // Check if config file exists
    DWORD attrib = GetFileAttributesA(g_configPath);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        SyncLegacyDisplayFlags(cfg);
        cfg.preset = DetectPreset(cfg);
        return;
    }

    // Display settings
    bool hasPreset = IniKeyExists("Display", "preset");
    int presetValue = ReadIniInt("Display", "preset", static_cast<int>(Custom));

    if (hasPreset && presetValue >= static_cast<int>(JustFPS) && presetValue <= static_cast<int>(Custom)) {
        cfg.preset = static_cast<OverlayPreset>(presetValue);
        if (cfg.preset != Custom) {
            ApplyPreset(cfg, cfg.preset);
        } else {
            bool legacyShowCPU   = ReadIniInt("Display", "showCPU", 1) != 0;
            bool legacyShowGPU   = ReadIniInt("Display", "showGPU", 1) != 0;
            cfg.showFPS         = ReadIniInt("Display", "showFPS", 1) != 0;
            cfg.showFpsLowHigh  = ReadIniInt("Display", "showFpsLowHigh", 1) != 0;
            cfg.showCpuUtil     = IniKeyExists("Display", "showCpuUtil") ? ReadIniInt("Display", "showCpuUtil", 1) != 0 : legacyShowCPU;
            cfg.showCpuTemp     = IniKeyExists("Display", "showCpuTemp") ? ReadIniInt("Display", "showCpuTemp", 1) != 0 : legacyShowCPU;
            cfg.showGpuUtil     = IniKeyExists("Display", "showGpuUtil") ? ReadIniInt("Display", "showGpuUtil", 1) != 0 : legacyShowGPU;
            cfg.showGpuTemp     = IniKeyExists("Display", "showGpuTemp") ? ReadIniInt("Display", "showGpuTemp", 1) != 0 : legacyShowGPU;
            cfg.showGpuHotspot  = IniKeyExists("Display", "showGpuHotspot") ? ReadIniInt("Display", "showGpuHotspot", 1) != 0 : legacyShowGPU;
            cfg.showGpuPower    = IniKeyExists("Display", "showGpuPower") ? ReadIniInt("Display", "showGpuPower", 0) != 0 : false;
            cfg.showGpuFan      = IniKeyExists("Display", "showGpuFan") ? ReadIniInt("Display", "showGpuFan", 0) != 0 : false;
            cfg.showCpuFrequency = IniKeyExists("Display", "showCpuFrequency") ? ReadIniInt("Display", "showCpuFrequency", 1) != 0 : legacyShowCPU;
            cfg.showGpuFrequency = IniKeyExists("Display", "showGpuFrequency") ? ReadIniInt("Display", "showGpuFrequency", 1) != 0 : legacyShowGPU;
            cfg.showVRAM        = IniKeyExists("Display", "showVRAM") ? ReadIniInt("Display", "showVRAM", 0) != 0 : legacyShowGPU;
            cfg.showRAM         = IniKeyExists("Display", "showRAM") ? ReadIniInt("Display", "showRAM", 0) != 0 : false;
        }
    } else {
        cfg.showFPS         = ReadIniInt("Display", "showFPS", 1) != 0;
        bool legacyShowCPU  = ReadIniInt("Display", "showCPU", 1) != 0;
        bool legacyShowGPU  = ReadIniInt("Display", "showGPU", 1) != 0;
        cfg.showCpuUtil     = legacyShowCPU;
        cfg.showCpuTemp     = legacyShowCPU;
        cfg.showGpuUtil     = legacyShowGPU;
        cfg.showGpuTemp     = legacyShowGPU;
        cfg.showGpuHotspot  = ReadIniInt("Display", "showGpuHotspot", 1) != 0;
        cfg.showGpuPower    = ReadIniInt("Display", "showGpuPower", 0) != 0;
        cfg.showGpuFan      = ReadIniInt("Display", "showGpuFan", 0) != 0;
        cfg.showVRAM        = ReadIniInt("Display", "showVRAM", 0) != 0;
        cfg.showRAM         = ReadIniInt("Display", "showRAM", 0) != 0;
        cfg.showFpsLowHigh  = ReadIniInt("Display", "showFpsLowHigh", 1) != 0;
        cfg.showCpuFrequency = ReadIniInt("Display", "showCpuFrequency", 1) != 0;
        cfg.showGpuFrequency = ReadIniInt("Display", "showGpuFrequency", 1) != 0;
        SyncLegacyDisplayFlags(cfg);
        cfg.preset = DetectPreset(cfg);
    }

    cfg.autoStart     = ReadIniInt("Layout", "autoStart", 1) != 0;
    cfg.position      = ReadIniInt("Layout", "position", 4);
    cfg.edgePadding   = ReadIniFloat("Layout", "edgePadding", 16.0f);
    cfg.uiScale       = ReadIniFloat("Layout", "uiScale", 1.0f);
    cfg.textSaturation = ReadIniFloat("Layout", "textSaturation", 1.0f);
    cfg.hudBgAlpha     = ReadIniFloat("Layout", "hudBgAlpha", 0.0f);

    // Hotkeys
    cfg.toggleKey     = ReadIniInt("Hotkeys", "toggleKey", VK_F12);
    cfg.settingsKey   = ReadIniInt("Hotkeys", "exitKey", 0);
    cfg.toggleMod     = ReadIniInt("Hotkeys", "toggleMod", 0);
    cfg.settingsMod   = ReadIniInt("Hotkeys", "settingsMod", 0);
    
    // GPU selection
    cfg.selectedGpu   = ReadIniInt("GPU", "selectedGpu", 0);
    
    // Clamp values to valid ranges
    if (cfg.position < 0 || cfg.position > 5) cfg.position = 4;
    if (cfg.uiScale < 0.75f) cfg.uiScale = 0.75f;
    if (cfg.uiScale > 2.25f) cfg.uiScale = 2.25f;
    if (cfg.textSaturation < 0.00f) cfg.textSaturation = 0.00f;
    if (cfg.textSaturation > 1.40f) cfg.textSaturation = 1.40f;
    if (cfg.hudBgAlpha < 0.00f) cfg.hudBgAlpha = 0.00f;
    if (cfg.hudBgAlpha > 1.00f) cfg.hudBgAlpha = 1.00f;
    if (cfg.selectedGpu < 0) cfg.selectedGpu = 0;

    SyncLegacyDisplayFlags(cfg);
}

// Check if welcome message has been shown (separate from config)
static bool HasWelcomeBeenShown()
{
    InitConfigPath();
    return ReadIniInt("App", "welcomeShown", 0) != 0;
}

static void MarkWelcomeShown()
{
    InitConfigPath();
    WriteIniInt("App", "welcomeShown", 1);
}

// Show welcome message on first run
static void ShowWelcomeMessage()
{
    if (HasWelcomeBeenShown()) {
        return;  // Already shown before
    }
    
    MessageBoxA(
        nullptr,
        "Welcome to justFPS!\n\n"
        "For the best experience, it is recommended to disable other FPS overlays:\n\n"
        "  - Steam Overlay (Steam > Settings > In-Game)\n"
        "  - Xbox Game Bar (Windows Settings > Gaming)\n"
        "  - NVIDIA GeForce Experience Overlay/NVIDIA ShadowPlay/NVIDIA App\n"
        "  - AMD Radeon Software Overlay\n"
        "  - Discord Overlay\n\n"
        "This prevents conflicts and ensures accurate FPS readings.\n\n"
        "Enjoy!",
        "justFPS",
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST
    );
    
    MarkWelcomeShown();
}

static void SaveConfig(const OverlayConfig& cfg)
{
    InitConfigPath();
    
    // Display settings
    WriteIniInt("Display", "preset", static_cast<int>(cfg.preset));
    WriteIniInt("Display", "showFPS", cfg.showFPS ? 1 : 0);
    WriteIniInt("Display", "showCPU", cfg.showCPU ? 1 : 0);
    WriteIniInt("Display", "showGPU", cfg.showGPU ? 1 : 0);
    WriteIniInt("Display", "showCpuUtil", cfg.showCpuUtil ? 1 : 0);
    WriteIniInt("Display", "showCpuTemp", cfg.showCpuTemp ? 1 : 0);
    WriteIniInt("Display", "showGpuUtil", cfg.showGpuUtil ? 1 : 0);
    WriteIniInt("Display", "showGpuTemp", cfg.showGpuTemp ? 1 : 0);
    WriteIniInt("Display", "showGpuHotspot", cfg.showGpuHotspot ? 1 : 0);
    WriteIniInt("Display", "showGpuPower", cfg.showGpuPower ? 1 : 0);
    WriteIniInt("Display", "showGpuFan", cfg.showGpuFan ? 1 : 0);
    WriteIniInt("Display", "showVRAM", cfg.showVRAM ? 1 : 0);
    WriteIniInt("Display", "showRAM", cfg.showRAM ? 1 : 0);
    WriteIniInt("Display", "showFpsLowHigh", cfg.showFpsLowHigh ? 1 : 0);
    WriteIniInt("Display", "showCpuFrequency", cfg.showCpuFrequency ? 1 : 0);
    WriteIniInt("Display", "showGpuFrequency", cfg.showGpuFrequency ? 1 : 0);
    
    // Layout settings
    WriteIniInt("Layout", "useFahrenheit", cfg.useFahrenheit ? 1 : 0);
    WriteIniInt("Layout", "autoStart", cfg.autoStart ? 1 : 0);
    WriteIniInt("Layout", "position", cfg.position);
    WriteIniFloat("Layout", "edgePadding", cfg.edgePadding);
    WriteIniFloat("Layout", "uiScale", cfg.uiScale);
    WriteIniFloat("Layout", "textSaturation", cfg.textSaturation);
    WriteIniFloat("Layout", "hudBgAlpha", cfg.hudBgAlpha);

    // Hotkeys
    WriteIniInt("Hotkeys", "toggleKey", cfg.toggleKey);
    WriteIniInt("Hotkeys", "exitKey", cfg.settingsKey);
    WriteIniInt("Hotkeys", "toggleMod", cfg.toggleMod);
    WriteIniInt("Hotkeys", "settingsMod", cfg.settingsMod);
    
    // GPU selection
    WriteIniInt("GPU", "selectedGpu", cfg.selectedGpu);
}

// ═══════════════════════════════════════════════════════════════════════════
// Hotkey helpers
// ═══════════════════════════════════════════════════════════════════════════
static bool ModsDown(int modFlags)
{
    if ((modFlags & HMOD_CTRL)  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if ((modFlags & HMOD_ALT)   && !(GetAsyncKeyState(VK_MENU)   & 0x8000)) return false;
    if ((modFlags & HMOD_SHIFT) && !(GetAsyncKeyState(VK_SHIFT)  & 0x8000)) return false;
    return true;
}

static int HeldMods()
{
    int m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= HMOD_CTRL;
    if (GetAsyncKeyState(VK_MENU)   & 0x8000) m |= HMOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)  & 0x8000) m |= HMOD_SHIFT;
    return m;
}

static const char* GetKeyName(int vk);

static void FormatKeyBinding(char* buf, int bufSize, int vk, int mod)
{
    if (vk <= 0) {
        strcpy_s(buf, bufSize, "(unbound)");
        return;
    }
    buf[0] = '\0';
    if (mod & HMOD_CTRL)  { strcat_s(buf, bufSize, "Ctrl+"); }
    if (mod & HMOD_ALT)   { strcat_s(buf, bufSize, "Alt+"); }
    if (mod & HMOD_SHIFT) { strcat_s(buf, bufSize, "Shift+"); }
    strcat_s(buf, bufSize, GetKeyName(vk));
}

// ═══════════════════════════════════════════════════════════════════════════
// App state
// ═══════════════════════════════════════════════════════════════════════════
enum AppMode    { MODE_CONFIG, MODE_OVERLAY };
enum PendingCmd { CMD_NONE, CMD_START_OVERLAY, CMD_SHOW_SETTINGS, CMD_EXIT };

static OverlayConfig g_Config;
static AppMode       g_Mode       = MODE_CONFIG;
static PendingCmd    g_Pending    = CMD_NONE;
static bool          g_Running    = true;
static bool          g_OvlVisible = true;
static bool          g_overlaySettingsOpen = false;
static bool          g_overlaySettingsJustOpened = false;

static HINSTANCE      g_hInstance = nullptr;
static HWND           g_hwnd     = nullptr;
static HANDLE         g_singleInstanceMutex = nullptr;
static NOTIFYICONDATA g_nid      = {};
static RECT           g_overlayBounds = {0, 0, 0, 0};  // ImGui overlay bounds for hit-testing
static ImFont*        g_baseFont = nullptr;
static ImFont*        g_overlayValueFont = nullptr;
static ImFont*        g_overlayLabelFont = nullptr;

// ── Hardware info ──
static char g_cpuName[256] = "Unknown";
static char g_gpuName[256] = "Unknown";

// ── Update checker state ──
static std::atomic<bool> g_updateAvailable{false};
static std::atomic<bool> g_updateCheckDone{false};
static char g_latestVersion[32] = "";

// ═══════════════════════════════════════════════════════════════════════════
// Update checker (checks GitHub releases API)
// ═══════════════════════════════════════════════════════════════════════════
static void CheckForUpdatesAsync()
{
    std::thread([]() {
        HINTERNET hSession = WinHttpOpen(L"FPS-Overlay/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { g_updateCheckDone = true; return; }

        HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com",
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
            L"/repos/nathwn12/just-fps/releases/latest",
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        // Validate HTTP response
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }
        if (statusCode != 200) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        // Read response
        std::string response;
        DWORD bytesRead = 0;
        char buffer[4096];
        const size_t kMaxResponse = 65536;
        while (response.size() < kMaxResponse &&
               WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response.append(buffer, bytesRead);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        // Parse "tag_name" from JSON response (simple string search)
        const char* tagKey = "\"tag_name\"";
        size_t pos = response.find(tagKey);
        if (pos != std::string::npos) {
            pos = response.find(':', pos);
            if (pos != std::string::npos) {
                size_t start = response.find('"', pos + 1);
                size_t end = response.find('"', start + 1);
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    std::string latestTag = response.substr(start + 1, end - start - 1);
                    snprintf(g_latestVersion, sizeof(g_latestVersion), "%s", latestTag.c_str());
                    
                    // Basic semver validation
                    bool validFormat = (latestTag.size() >= 5 && latestTag[0] == 'v');
                    if (validFormat) {
                        for (size_t i = 1; i < latestTag.size(); i++) {
                            char c = latestTag[i];
                            if (!(c == '.' || (c >= '0' && c <= '9'))) {
                                validFormat = false;
                                break;
                            }
                        }
                    }
                    if (validFormat && strcmp(g_latestVersion, APP_VERSION) != 0) {
                        g_updateAvailable = true;
                    }
                }
            }
        }
        g_updateCheckDone = true;
    }).detach();
}

// ── GPU stats (from LHWM) ──
static float g_gpuUsage = 0.0f;
static float g_gpuTemp  = 0.0f;
static float g_gpuHotspotTemp = 0.0f;
static float g_gpuPower = 0.0f;
static float g_gpuFan = 0.0f;
static float g_vramUsed  = 0.0f;  // in GB
static float g_vramTotal = 0.0f;  // in GB

// ── Clock frequency values (from LHWM) ──
static float g_cpuClockMhz = 0.0f;
static float g_cpuClockMaxMhz = 0.0f;
static float g_cpuClockMaxObserved = 0.0f;
static float g_gpuClockMhz = 0.0f;
static float g_gpuClockMaxMhz = 0.0f;

// ── ETW state ──
static TRACEHANDLE      g_etwSession = 0;
static TRACEHANDLE      g_etwTrace   = 0;
static std::thread      g_etwThread;
static std::atomic<bool>  g_etwThreadStarted{false};
static std::atomic<bool>  g_etwRunning{false};
static std::thread      g_etwStartupThread;
static std::atomic<float> g_gameFps{0.0f};
static float g_fpsLow = 0.0f;
static float g_fpsHigh = 0.0f;
static bool  g_fpsRangeInitialized = false;
static std::atomic<DWORD> g_targetPid{0};
static DWORD              g_lastTargetPid = 0;    // to detect PID change
static bool               g_etwAvailable = false;
static bool               g_isAdmin = false;      // running as administrator?
static double              g_qpcFreq     = 1.0;
static char               g_targetProcessName[128] = "";  // current tracked process name

// ── CPU temperature (WMI) ──
static float g_cpuTemp = 0.0f;
static bool  g_cpuTempAvailable = false;

// ── LibreHardwareMonitor (LHWM) state ──
static std::atomic<bool> g_lhwmAvailable{false};
static std::thread      g_lhwmInitThread;
static std::string g_lhwmCpuTempPath;      // e.g., "/amdcpu/0/temperature/3"
static std::string g_lhwmGpuTempPath;      // e.g., "/gpu-nvidia/0/temperature/0"
static std::string g_lhwmGpuHotspotPath;   // GPU hotspot temperature, if available
static std::string g_lhwmGpuLoadPath;      // e.g., "/gpu-nvidia/0/load/0"
static std::string g_lhwmGpuVramUsedPath;  // VRAM used
static std::string g_lhwmGpuVramTotalPath; // VRAM total
static std::string g_lhwmGpuPowerPath;
static std::string g_lhwmGpuFanPath;
static std::string g_lhwmCpuClockPath;
static std::string g_lhwmCpuClockMaxPath;
static std::string g_lhwmGpuClockPath;
static std::string g_lhwmGpuClockMaxPath;
static float g_lhwmCpuTemp = 0.0f;         // CPU temp from LHWM (used directly)

// ── DX11 ──
static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext  = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

// ── Hotkey listener state ──
static int  g_listeningFor = 0;   // 0=none, 1=toggle, 2=settings

// ═══════════════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════════════
bool    CreateDeviceD3D(HWND);
void    CleanupDeviceD3D();
void    CreateRenderTarget();
void    CleanupRenderTarget();
void    AddTrayIcon();
void    RemoveTrayIcon();
void    UpdateTrayTooltip();
void    SwitchToOverlay();
void    SwitchToConfig();
void    ShutdownBackends();
void    InitBackends();
void    ApplyStyle();
static float GetCpuUsage();

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND, UINT, WPARAM, LPARAM);

// ═══════════════════════════════════════════════════════════════════════════
// Utility: key name from VK code
// ═══════════════════════════════════════════════════════════════════════════
static const char* GetKeyName(int vk)
{
    static char buf[64];
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;

    // Extended-key flag for nav keys
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
        case VK_NUMLOCK: case VK_SNAPSHOT: case VK_CANCEL:
            lp |= (1 << 24);
            break;
    }

    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0)
        return buf;
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════
// Admin check
// ═══════════════════════════════════════════════════════════════════════════
static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// ═══════════════════════════════════════════════════════════════════════════
// Hardware detection
// ═══════════════════════════════════════════════════════════════════════════
static void QueryCpuName()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD sz = sizeof(g_cpuName);
        RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(g_cpuName), &sz);
        RegCloseKey(hKey);

        // trim leading spaces
        char* p = g_cpuName;
        while (*p == ' ') p++;
        if (p != g_cpuName) memmove(g_cpuName, p, strlen(p) + 1);
    }
}

static void QueryGpuName()
{
    if (!g_pd3dDevice) return;

    IDXGIDevice* dxgiDev = nullptr;
    g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice),
                                 reinterpret_cast<void**>(&dxgiDev));
    if (!dxgiDev) return;

    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            g_gpuName, sizeof(g_gpuName), nullptr, nullptr);
        adapter->Release();
    }
    dxgiDev->Release();
}

// ═══════════════════════════════════════════════════════════════════════════
// Process name and description from PID
// ═══════════════════════════════════════════════════════════════════════════
static void GetFileDescription(const char* filePath, char* outDesc, size_t maxLen)
{
    outDesc[0] = '\0';
    
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeA(filePath, &dummy);
    if (size == 0) return;
    
    std::vector<char> data(size);
    if (!GetFileVersionInfoA(filePath, 0, size, data.data())) return;
    
    // Try to get FileDescription
    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate;
    UINT cbTranslate = 0;
    
    if (!VerQueryValueA(data.data(), "\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate))
        return;
    
    if (cbTranslate < sizeof(LANGANDCODEPAGE)) return;
    
    char subBlock[128];
    snprintf(subBlock, sizeof(subBlock),
             "\\StringFileInfo\\%04x%04x\\FileDescription",
             lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
    
    char* description = nullptr;
    UINT descLen = 0;
    if (VerQueryValueA(data.data(), subBlock,
                       reinterpret_cast<LPVOID*>(&description), &descLen)) {
        if (description && descLen > 0 && description[0] != '\0') {
            snprintf(outDesc, maxLen, "%s", description);
        }
    }
}

static void GetProcessName(DWORD pid, char* outName, size_t maxLen)
{
    outName[0] = '\0';
    if (pid == 0) return;
    
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (hProc) {
        char fullPath[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(hProc, 0, fullPath, &size)) {
            // Extract just the filename
            const char* exeName = strrchr(fullPath, '\\');
            if (exeName) exeName++; else exeName = fullPath;
            
            // Try to get file description
            char description[256] = {};
            GetFileDescription(fullPath, description, sizeof(description));
            
            if (description[0]) {
                snprintf(outName, maxLen, "%s (%s)", exeName, description);
            } else {
                snprintf(outName, maxLen, "%s", exeName);
            }
        }
        CloseHandle(hProc);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CPU Temperature via WMI (works on some systems)
// ═══════════════════════════════════════════════════════════════════════════
static IWbemLocator*   g_pWbemLocator  = nullptr;
static IWbemServices*  g_pWbemServices = nullptr;
static bool            g_wmiInitialized = false;

static bool InitWMI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    
    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE,
                              nullptr, EOAC_NONE, nullptr);
    // Ignore if already initialized
    
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&g_pWbemLocator));
    if (FAILED(hr)) return false;
    
    // Try OpenHardwareMonitor WMI namespace first (most reliable)
    hr = g_pWbemLocator->ConnectServer(
        _bstr_t(L"ROOT\\OpenHardwareMonitor"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_pWbemServices);
    
    if (FAILED(hr)) {
        // Try standard WMI namespace (works on some systems)
        hr = g_pWbemLocator->ConnectServer(
            _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
            0, nullptr, nullptr, &g_pWbemServices);
    }
    
    if (FAILED(hr)) {
        g_pWbemLocator->Release();
        g_pWbemLocator = nullptr;
        return false;
    }
    
    hr = CoSetProxyBlanket(g_pWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    
    g_wmiInitialized = true;
    return true;
}

static void ShutdownWMI()
{
    if (g_pWbemServices) { g_pWbemServices->Release(); g_pWbemServices = nullptr; }
    if (g_pWbemLocator)  { g_pWbemLocator->Release();  g_pWbemLocator  = nullptr; }
    g_wmiInitialized = false;
}

static float QueryCpuTemperature()
{
    if (!g_wmiInitialized || !g_pWbemServices) return 0.0f;
    
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hr;
    
    // Try OpenHardwareMonitor sensor query
    hr = g_pWbemServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name LIKE '%CPU%'"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnumerator);
    
    if (FAILED(hr)) {
        // Try MSAcpi_ThermalZoneTemperature (built-in, but less reliable)
        hr = g_pWbemServices->ExecQuery(
            _bstr_t(L"WQL"),
            _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &pEnumerator);
    }
    
    if (FAILED(hr)) return 0.0f;
    
    float temp = 0.0f;
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    
    if (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
        VARIANT vtProp;
        VariantInit(&vtProp);
        
        // Try "Value" first (OpenHardwareMonitor)
        hr = pObj->Get(L"Value", 0, &vtProp, nullptr, nullptr);
        if (SUCCEEDED(hr) && vtProp.vt == VT_R4) {
            temp = vtProp.fltVal;
        } else {
            // Try "CurrentTemperature" (MSAcpi - returns in tenths of Kelvin)
            VariantClear(&vtProp);
            hr = pObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr);
            if (SUCCEEDED(hr) && (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4)) {
                // Convert from tenths of Kelvin to Celsius
                temp = (vtProp.lVal / 10.0f) - 273.15f;
            }
        }
        VariantClear(&vtProp);
        pObj->Release();
    }
    
    pEnumerator->Release();
    return temp;
}

// ═══════════════════════════════════════════════════════════════════════════
// PawnIO Driver Installation — Required for CPU temperature access
// ═══════════════════════════════════════════════════════════════════════════
static bool g_pawnIOPromptShown = false;  // Only show prompt once per session

// Check if PawnIO driver is installed via registry
// LibreHardwareMonitor checks: SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO
static bool IsPawnIOInstalled()
{
    HKEY hKey = nullptr;
    
    // Try native registry first (64-bit on 64-bit Windows, 32-bit on 32-bit Windows)
    LONG result = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        0, KEY_READ, &hKey
    );
    
    if (result == ERROR_SUCCESS) {
        // Check if DisplayVersion exists
        char versionStr[64] = {0};
        DWORD size = sizeof(versionStr);
        DWORD type = REG_SZ;
        result = RegQueryValueExA(hKey, "DisplayVersion", nullptr, &type, 
                                  reinterpret_cast<LPBYTE>(versionStr), &size);
        RegCloseKey(hKey);
        
        if (result == ERROR_SUCCESS && versionStr[0] != '\0') {
            return true;
        }
    }
    
    // Try 64-bit registry view explicitly (for 32-bit apps on 64-bit Windows)
    result = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        0, KEY_READ | KEY_WOW64_64KEY, &hKey
    );
    
    if (result == ERROR_SUCCESS) {
        char versionStr[64] = {0};
        DWORD size = sizeof(versionStr);
        DWORD type = REG_SZ;
        result = RegQueryValueExA(hKey, "DisplayVersion", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(versionStr), &size);
        RegCloseKey(hKey);
        
        if (result == ERROR_SUCCESS && versionStr[0] != '\0') {
            return true;
        }
    }
    
    return false;
}

// Extract embedded PawnIO_setup.exe and run it
static bool ExtractAndRunPawnIOSetup()
{
    HMODULE hModule = GetModuleHandle(nullptr);
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(IDR_PAWNIO_SETUP), RT_RCDATA);
    if (!hResource) return false;

    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource) return false;

    LPVOID pResourceData = LockResource(hLoadedResource);
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!pResourceData || dwResourceSize == 0) return false;

    // Create unique temp directory to prevent race attacks
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);

    char tempDir[MAX_PATH];
    snprintf(tempDir, MAX_PATH, "%sjustFPS_%08X", tempPath, GetCurrentProcessId());
    CreateDirectoryA(tempDir, nullptr);

    char tempFile[MAX_PATH];
    snprintf(tempFile, MAX_PATH, "%s\\PawnIO_setup.exe", tempDir);

    // Exclusive creation — fail if file already exists (race prevention)
    HANDLE hFile = CreateFileA(tempFile, GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(tempDir);
        return false;
    }

    DWORD bytesWritten;
    BOOL writeResult = WriteFile(hFile, pResourceData, dwResourceSize, &bytesWritten, nullptr);
    CloseHandle(hFile);

    if (!writeResult || bytesWritten != dwResourceSize) {
        DeleteFileA(tempFile);
        RemoveDirectoryA(tempDir);
        return false;
    }

    // Verify hash before executing
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hashDigest[32];
    DWORD hashLen = sizeof(hashDigest);

    if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        if (hProv) CryptReleaseContext(hProv, 0);
        DeleteFileA(tempFile);
        RemoveDirectoryA(tempDir);
        return false;
    }

    // Re-read the file to hash it
    HANDLE hReadFile = CreateFileA(tempFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hReadFile != INVALID_HANDLE_VALUE) {
        BYTE readBuf[8192];
        DWORD readBytes;
        while (ReadFile(hReadFile, readBuf, sizeof(readBuf), &readBytes, nullptr) && readBytes > 0) {
            CryptHashData(hHash, readBuf, readBytes, 0);
        }
        CloseHandle(hReadFile);
    }

    DWORD digestLen = sizeof(hashDigest);
    CryptGetHashParam(hHash, HP_HASHVAL, hashDigest, &digestLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    // Pre-computed SHA-256 of PawnIO_setup.exe
    const BYTE kExpectedHash[32] = {
        0x3A, 0x34, 0xB5, 0xDF, 0x23, 0x1F, 0x10, 0xF2,
        0x52, 0xC4, 0xB5, 0x0D, 0x3C, 0x77, 0x75, 0x34,
        0xB2, 0xA2, 0xCE, 0xC5, 0xE1, 0xDF, 0x94, 0x66,
        0xAB, 0x2D, 0xCA, 0x40, 0x1C, 0x06, 0x8C, 0x44
    };

    if (memcmp(hashDigest, kExpectedHash, 32) != 0) {
        DeleteFileA(tempFile);
        RemoveDirectoryA(tempDir);
        return false;  // Hash mismatch — refuse to execute
    }

    // Run the installer silently with -install flag
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = tempFile;
    sei.lpParameters = "-install";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    bool success = false;
    if (ShellExecuteExA(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }
        success = true;
    }

    DeleteFileA(tempFile);
    RemoveDirectoryA(tempDir);
    return success;
}

// Show prompt to install PawnIO driver
static bool PromptAndInstallPawnIO()
{
    if (g_pawnIOPromptShown) {
        return false;  // Already prompted this session
    }
    g_pawnIOPromptShown = true;
    
    int result = MessageBoxA(
        nullptr,
        "PawnIO driver is not installed.\n\n"
        "This driver is required for LibreHardwareMonitor to correctly read "
        "CPU and GPU temperatures on modern systems.\n\n"
        "Would you like to install it now?\n\n"
        "(You may need to restart justFPS after installation)",
        "justFPS - Driver Required",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST
    );
    
    if (result == IDYES) {
        return ExtractAndRunPawnIOSetup();
    }
    
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// LibreHardwareMonitor (LHWM) — cross-vendor hardware monitoring
// ═══════════════════════════════════════════════════════════════════════════
// Helper to check if a hardware name is a GPU (excluding integrated graphics)
static bool IsDiscreteGpu(const std::string& name) {
    // Check for discrete GPU identifiers
    bool isGpu = (name.find("GeForce") != std::string::npos ||
                  name.find("RTX") != std::string::npos ||
                  name.find("GTX") != std::string::npos ||
                  name.find("Radeon RX") != std::string::npos ||
                  name.find("Radeon Pro") != std::string::npos ||
                  name.find("Arc") != std::string::npos ||  // Intel Arc
                  name.find("NVIDIA") != std::string::npos);
    
    // Exclude integrated graphics
    bool isIntegrated = (name.find("Radeon Graphics") != std::string::npos ||  // AMD APU
                         name.find("Intel UHD") != std::string::npos ||
                         name.find("Intel HD") != std::string::npos ||
                         name.find("Intel Iris") != std::string::npos);
    
    return isGpu && !isIntegrated;
}

// Find an existing GPU in the list by name, or return -1
static int FindGpuByName(const char* name) {
    for (int i = 0; i < g_gpuCount; i++) {
        if (strcmp(g_gpuList[i].name, name) == 0) return i;
    }
    return -1;
}

static bool InitLHWM()
{
    try {
        auto sensors = LHWM::GetHardwareSensorMap();
        if (sensors.empty()) return false;
        
        // The map structure from lhwm-cpp-wrapper is:
        // Key (map key) = Hardware name (e.g., "AMD Ryzen 9 5900HS...")
        // Value = vector<tuple<sensorName, sensorType, sensorPath>>
        //   tuple[0] = Sensor name (e.g., "CPU Core #1", "GPU Core")
        //   tuple[1] = Sensor type (e.g., "Temperature", "Load")
        //   tuple[2] = Sensor path (e.g., "/amdcpu/0/temperature/0")
        
        std::string cpuTempFallback;
        g_gpuCount = 0;  // Reset GPU count
        
        for (const auto& [hardwareName, sensorList] : sensors) {
            // Check if this is CPU or GPU hardware by hardware name
            bool isCpuHardware = (hardwareName.find("Ryzen") != std::string::npos ||
                                  hardwareName.find("Intel") != std::string::npos ||
                                  hardwareName.find("CPU") != std::string::npos ||
                                  hardwareName.find("Core") != std::string::npos);
            
            bool isDiscreteGpuHardware = IsDiscreteGpu(hardwareName);
            
            // If this is a discrete GPU, find or create entry in GPU list
            int gpuIndex = -1;
            if (isDiscreteGpuHardware && g_gpuCount < MAX_GPUS) {
                // Clean the hardware name - LHWM may return "Name : /path", we only want the name
                std::string cleanName = hardwareName;
                size_t colonPos = cleanName.find(" : ");
                if (colonPos != std::string::npos) {
                    cleanName = cleanName.substr(0, colonPos);
                }
                
                gpuIndex = FindGpuByName(cleanName.c_str());
                if (gpuIndex < 0) {
                    // New GPU - add to list
                    gpuIndex = g_gpuCount;
                    snprintf(g_gpuList[gpuIndex].name, sizeof(g_gpuList[gpuIndex].name), "%s", cleanName.c_str());
                    g_gpuList[gpuIndex].tempPath.clear();
                    g_gpuList[gpuIndex].hotspotPath.clear();
                    g_gpuList[gpuIndex].loadPath.clear();
                    g_gpuList[gpuIndex].vramUsedPath.clear();
                    g_gpuList[gpuIndex].vramTotalPath.clear();
                    g_gpuList[gpuIndex].clockPath.clear();
                    g_gpuList[gpuIndex].clockMaxPath.clear();
                    g_gpuList[gpuIndex].powerPath.clear();
                    g_gpuList[gpuIndex].fanPath.clear();
                    g_gpuCount++;
                }
            }
            
            // Iterate through all sensors for this hardware
            for (const auto& sensorInfo : sensorList) {
                const auto& [sensorName, sensorType, sensorPath] = sensorInfo;
                
                // Also detect by path pattern
                bool isCpuPath = (sensorPath.find("/amdcpu/") != std::string::npos ||
                                  sensorPath.find("/intelcpu/") != std::string::npos);
                bool isGpuPath = (sensorPath.find("/gpu-nvidia/") != std::string::npos ||
                                  sensorPath.find("/gpu-amd/") != std::string::npos ||
                                  sensorPath.find("/gpu-intel/") != std::string::npos);
                
                // CPU temperature sensors
                if ((isCpuHardware || isCpuPath) && sensorType == "Temperature") {
                    // Priority order for CPU temp (matching Task Manager):
                    // 1. "Soc" - AMD SoC temperature (most accurate for overall CPU temp)
                    // 2. "Package" - Intel package temp
                    // 3. Fallback to any other CPU temp
                    if (sensorName == "Soc" || sensorName.find("Soc") != std::string::npos) {
                        g_lhwmCpuTempPath = sensorPath;  // Best choice for AMD
                    } else if (g_lhwmCpuTempPath.empty() && 
                               sensorName.find("Package") != std::string::npos) {
                        g_lhwmCpuTempPath = sensorPath;  // Best choice for Intel
                    } else if (g_lhwmCpuTempPath.empty()) {
                        cpuTempFallback = sensorPath;
                    }
                    
                }
                
                // CPU Clock sensors
                if ((isCpuHardware || isCpuPath) && sensorType == "Clock") {
                    if (sensorName.find("Core") != std::string::npos || g_lhwmCpuClockPath.empty()) {
                        g_lhwmCpuClockPath = sensorPath;
                    }
                    if (sensorName.find("Max") != std::string::npos || sensorName.find("Boost") != std::string::npos) {
                        g_lhwmCpuClockMaxPath = sensorPath;
                    }
                }
                
                // GPU sensors - store in the GPU's entry
                if (gpuIndex >= 0) {
                    if (sensorType == "Temperature") {
                        // Prefer "GPU Core" for normal temp, store hotspot separately if available.
                        bool isHotSpot = (sensorName.find("Hot Spot") != std::string::npos ||
                                          sensorName.find("Hotspot") != std::string::npos ||
                                          sensorName.find("Hot Spot") != std::string::npos ||
                                          sensorPath.find("/hotspot") != std::string::npos);
                        bool isGpuCore = (sensorName == "GPU Core" || 
                                          sensorName.find("GPU Core") != std::string::npos);
                        
                        if (isHotSpot) {
                            g_gpuList[gpuIndex].hotspotPath = sensorPath;
                        } else if (isGpuCore) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Best choice
                        } else if (g_gpuList[gpuIndex].tempPath.empty()) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Fallback (not hotspot)
                        }
                    }
                    else if (sensorType == "Load") {
                        if (sensorName.find("Core") != std::string::npos || 
                            sensorName.find("GPU") != std::string::npos ||
                            g_gpuList[gpuIndex].loadPath.empty()) {
                            g_gpuList[gpuIndex].loadPath = sensorPath;
                        }
                    }
                    else if (sensorType == "SmallData" || sensorType == "Data") {
                        if (sensorName.find("Memory Used") != std::string::npos ||
                            sensorName.find("GPU Memory Used") != std::string::npos) {
                            g_gpuList[gpuIndex].vramUsedPath = sensorPath;
                        }
                        else if (sensorName.find("Memory Total") != std::string::npos ||
                                 sensorName.find("GPU Memory Total") != std::string::npos) {
                            g_gpuList[gpuIndex].vramTotalPath = sensorPath;
                        }
                    }
                    else if (sensorType == "Power") {
                        if (sensorName.find("GPU") != std::string::npos || sensorName.find("Core") != std::string::npos || g_gpuList[gpuIndex].powerPath.empty()) {
                            g_gpuList[gpuIndex].powerPath = sensorPath;
                        }
                    }
                    else if (sensorType == "Fan") {
                        if (g_gpuList[gpuIndex].fanPath.empty()) {
                            g_gpuList[gpuIndex].fanPath = sensorPath;
                        }
                    }
                    else if (sensorType == "Clock") {
                        if (sensorName.find("Core") != std::string::npos || g_gpuList[gpuIndex].clockPath.empty()) {
                            g_gpuList[gpuIndex].clockPath = sensorPath;
                        }
                        if (sensorName.find("Max") != std::string::npos || sensorName.find("Boost") != std::string::npos) {
                            g_gpuList[gpuIndex].clockMaxPath = sensorPath;
                        }
                    }
                }
            }
        }
        
        // Use fallback CPU temp if needed
        if (g_lhwmCpuTempPath.empty() && !cpuTempFallback.empty()) {
            g_lhwmCpuTempPath = cpuTempFallback;
        }
        
        // Clamp selected GPU to valid range
        if (g_Config.selectedGpu >= g_gpuCount) {
            g_Config.selectedGpu = 0;
        }
        
        // Set active GPU paths and name
        if (g_gpuCount > 0) {
            int idx = g_Config.selectedGpu;
            g_lhwmGpuTempPath = g_gpuList[idx].tempPath;
            g_lhwmGpuHotspotPath = g_gpuList[idx].hotspotPath;
            g_lhwmGpuLoadPath = g_gpuList[idx].loadPath;
            g_lhwmGpuVramUsedPath = g_gpuList[idx].vramUsedPath;
            g_lhwmGpuVramTotalPath = g_gpuList[idx].vramTotalPath;
            g_lhwmGpuClockPath = g_gpuList[idx].clockPath;
            g_lhwmGpuClockMaxPath = g_gpuList[idx].clockMaxPath;
            g_lhwmGpuPowerPath = g_gpuList[idx].powerPath;
            g_lhwmGpuFanPath = g_gpuList[idx].fanPath;
            snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[idx].name);
        }
        
        return !g_lhwmCpuTempPath.empty() || g_gpuCount > 0;
    }
    catch (...) {
        return false;
    }
}

static void PollLHWMStats()
{
    if (!g_lhwmAvailable) return;
    
    try {
        // CPU temperature (stored in g_lhwmCpuTemp, used directly elsewhere)
        if (!g_lhwmCpuTempPath.empty()) {
            g_lhwmCpuTemp = LHWM::GetSensorValue(g_lhwmCpuTempPath);
        }
        
        // GPU stats - write directly to unified variables
        if (!g_lhwmGpuTempPath.empty()) {
            g_gpuTemp = LHWM::GetSensorValue(g_lhwmGpuTempPath);
        }
        if (!g_lhwmGpuHotspotPath.empty()) {
            g_gpuHotspotTemp = LHWM::GetSensorValue(g_lhwmGpuHotspotPath);
        }
        if (!g_lhwmGpuLoadPath.empty()) {
            g_gpuUsage = LHWM::GetSensorValue(g_lhwmGpuLoadPath);
        }
        if (!g_lhwmGpuVramUsedPath.empty()) {
            // Value is in MB, convert to GB
            g_vramUsed = LHWM::GetSensorValue(g_lhwmGpuVramUsedPath) / 1024.0f;
        }
        if (!g_lhwmGpuVramTotalPath.empty()) {
            g_vramTotal = LHWM::GetSensorValue(g_lhwmGpuVramTotalPath) / 1024.0f;
        }
        if (!g_lhwmGpuPowerPath.empty()) {
            g_gpuPower = LHWM::GetSensorValue(g_lhwmGpuPowerPath);
        }
        if (!g_lhwmGpuFanPath.empty()) {
            g_gpuFan = LHWM::GetSensorValue(g_lhwmGpuFanPath);
        }
        
        // Clock sensors
        if (!g_lhwmCpuClockPath.empty()) {
            g_cpuClockMhz = LHWM::GetSensorValue(g_lhwmCpuClockPath);
            if (g_cpuClockMhz > g_cpuClockMaxObserved) g_cpuClockMaxObserved = g_cpuClockMhz;
        }
        if (!g_lhwmCpuClockMaxPath.empty()) {
            g_cpuClockMaxMhz = LHWM::GetSensorValue(g_lhwmCpuClockMaxPath);
        }
        if (!g_lhwmGpuClockPath.empty()) {
            g_gpuClockMhz = LHWM::GetSensorValue(g_lhwmGpuClockPath);
        }
        if (!g_lhwmGpuClockMaxPath.empty()) {
            g_gpuClockMaxMhz = LHWM::GetSensorValue(g_lhwmGpuClockMaxPath);
        }
    }
    catch (...) {
        // Silently ignore polling errors
    }
}

// Switch to a different GPU by index
static void SelectGpu(int index)
{
    if (index < 0 || index >= g_gpuCount) return;
    
    g_Config.selectedGpu = index;
    
    // Update active sensor paths
    g_lhwmGpuTempPath = g_gpuList[index].tempPath;
    g_lhwmGpuHotspotPath = g_gpuList[index].hotspotPath;
    g_lhwmGpuLoadPath = g_gpuList[index].loadPath;
    g_lhwmGpuVramUsedPath = g_gpuList[index].vramUsedPath;
    g_lhwmGpuVramTotalPath = g_gpuList[index].vramTotalPath;
    g_lhwmGpuClockPath = g_gpuList[index].clockPath;
    g_lhwmGpuClockMaxPath = g_gpuList[index].clockMaxPath;
    g_lhwmGpuPowerPath = g_gpuList[index].powerPath;
    g_lhwmGpuFanPath = g_gpuList[index].fanPath;
    
    snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[index].name);
}

// ═══════════════════════════════════════════════════════════════════════════
// ETW — game FPS capture (hooks DXGI Present events system-wide)
// ═══════════════════════════════════════════════════════════════════════════
static void WINAPI EtwCallback(PEVENT_RECORD pEvent)
{
    if (!g_etwRunning.load(std::memory_order_relaxed)) return;

    DWORD pid = pEvent->EventHeader.ProcessId;
    DWORD target = g_targetPid.load(std::memory_order_relaxed);
    if (target == 0 || pid != target) return;

    bool isValidPresentEvent = false;
    bool isDxgiEvent = false;
    bool isD3D9Event = false;
    bool isDxgKrnlOnlyEvent = false;
    
    // Check for DXGI Present::Start (Event ID 42) - DirectX 10/11/12
    if (memcmp(&pEvent->EventHeader.ProviderId, &DXGI_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 42) {
            isValidPresentEvent = true;
            isDxgiEvent = true;
        }
    }
    // Check for D3D9 Present events (Event ID 1 = Present::Start)
    else if (memcmp(&pEvent->EventHeader.ProviderId, &D3D9_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 1) {
            isValidPresentEvent = true;
            isD3D9Event = true;
        }
    }
    // Check for DxgKrnl events - captures Vulkan, OpenGL, and all other graphics APIs at kernel level
    else if (memcmp(&pEvent->EventHeader.ProviderId, &DXGKRNL_PROVIDER, sizeof(GUID)) == 0) {
        USHORT eventId = pEvent->EventHeader.EventDescriptor.Id;
        // Present::Info, Flip::Info, or Blit::Info events indicate a frame present
        if (eventId == DXGKRNL_EVENT_PRESENT_INFO ||
            eventId == DXGKRNL_EVENT_FLIP_INFO ||
            eventId == DXGKRNL_EVENT_BLIT_INFO) {
            isValidPresentEvent = true;
            isDxgKrnlOnlyEvent = true;
        }
    }

    if (!isValidPresentEvent) return;

    double ts = (double)pEvent->EventHeader.TimeStamp.QuadPart / g_qpcFreq;

    // Simple 1-second accumulator (all on the ETW thread — no lock needed)
    static DWORD s_lastPid   = 0;
    static double s_startTs  = 0;
    static int   s_dxgiCount = 0;      // Count of DXGI events (DirectX 10/11/12)
    static int   s_d3d9Count = 0;      // Count of D3D9 events
    static int   s_dxgKrnlCount = 0;   // Count of DxgKrnl-only events (Vulkan/OpenGL)

    if (pid != s_lastPid) { 
        s_lastPid = pid; 
        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts; 
        return; 
    }

    // Count events by source
    if (isDxgiEvent) s_dxgiCount++;
    if (isD3D9Event) s_d3d9Count++;
    if (isDxgKrnlOnlyEvent) s_dxgKrnlCount++;
    
    double elapsed = ts - s_startTs;
    if (elapsed >= 1.0) {
        // Prioritize DXGI/D3D9 (explicit game API calls) over DxgKrnl (kernel-level)
        // This filters out desktop apps like explorer.exe that only show up in DxgKrnl
        //
        // Priority:
        // 1. D3D9 events = DirectX 9 game
        // 2. DXGI events = DirectX 10/11/12 game  
        // 3. ONLY if no DXGI/D3D9 events, use DxgKrnl = Vulkan/OpenGL game
        //
        // Desktop apps (explorer, terminals, browsers) go through DWM which uses DxgKrnl,
        // but they don't call DXGI/D3D9 Present directly, so they get filtered out.
        
        int frameCount = 0;
        
        if (s_d3d9Count > 0) {
            // DirectX 9 game - use D3D9 count
            frameCount = s_d3d9Count;
        } else if (s_dxgiCount > 0) {
            // DirectX 10/11/12 game - use DXGI count
            frameCount = s_dxgiCount;
        } else if (s_dxgKrnlCount > 0) {
            // No DXGI/D3D9 events, only DxgKrnl
            // This could be Vulkan/OpenGL OR a desktop app through DWM
            // 
            // Filter: Only count as a game if FPS >= 20
            // Real games render at 20+ FPS, desktop apps typically < 20 FPS
            float potentialFps = (float)s_dxgKrnlCount / (float)elapsed;
            if (potentialFps >= 20.0f) {
                frameCount = s_dxgKrnlCount;
            }
            // If < 20 FPS, treat as desktop app (frameCount stays 0)
        }
        
        if (frameCount > 0) {
            g_gameFps.store((float)frameCount / (float)elapsed, std::memory_order_relaxed);
        } else {
            g_gameFps.store(0.0f, std::memory_order_relaxed);
        }
        
        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts;
    }
}

static bool StartEtwSession()
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = (double)freq.QuadPart;

    // Buffer for properties + session name
    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;

    // Stop any leftover session from a previous crash
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize   = sizeof(buf);
    buf.p.LoggerNameOffset   = offsetof(decltype(buf), name);
    ControlTraceA(0, GetEtwSessionName(), &buf.p, EVENT_TRACE_CONTROL_STOP);

    // Prepare fresh properties
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize    = sizeof(buf);
    buf.p.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    buf.p.Wnode.ClientContext = 1;                        // QPC timestamps
    buf.p.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    buf.p.LoggerNameOffset    = offsetof(decltype(buf), name);

    ULONG rc = StartTraceA(&g_etwSession, GetEtwSessionName(), &buf.p);
    if (rc != ERROR_SUCCESS) return false;

    // Enable DXGI provider for DirectX 10/11/12 Present events
    rc = EnableTraceEx2(g_etwSession, &DXGI_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    // Enable D3D9 provider for DirectX 9 games
    rc = EnableTraceEx2(g_etwSession, &D3D9_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    // D3D9 provider is optional - continue if it fails

    // Enable DxgKrnl provider for Vulkan, OpenGL, and all other graphics APIs
    // The Present keyword (0x8000000) captures Present, Flip, and Blit events at the kernel level
    rc = EnableTraceEx2(g_etwSession, &DXGKRNL_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 
                        DXGKRNL_KEYWORD_PRESENT | DXGKRNL_KEYWORD_BASE, 
                        0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        // DxgKrnl failed - continue anyway with just DXGI (DirectX will still work)
        // This might fail on older Windows versions or without proper permissions
    }

    EVENT_TRACE_LOGFILEA logFile = {};
    logFile.LoggerName          = const_cast<LPSTR>(GetEtwSessionName());
    logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EtwCallback;

    g_etwTrace = OpenTraceA(&logFile);
    if (g_etwTrace == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    g_etwThread = std::thread([]() {
        TRACEHANDLE h = g_etwTrace;
        ProcessTrace(&h, 1, nullptr, nullptr);
    });
    g_etwThreadStarted.store(true);
    g_etwRunning.store(true);

    return true;
}

static void StopEtwSession()
{
    if (!g_etwRunning.load()) return;
    g_etwRunning.store(false);

    if (g_etwTrace != 0 && g_etwTrace != (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        CloseTrace(g_etwTrace);
        g_etwTrace = 0;
    }

    // Join the startup thread if still running
    if (g_etwStartupThread.joinable()) {
        g_etwStartupThread.join();
    }

    // Join the ETW processing thread
    if (g_etwThreadStarted.load()) {
        if (g_etwThread.joinable()) {
            // ProcessTrace may block; use a bounded wait
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (g_etwThread.joinable() && std::chrono::steady_clock::now() < deadline) {
                Sleep(1);
            }
            if (g_etwThread.joinable()) {
                // ProcessTrace didn't unblock — force-terminate by closing trace handle
                // The thread will eventually wake up; just detach at this point
                g_etwThread.detach();
            } else {
                g_etwThread.join();
            }
        }
    }

    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(g_etwSession, GetEtwSessionName(), &buf.p, EVENT_TRACE_CONTROL_STOP);
    g_etwSession = 0;

    g_gameFps.store(0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// CPU usage
// ═══════════════════════════════════════════════════════════════════════════
static float GetCpuUsage()
{
    static ULARGE_INTEGER sI = {}, sK = {}, sU = {};
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return 0;

    ULARGE_INTEGER i,k,u;
    i.LowPart = fi.dwLowDateTime; i.HighPart = fi.dwHighDateTime;
    k.LowPart = fk.dwLowDateTime; k.HighPart = fk.dwHighDateTime;
    u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;

    ULONGLONG di = i.QuadPart - sI.QuadPart;
    ULONGLONG dk = k.QuadPart - sK.QuadPart;
    ULONGLONG du = u.QuadPart - sU.QuadPart;
    sI = i; sK = k; sU = u;

    ULONGLONG total = dk + du;
    return total ? (1.0f - (float)di / (float)total) * 100.0f : 0.0f;
}

// ═══════════════════════════════════════════════════════════════════════════
// Tray icon
// ═══════════════════════════════════════════════════════════════════════════
void AddTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Load embedded icon (resource ID 1), fallback to default if not found
    g_nid.hIcon            = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
    if (!g_nid.hIcon)
        g_nid.hIcon        = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpy(g_nid.szTip, "justFPS");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() { Shell_NotifyIcon(NIM_DELETE, &g_nid); }

void UpdateTrayTooltip()
{
    if (g_updateAvailable) {
        snprintf(g_nid.szTip, sizeof(g_nid.szTip), "justFPS - Update available! (%s)", g_latestVersion);
    } else {
        lstrcpy(g_nid.szTip, "justFPS");
    }
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

// ═══════════════════════════════════════════════════════════════════════════
// ImGui style
// ═══════════════════════════════════════════════════════════════════════════
void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(14, 10);
    s.FramePadding  = ImVec2(8, 5);
    s.ItemSpacing   = ImVec2(10, 8);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f,0.08f,0.10f,1);
    c[ImGuiCol_Border]           = ImVec4(0.25f,0.27f,0.32f,0.6f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.14f,0.14f,0.17f,1);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f,0.20f,0.24f,1);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.26f,0.26f,0.30f,1);
    c[ImGuiCol_CheckMark]        = ImVec4(0.30f,0.75f,1.00f,1);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.30f,0.75f,1.00f,1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f,0.85f,1.00f,1);
    c[ImGuiCol_Button]           = ImVec4(0.16f,0.16f,0.20f,1);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.22f,0.22f,0.28f,1);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.28f,0.28f,0.34f,1);
    c[ImGuiCol_Separator]        = ImVec4(0.22f,0.24f,0.28f,1);
}

// ── UI helpers for settings deck ──
static void DrawSectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1), label);
    ImGui::Spacing();
}

static void BeginCard() {
    ImGui::BeginGroup();
}

static void EndCard() {
    ImGui::EndGroup();
    ImGui::Separator();
    ImGui::Spacing();
}

// Draws a checkbox toggle that auto-detects Custom preset on change
static bool DrawStatToggle(const char* label, bool* value, bool showUnavailable = false) {
    bool changed = ImGui::Checkbox(label, value);
    if (changed) {
        extern OverlayConfig g_Config;
        extern OverlayPreset DetectPreset(const OverlayConfig&);
        extern void SyncLegacyDisplayFlags(OverlayConfig&);
        g_Config.preset = DetectPreset(g_Config);
        SyncLegacyDisplayFlags(g_Config);
    }
    if (showUnavailable) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.2f, 1), "(unavailable)");
    }
    return changed;
}

// Draws a small inline status chip
static void DrawStatusChip(const char* label, bool available) {
    ImGui::SameLine();
    if (available) {
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1), "  %s", label);
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.50f, 1), "  %s", label);
    }
}

#include <string>

// ═══════════════════════════════════════════════════════════════════════════
// DX11
// ═══════════════════════════════════════════════════════════════════════════
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dDeviceContext)))
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* buf = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buf));
    if (buf) { g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_pRenderTargetView); buf->Release(); }
}

void CleanupRenderTarget()
{
    if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════════════
// Backend / mode helpers
// ═══════════════════════════════════════════════════════════════════════════
void ShutdownBackends()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    CleanupDeviceD3D();
}

void InitBackends()
{
    CreateDeviceD3D(g_hwnd);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
}

// Toggle click-through mode on the overlay window
static void SetClickThrough(bool enable)
{
    LONG_PTR style = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    if (enable)
        style |= WS_EX_TRANSPARENT;
    else
        style &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, style);
}

void SwitchToOverlay()
{
    ShutdownBackends();
    DestroyWindow(g_hwnd);

    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    
    // Always start click-through - we toggle it when CTRL is held
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    
    g_hwnd = CreateWindowEx(
        exStyle,
        "justFPS", "justFPS", WS_POPUP,
        work.left, work.top,
        work.right - work.left, work.bottom - work.top,
        nullptr, nullptr, g_hInstance, nullptr);

    SetLayeredWindowAttributes(g_hwnd, RGB(0,0,0), 255, LWA_ALPHA);
    MARGINS m = { -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &m);
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));

    InitBackends();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    // Start ETW in background to avoid blocking overlay startup
    g_etwStartupThread = std::thread([]() { g_etwAvailable = StartEtwSession(); });

    g_Mode       = MODE_OVERLAY;
    g_OvlVisible = true;
    g_overlaySettingsOpen = false;
}

void SwitchToConfig()
{
    StopEtwSession();
    RemoveTrayIcon();
    ShutdownBackends();
    DestroyWindow(g_hwnd);

    int cw = 420, ch = 730;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, "justFPS", "justFPS",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        cx, cy, cw, ch, nullptr, nullptr, g_hInstance, nullptr);

    LONG_PTR exStyle = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
    SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);

    InitBackends();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    g_Mode = MODE_CONFIG;
    g_overlaySettingsOpen = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Rendering helpers
// ═══════════════════════════════════════════════════════════════════════════
static void DrawFrequency(ImFont* valueFont, const ImVec4& color, float currentMhz, float maxMhz) {
    if (currentMhz <= 0.0f) return;
    float currentGhz = currentMhz / 1000.0f;
    if (maxMhz > 0.0f) {
        float maxGhz = maxMhz / 1000.0f;
        if (valueFont) ImGui::PushFont(valueFont);
        ImGui::TextColored(color, "%.2f/%.2f GHz", currentGhz, maxGhz);
        if (valueFont) ImGui::PopFont();
    } else {
        if (valueFont) ImGui::PushFont(valueFont);
        ImGui::TextColored(color, "%.2f GHz", currentGhz);
        if (valueFont) ImGui::PopFont();
    }
}

static void Present(float r, float g, float b, float a)
{
    ImGui::Render();
    const float c[4] = { r, g, b, a };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTargetView, c);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(0, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// WinMain
// ═══════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInstance = hInst;

    // Harden DLL search path — prevents hijacking from current directory
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    SetDllDirectoryA("");

    g_singleInstanceMutex = CreateMutexA(nullptr, TRUE, "justFPS.SingleInstance");
    if (!g_singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ── Load saved configuration ──
    LoadConfig(g_Config);
    bool isFirstBoot = (GetFileAttributesA(g_configPath) == INVALID_FILE_ATTRIBUTES);

    // ── Check for updates in background ──
    CheckForUpdatesAsync();

    // ── Show welcome message on first run ──
    ShowWelcomeMessage();

    // ── Query hardware ──
    QueryCpuName();

    // ── Register window class with icon ──
    HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    if (!hIcon) hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.lpszClassName = "justFPS";
    RegisterClassEx(&wc);

    // ── Config window ──
    int cw = 420, ch = 730;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, "justFPS",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        cx, cy, cw, ch, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        return 1;
    }

    // ── Check admin privileges (app should always run as admin via manifest) ──
    g_isAdmin = IsRunningAsAdmin();

    if (!CreateDeviceD3D(g_hwnd)) {
        MessageBox(g_hwnd, "DirectX 11 initialisation failed.", "justFPS", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        return 1;
    }

    // Get GPU name from DXGI adapter (fallback if LHWM doesn't provide it)
    QueryGpuName();

    // Check if PawnIO driver is installed (required for temperature access on modern systems)
    // This check happens before LHWM init so user can install it first
    if (!IsPawnIOInstalled()) {
        if (PromptAndInstallPawnIO()) {
            // Driver was installed, continue with initialization
        }
        // If user declined, we still try to initialize - some features may work
    }
    
    // Initialize hardware monitoring asynchronously (don't block startup)
    g_lhwmInitThread = std::thread([] {
        bool lhwmOk = InitLHWM();
        if (lhwmOk && !g_lhwmCpuTempPath.empty()) {
            g_cpuTempAvailable = true;
        } else {
            g_cpuTempAvailable = InitWMI();
            if (g_cpuTempAvailable) {
                float testTemp = QueryCpuTemperature();
                g_cpuTempAvailable = (testTemp > 0.0f && testTemp < 150.0f);
            }
        }
        g_lhwmAvailable.store(lhwmOk, std::memory_order_release);
    });

    // Show config window (unless auto-start is enabled)
    if (!g_Config.autoStart) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    // ── ImGui context (lives for the whole app) ──
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;

    g_baseFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 22.0f);
    g_overlayValueFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 22.0f);
    g_overlayLabelFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 22.0f);
    if (!g_baseFont) {
        io.Fonts->Clear();
        ImFontConfig fc;
        fc.SizePixels = 20;
        g_baseFont = io.Fonts->AddFontDefault(&fc);
        g_overlayValueFont = nullptr;
        g_overlayLabelFont = nullptr;
    }
    if (!g_overlayValueFont) g_overlayValueFont = g_baseFont;
    if (!g_overlayLabelFont) g_overlayLabelFont = g_overlayValueFont;

    ApplyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ── Timing ──
    using Clock = std::chrono::high_resolution_clock;
    auto lastCpuTime = Clock::now();
    auto lastGpuTime = lastCpuTime;
    auto lastRamTime = lastCpuTime;
    float cpuUsage = 0;
    GetCpuUsage(); // seed

    // ── Auto-start overlay if enabled ──
    if (g_Config.autoStart) {
        g_Pending = CMD_START_OVERLAY;
    }

    // ── Main loop ──
    MSG msg = {};
    while (g_Running)
    {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;

        if (g_Pending == CMD_START_OVERLAY) { g_Pending = CMD_NONE; SwitchToOverlay(); if (isFirstBoot) { g_overlaySettingsOpen = true; g_overlaySettingsJustOpened = true; } }
        if (g_Pending == CMD_SHOW_SETTINGS) {
            g_Pending = CMD_NONE;
            if (g_Mode == MODE_OVERLAY) {
                g_OvlVisible = true;
                g_overlaySettingsOpen = true;
                g_overlaySettingsJustOpened = true;
            } else {
                SwitchToConfig();
            }
        }
        if (g_Pending == CMD_EXIT)          { g_Running = false; break; }

        // ── Hotkey listener for rebind UI ──
        if (g_listeningFor != 0) {
            if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                g_listeningFor = 0;
            } else {
                for (int vk = 1; vk < 256; vk++) {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    if (vk == VK_ESCAPE) continue;
                    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
                    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;

                    if (GetAsyncKeyState(vk) & 1) {
                        int mod = HeldMods();
                        if (g_listeningFor == 1) { g_Config.toggleKey = vk; g_Config.toggleMod = mod; }
                        if (g_listeningFor == 2) { g_Config.settingsKey = vk; g_Config.settingsMod = mod; }
                        g_listeningFor = 0;
                        break;
                    }
                }
            }
        }

        // ══════════════════════════════════════════════════════════════
        // CONFIG MODE
        // ══════════════════════════════════════════════════════════════
        if (g_Mode == MODE_CONFIG)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("##cfg", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

            // ── Title bar ──
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored(ImVec4(.35f,.78f,1,1), "justFPS");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine(); ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), " %s", APP_VERSION);
            
            // Show update available notification
            if (g_updateAvailable) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.2f,.9f,.4f,1), " -");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.2f,.9f,.4f,1));
                if (ImGui::SmallButton("Update available!")) {
                    ShellExecuteA(nullptr, "open", 
                        "https://github.com/nathwn12/just-fps/releases/latest",
                        nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to download %s", g_latestVersion);
                }
            }

            // Developer text
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), " justFPS");

            ImGui::Spacing(); ImGui::Separator();

            // ── PRESET ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "PRESET");
            ImGui::Spacing();
            if (ImGui::BeginCombo("##preset", GetPresetLabel(g_Config.preset))) {
                for (int i = (int)OverlayPreset::JustFPS; i <= (int)OverlayPreset::Custom; ++i) {
                    OverlayPreset preset = (OverlayPreset)i;
                    bool selected = g_Config.preset == preset;
                    if (ImGui::Selectable(GetPresetLabel(preset), selected)) {
                        ApplyPreset(g_Config, preset);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing(); ImGui::Separator();
            ImGui::Spacing();

            // ── DISPLAY ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "DISPLAY");
            ImGui::Spacing();
            if (ImGui::Checkbox("  FPS Counter (game)", &g_Config.showFPS)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (!g_isAdmin) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "(needs admin!)");
            }
            if (ImGui::Checkbox("  CPU Utilization", &g_Config.showCpuUtil)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  CPU Temperature", &g_Config.showCpuTemp)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  GPU Utilization", &g_Config.showGpuUtil)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (!g_lhwmAvailable || g_gpuCount == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "(unavailable)");
            }
            if (ImGui::Checkbox("  GPU Temperature", &g_Config.showGpuTemp)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (!g_lhwmAvailable || g_gpuCount == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "(unavailable)");
            }
            if (ImGui::Checkbox("  GPU Hotspot Temp", &g_Config.showGpuHotspot)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (!g_lhwmAvailable || g_gpuCount == 0 || g_lhwmGpuHotspotPath.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "(unsupported)");
            }
            if (ImGui::Checkbox("  GPU Power", &g_Config.showGpuPower)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  GPU Fan", &g_Config.showGpuFan)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  Show FPS lows/highs", &g_Config.showFpsLowHigh)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  Show CPU frequency", &g_Config.showCpuFrequency)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  Show GPU frequency", &g_Config.showGpuFrequency)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (ImGui::Checkbox("  GPU VRAM Usage", &g_Config.showVRAM)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }
            if (!g_lhwmAvailable || g_gpuCount == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "(unavailable)");
            }
            if (ImGui::Checkbox("  RAM Usage", &g_Config.showRAM)) {
                g_Config.preset = DetectPreset(g_Config);
                SyncLegacyDisplayFlags(g_Config);
            }

            // ── GPU SELECTION ──
            if (g_gpuCount > 0) {
                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "GPU SELECTION");
                ImGui::Spacing();
                
                const char* previewName = (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) 
                    ? g_gpuList[g_Config.selectedGpu].name 
                    : "Select GPU...";
                
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##gpuselect", previewName)) {
                    for (int i = 0; i < g_gpuCount; i++) {
                        bool isSelected = (g_Config.selectedGpu == i);
                        if (ImGui::Selectable(g_gpuList[i].name, isSelected)) {
                            SelectGpu(i);
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                
                if (g_gpuCount > 1) {
                    ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Multiple GPUs detected - select which to monitor");
                }
            }

            // ── POSITION ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "POSITION");
            ImGui::Spacing();
            ImGui::RadioButton("Top Left",      &g_Config.position, 0); ImGui::SameLine(0,18);
            ImGui::RadioButton("Top Middle",    &g_Config.position, 4); ImGui::SameLine(0,18);
            ImGui::RadioButton("Top Right",     &g_Config.position, 1);
            ImGui::RadioButton("Bottom Left",   &g_Config.position, 2); ImGui::SameLine(0,18);
            ImGui::RadioButton("Bottom Middle", &g_Config.position, 5); ImGui::SameLine(0,18);
            ImGui::RadioButton("Bottom Right",  &g_Config.position, 3);
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Hold CTRL and right-click overlay for menu");

            // ── LAYOUT ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "LAYOUT");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Overlay UI Size");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##uiscale", &g_Config.uiScale, 0.75f, 2.25f, "");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Scales only the in-game overlay");
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Text Saturation");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##textsat", &g_Config.textSaturation, 0.00f, 1.40f, "");
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Edge Padding");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##edgepad", &g_Config.edgePadding, 0.0f, 100.0f, "%.0f px");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Distance from the screen edge (0-100 px)");
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Background Opacity");
            ImGui::SetNextItemWidth(-1);
            float bgPct = g_Config.hudBgAlpha * 100.0f;
            ImGui::SliderFloat("##hudbg", &bgPct, 0.0f, 100.0f, "%.0f%%");
            g_Config.hudBgAlpha = bgPct / 100.0f;

            // ── TEMPERATURE UNIT ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "TEMPERATURE");
            ImGui::Spacing();
            int tempUnit = g_Config.useFahrenheit ? 1 : 0;
            if (ImGui::RadioButton("Celsius", &tempUnit, 0)) g_Config.useFahrenheit = false;
            ImGui::SameLine(0,24);
            if (ImGui::RadioButton("Fahrenheit", &tempUnit, 1)) g_Config.useFahrenheit = true;

            // ── HOTKEYS ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "HOTKEYS");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Leave unbound until you choose your own keys.");
            ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Set one for Show/Hide FPS Overlay and one for Show/Hide Settings UI.");
            ImGui::Spacing();

            // Toggle key
            ImGui::Text("Overlay:");
            ImGui::SameLine(90);
            if (g_listeningFor == 1) {
                ImGui::TextColored(ImVec4(1,.8f,.2f,1), "Press any key...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##1")) g_listeningFor = 0;
            } else {
                char buf[64];
                FormatKeyBinding(buf, sizeof(buf), g_Config.toggleKey, g_Config.toggleMod);
                ImGui::Text("%s", buf);
                ImGui::SameLine();
                if (ImGui::SmallButton("Change##1")) g_listeningFor = 1;
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##1")) { g_Config.toggleKey = 0; g_Config.toggleMod = 0; }
            }

            // Settings key
            ImGui::Text("Settings UI:");
            ImGui::SameLine(90);
            if (g_listeningFor == 2) {
                ImGui::TextColored(ImVec4(1,.8f,.2f,1), "Press any key...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##2")) g_listeningFor = 0;
            } else {
                char buf[64];
                FormatKeyBinding(buf, sizeof(buf), g_Config.settingsKey, g_Config.settingsMod);
                ImGui::Text("%s", buf);
                ImGui::SameLine();
                if (ImGui::SmallButton("Change##2")) g_listeningFor = 2;
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##2")) { g_Config.settingsKey = 0; g_Config.settingsMod = 0; }
            }

            // ── STARTUP ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "STARTUP");
            ImGui::Spacing();
            ImGui::Checkbox("  Start overlay immediately", &g_Config.autoStart);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skip this window and start the overlay directly next time");
            // ── HARDWARE ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "DETECTED HARDWARE");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "CPU:  %s", g_cpuName);
            ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "GPU:  %s", g_gpuName);

            // ── BUTTONS ──
            ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
            if (ImGui::Button("Apply Settings", ImVec2(ImGui::GetContentRegionAvail().x, 42)))
                SaveConfig(g_Config);
            ImGui::Spacing();
            if (ImGui::Button("Start Overlay", ImVec2(ImGui::GetContentRegionAvail().x, 42)))
                { SaveConfig(g_Config); g_Pending = CMD_START_OVERLAY; }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.60f,.12f,.12f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(.72f,.16f,.16f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(.50f,.10f,.10f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8);
            if (ImGui::Button("Exit", ImVec2(ImGui::GetContentRegionAvail().x, 42)))
                g_Pending = CMD_EXIT;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::End();

            // Config mode pacing: respond to input quickly but don't burn GPU
            static auto lastCfgFrame = std::chrono::steady_clock::now();
            auto nowCfg = std::chrono::steady_clock::now();
            auto cfgElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(nowCfg - lastCfgFrame).count();
            if (cfgElapsed < 16) {
                Sleep(16 - (DWORD)cfgElapsed);
            }
            lastCfgFrame = std::chrono::steady_clock::now();

            Present(0.08f, 0.08f, 0.10f, 1);
        }

        // ══════════════════════════════════════════════════════════════
        // OVERLAY MODE
        // ══════════════════════════════════════════════════════════════
        else
        {
            // ── Hotkeys (user-configurable) ──
            for (int vk = 1; vk < 256; vk++) {
                if (!(GetAsyncKeyState(vk) & 1)) continue;
                if (vk == g_Config.toggleKey   && ModsDown(g_Config.toggleMod))
                    { g_OvlVisible = !g_OvlVisible; break; }
                if (vk == g_Config.settingsKey && ModsDown(g_Config.settingsMod))
                    { g_Pending = CMD_SHOW_SETTINGS; break; }
            }

            // ── Show/Hide window via DWM cloak (faster than alpha toggling) ──
            static bool wasCloaked = true;
            bool cloak = !g_OvlVisible;
            if (cloak != wasCloaked) {
                DwmSetWindowAttribute(g_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                wasCloaked = cloak;
            }

            if (!g_OvlVisible) {
                Sleep(100);  // ~10 Hz polling while hidden — no rendering at all
                continue;
            }

            // ── Update target PID (foreground window's process) ──
            HWND fg = GetForegroundWindow();
            DWORD currentPid = g_lastTargetPid;
            if (fg && fg != g_hwnd) {
                GetWindowThreadProcessId(fg, &currentPid);
                g_targetPid.store(currentPid, std::memory_order_relaxed);
            }
            auto now = Clock::now();
            
            // ── Reset FPS when target app changes or closes ──
            if (currentPid != g_lastTargetPid) {
                g_gameFps.store(0.0f, std::memory_order_relaxed);
                g_lastTargetPid = currentPid;
                g_fpsRangeInitialized = false;
                g_fpsLow = 0.0f;
                g_fpsHigh = 0.0f;
                // Update process name
                GetProcessName(currentPid, g_targetProcessName, sizeof(g_targetProcessName));
            }
            // Also check if the process is still alive
            static auto lastPidAliveCheck = Clock::now();
            if (g_lastTargetPid != 0 &&
                std::chrono::duration<float>(now - lastPidAliveCheck).count() >= 1.0f) {
                lastPidAliveCheck = now;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_lastTargetPid);
                if (hProc) {
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(hProc, &exitCode) && exitCode != STILL_ACTIVE) {
                        g_gameFps.store(0.0f, std::memory_order_relaxed);
                        g_lastTargetPid = 0;
                        g_fpsRangeInitialized = false;
                        g_fpsLow = 0.0f;
                        g_fpsHigh = 0.0f;
                    }
                    CloseHandle(hProc);
                } else {
                    // Process no longer exists
                    g_gameFps.store(0.0f, std::memory_order_relaxed);
                    g_lastTargetPid = 0;
                    g_fpsRangeInitialized = false;
                    g_fpsLow = 0.0f;
                    g_fpsHigh = 0.0f;
                }
            }

            // ── Periodic metrics (once/sec) ──
            static float cachedRamUsed = 0, cachedRamTotal = 1;

            {
                float cpuElapsed = std::chrono::duration<float>(now - lastCpuTime).count();
                if (cpuElapsed >= 1.0f) {
                    cpuUsage = GetCpuUsage();
                    // Poll CPU temp - prefer LHWM over WMI
                    if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty()) {
                        g_cpuTemp = g_lhwmCpuTemp;
                    } else if (g_cpuTempAvailable) {
                        g_cpuTemp = QueryCpuTemperature();
                    }
                    lastCpuTime = now;
                }

                float gpuElapsed = std::chrono::duration<float>(now - lastGpuTime).count();
                if (gpuElapsed >= 1.0f) {
                    // Poll LHWM first (covers AMD, Intel, NVIDIA)
                    if (g_lhwmAvailable) {
                        PollLHWMStats();
                    }
                    lastGpuTime = now;
                    
                    // Update tray tooltip if update check completed
                    static bool tooltipUpdated = false;
                    if (g_updateCheckDone && !tooltipUpdated) {
                        UpdateTrayTooltip();
                        tooltipUpdated = true;
                    }
                }

                float ramElapsed = std::chrono::duration<float>(now - lastRamTime).count();
                if (ramElapsed >= 1.0f) {
                    MEMORYSTATUSEX mem = {}; mem.dwLength = sizeof(mem);
                    GlobalMemoryStatusEx(&mem);
                    cachedRamUsed  = (float)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.f*1024.f*1024.f);
                    cachedRamTotal = (float)(mem.ullTotalPhys)                    / (1024.f*1024.f*1024.f);
                    lastRamTime = now;
                }
            }
            
            // Use cached values
            float ramUsed = cachedRamUsed;
            float ramTotal = cachedRamTotal;

            // ── Game FPS (from ETW) ──
            float gameFps = g_gameFps.load(std::memory_order_relaxed);

            // ── Track FPS low/high ──
            if (gameFps > 0.0f) {
                if (!g_fpsRangeInitialized) {
                    g_fpsLow = gameFps;
                    g_fpsHigh = gameFps;
                    g_fpsRangeInitialized = true;
                } else {
                    if (gameFps < g_fpsLow) g_fpsLow = gameFps;
                    if (gameFps > g_fpsHigh) g_fpsHigh = gameFps;
                }
            }

            // ── Handle CTRL key for right-click menu ──
            // Only respond to CTRL when cursor is hovering over the overlay
            POINT cursorPt; GetCursorPos(&cursorPt);
            bool cursorOverOverlay = PtInRect(&g_overlayBounds, cursorPt);
            bool ctrlKeyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

            // interactionActive controls click-through and window flags.
            static bool interactionActive = false;
            
            // Enter interaction mode: Ctrl held AND cursor over overlay
            if (ctrlKeyDown && cursorOverOverlay) {
                interactionActive = true;
            }
            // Exit interaction mode: Ctrl released, or cursor left overlay.
            else if (!ctrlKeyDown) {
                interactionActive = false;
            }
            else if (!cursorOverOverlay) {
                interactionActive = false;
            }
            
            bool ctrlHeld = interactionActive;

            // Manage click-through state
            bool wantsInput = ctrlHeld || g_overlaySettingsOpen;
            static bool clickThroughEnabled = true;
            if (wantsInput && clickThroughEnabled) {
                SetClickThrough(false);
                clickThroughEnabled = false;
            } else if (!wantsInput && !clickThroughEnabled) {
                SetClickThrough(true);
                clickThroughEnabled = true;
            }
            
            // Right-click context menu (when CTRL is held AND right-click happens IN interaction mode)
            // Track right mouse button state to detect fresh clicks (not clicks from elsewhere)
            static bool rightMouseWasDown = false;
            bool rightMouseDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool rightMouseJustPressed = rightMouseDown && !rightMouseWasDown;
            rightMouseWasDown = rightMouseDown;
            
            // Only show menu if right-click happened while already in interaction mode
            if (ctrlHeld && rightMouseJustPressed) {
                HMENU m = CreatePopupMenu();
                AppendMenu(m, MF_STRING, IDM_SETTINGS, "Settings");
                AppendMenu(m, MF_STRING, IDM_EXIT, "Exit");
                SetForegroundWindow(g_hwnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         cursorPt.x, cursorPt.y, 0, g_hwnd, nullptr);
                DestroyMenu(m);
                PostMessage(g_hwnd, WM_NULL, 0, 0);
                switch (cmd) {
                    case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
                    case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }

            // ── ImGui frame ──
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Position: always use the selected fixed preset.
            float margin = g_Config.edgePadding * g_Config.uiScale;
            MONITORINFO mi = { sizeof(mi) };
            HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
            GetMonitorInfo(mon, &mi);
            float left = (float)mi.rcWork.left;
            float top = (float)mi.rcWork.top;
            float right = (float)mi.rcWork.right;
            float bottom = (float)mi.rcWork.bottom;
            ImVec2 pos, pivot = {0, 0};
            // A second pass after drawing corrects for auto-resized content.
            switch (g_Config.position) {
                default:
                case 0: pos={left+margin,top+margin};                 pivot={0,0}; break;
                case 1: pos={right-margin,top+margin};                pivot={1,0}; break;
                case 2: pos={left+margin,bottom-margin};              pivot={0,1}; break;
                case 3: pos={right-margin,bottom-margin};             pivot={1,1}; break;
                case 4: pos={(left+right)*0.5f,top+margin};           pivot={0.5f,0}; break;
                case 5: pos={(left+right)*0.5f,bottom-margin};        pivot={0.5f,1}; break;
            }

            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

            // Window flags - fixed-position overlay; CTRL only enables the context menu.
            ImGuiWindowFlags wf =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove;
            
            if (!ctrlHeld && !g_overlaySettingsOpen) {
                wf |= ImGuiWindowFlags_NoInputs;
            }

            ImGui::Begin("##ovl", nullptr, wf);

            // ── Pill/Group background (0% = completely gone) ──
            if (g_Config.hudBgAlpha > 0.0f) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wMin = ImGui::GetWindowPos();
                ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowSize().x, wMin.y + ImGui::GetWindowSize().y);
                int alpha = (int)(180.0f * g_Config.hudBgAlpha);
                ImU32 bgCol = IM_COL32(8, 8, 10, alpha);
                ImU32 borderCol = IM_COL32(40, 44, 52, (int)(120.0f * g_Config.hudBgAlpha));
                float radius = 8.0f;
                dl->AddRectFilled(ImVec2(wMin.x-6, wMin.y-6), ImVec2(wMax.x+6, wMax.y+6), bgCol, radius+2);
                dl->AddRect(ImVec2(wMin.x-6, wMin.y-6), ImVec2(wMax.x+6, wMax.y+6), borderCol, radius, 0, 1.0f);
            }

            ImGui::SetWindowFontScale(g_Config.uiScale);

            // Steam-like colors: muted colored labels, soft gray values.
            const float textSat = g_Config.textSaturation;
            const ImVec4 fpsCol = (g_Config.preset == OverlayPreset::JustFPS)
                ? ImVec4(0.20f, 0.76f, 0.28f, 1.0f)
                : AdjustTextSaturation(ImVec4(0.93f, 0.50f, 0.50f, 1.0f), textSat);
            const ImVec4 cpuCol = AdjustTextSaturation(ImVec4(0.84f, 0.88f, 0.33f, 1.0f), textSat); // Yellow-green
            const ImVec4 gpuCol = AdjustTextSaturation(ImVec4(0.20f, 0.76f, 0.28f, 1.0f), textSat); // Steam green
            const ImVec4 memCol = AdjustTextSaturation(ImVec4(0.78f, 0.55f, 0.96f, 1.0f), textSat); // Lavender
            const ImVec4 valCol = AdjustNeutralTextContrast(ImVec4(0.80f, 0.80f, 0.82f, 1.0f), textSat); // Values
            const ImVec4 dimCol = AdjustNeutralTextContrast(ImVec4(0.45f, 0.45f, 0.48f, 1.0f), textSat); // Unavailable
            auto TextColoredFont = [](ImFont* font, const ImVec4& color, const char* fmt, ...) {
                if (font) ImGui::PushFont(font);
                va_list args;
                va_start(args, fmt);
                ImGui::TextColoredV(color, fmt, args);
                va_end(args);
                if (font) ImGui::PopFont();
            };

            auto DrawFpsRow = [&](const ImVec4& valueColor, float fpsValue, float lowValue, float highValue, bool showRange) {
                TextColoredFont(g_overlayLabelFont, fpsCol, "FPS");
                ImGui::SameLine(0, 4);
                TextColoredFont(g_overlayValueFont, valueColor, "%.0f", fpsValue);
                if (showRange) {
                    ImGui::SameLine(0, 6);
                    TextColoredFont(g_overlayValueFont, valueColor, "\xE2\x86\x93%.0f", lowValue);
                    ImGui::SameLine(0, 6);
                    TextColoredFont(g_overlayValueFont, valueColor, "\xE2\x86\x91%.0f", highValue);
                }
            };
            
            // ── Draw glowing border when CTRL is held ──
            if (ctrlHeld) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wMin = ImGui::GetWindowPos();
                ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowSize().x, wMin.y + ImGui::GetWindowSize().y);
                
                // Animated glow effect (pulsing)
                float t = (float)fmod(ImGui::GetTime() * 2.0, 3.14159 * 2.0);
                float glow = 0.6f + 0.4f * sinf(t);
                
                // Draw multiple borders for glow effect (outer to inner)
                ImU32 glowColor1 = IM_COL32(80, 180, 255, (int)(40 * glow));
                ImU32 glowColor2 = IM_COL32(80, 180, 255, (int)(80 * glow));
                ImU32 glowColor3 = IM_COL32(100, 200, 255, (int)(160 * glow));
                ImU32 coreColor  = IM_COL32(120, 220, 255, (int)(255 * glow));
                
                dl->AddRect(ImVec2(wMin.x - 4, wMin.y - 4), ImVec2(wMax.x + 4, wMax.y + 4), glowColor1, 8.0f, 0, 3.0f);
                dl->AddRect(ImVec2(wMin.x - 2, wMin.y - 2), ImVec2(wMax.x + 2, wMax.y + 2), glowColor2, 6.0f, 0, 2.0f);
                dl->AddRect(ImVec2(wMin.x - 1, wMin.y - 1), ImVec2(wMax.x + 1, wMax.y + 1), glowColor3, 4.0f, 0, 1.5f);
                dl->AddRect(wMin, wMax, coreColor, 4.0f, 0, 1.0f);
            }

            // ═══════════════════════════════════════════════════════════
            // STATS (Steam-style horizontal)
            // ═══════════════════════════════════════════════════════════
            {
                bool needSep = false;

                // FPS
                if (g_Config.showFPS) {
                    if (needSep) ImGui::SameLine(0, 10);
                    if (g_etwAvailable && gameFps > 0) {
                        DrawFpsRow(valCol, gameFps, g_fpsLow, g_fpsHigh, g_Config.showFpsLowHigh && g_fpsRangeInitialized);
                    } else {
                        DrawFpsRow(dimCol, 0.0f, 0.0f, 0.0f, g_Config.showFpsLowHigh);
                    }
                    needSep = true;
                }

                // CPU
                if (g_Config.showCpuUtil || g_Config.showCpuTemp || g_Config.showCpuFrequency) {
                    if (needSep) ImGui::SameLine(0, 12);
                    TextColoredFont(g_overlayLabelFont, cpuCol, "CPU");
                    ImGui::SameLine(0, 4);
                    if (g_Config.showCpuUtil) {
                        TextColoredFont(g_overlayValueFont, valCol, "%.0f%%", cpuUsage);
                    }
                    if (g_Config.showCpuTemp && g_cpuTempAvailable && g_cpuTemp > 0) {
                        ImGui::SameLine(0, 6);
                        float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                        TextColoredFont(g_overlayValueFont, valCol, "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                    }
                    if (g_Config.showCpuFrequency && g_lhwmAvailable && g_cpuClockMhz > 0.0f) {
                        ImGui::SameLine(0, 8);
                        float maxMhz = (g_cpuClockMaxMhz > 0.0f) ? g_cpuClockMaxMhz : g_cpuClockMaxObserved;
                        DrawFrequency(g_overlayValueFont, valCol, g_cpuClockMhz, maxMhz);
                    }
                    needSep = true;
                }

                // GPU
                if (g_Config.showGpuUtil || g_Config.showGpuTemp || g_Config.showGpuHotspot || g_Config.showVRAM || g_Config.showGpuPower || g_Config.showGpuFan) {
                    if (needSep) ImGui::SameLine(0, 12);

                    float hasGpuData = g_lhwmAvailable && g_gpuCount > 0;

                    if (hasGpuData) {
                        bool showedGpuLabel = false;
                        // GPU util (with label)
                        if (g_Config.showGpuUtil) {
                            TextColoredFont(g_overlayLabelFont, gpuCol, "GPU");
                            showedGpuLabel = true;
                            ImGui::SameLine(0, 4);
                            TextColoredFont(g_overlayValueFont, valCol, "%.0f%%", g_gpuUsage);
                        }
                        // GPU temp (label only if not shown yet)
                        if (g_Config.showGpuTemp && g_gpuTemp > 0) {
                            if (!showedGpuLabel) {
                                TextColoredFont(g_overlayLabelFont, gpuCol, "GPU");
                                showedGpuLabel = true;
                                ImGui::SameLine(0, 4);
                            }
                            float dispTemp = ToDisplayTemp(g_gpuTemp, g_Config.useFahrenheit);
                            ImGui::SameLine(0, 6);
                            TextColoredFont(g_overlayValueFont, valCol, "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                        }
                        // GPU hotspot
                        if (g_Config.showGpuHotspot && !g_lhwmGpuHotspotPath.empty() && g_gpuHotspotTemp > 0) {
                            float dispHotspot = ToDisplayTemp(g_gpuHotspotTemp, g_Config.useFahrenheit);
                            ImGui::SameLine(0, 6);
                            TextColoredFont(g_overlayValueFont, valCol, "%.0f\xC2\xB0%s", dispHotspot, g_Config.useFahrenheit ? "F" : "C");
                        }
                        // VRAM
                        if (g_Config.showVRAM) {
                            float dispVramUsed = g_vramUsed;
                            float dispVramTotal = g_vramTotal;
                            if (dispVramTotal > 0) {
                                ImGui::SameLine(0, 8);
                                TextColoredFont(g_overlayValueFont, valCol, "%.1f/%.0fG", dispVramUsed, dispVramTotal);
                            }
                        }
                        // GPU frequency
                        if (g_Config.showGpuFrequency && g_gpuClockMhz > 0.0f) {
                            ImGui::SameLine(0, 8);
                            DrawFrequency(g_overlayValueFont, valCol, g_gpuClockMhz, g_gpuClockMaxMhz);
                        }
                        // GPU power
                        if (g_Config.showGpuPower && g_gpuPower > 0.0f) {
                            ImGui::SameLine(0, 8);
                            TextColoredFont(g_overlayValueFont, valCol, "%.0fW", g_gpuPower);
                        }
                        // GPU fan
                        if (g_Config.showGpuFan && g_gpuFan > 0.0f) {
                            ImGui::SameLine(0, 8);
                            TextColoredFont(g_overlayValueFont, valCol, "%.0f RPM", g_gpuFan);
                        }
                    } else {
                        TextColoredFont(g_overlayValueFont, dimCol, "GPU N/A");
                    }
                    needSep = true;
                }

                // RAM
                if (g_Config.showRAM) {
                    if (needSep) ImGui::SameLine(0, 12);
                    TextColoredFont(g_overlayLabelFont, memCol, "RAM");
                    ImGui::SameLine(0, 4);
                    TextColoredFont(g_overlayValueFont, valCol, "%.1f/%.1f GB", ramUsed, ramTotal);
                    needSep = true;
                }
            }
            
            // ── Show helper text when CTRL is held ──
            if (ctrlHeld) {
                ImGui::Spacing();
                ImGui::SetWindowFontScale(0.85f * g_Config.uiScale);
                ImGui::TextColored(ImVec4(0.5f, 0.75f, 1.0f, 1.0f), "Right-click for menu");
                ImGui::SetWindowFontScale(g_Config.uiScale);
            }

            if (g_overlaySettingsOpen) {
                ImGui::SetNextWindowSize(ImVec2(400, 520), ImGuiCond_FirstUseEver);
                if (g_overlaySettingsJustOpened) {
                    ImGuiViewport* vp = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    g_overlaySettingsJustOpened = false;
                }
                if (ImGui::Begin("Settings", &g_overlaySettingsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
                    // ── Overlay settings title ──
                    ImGui::TextColored(ImVec4(.35f,.78f,1,1), "justFPS Settings");
                    ImGui::Separator();

                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "PRESET");
                    ImGui::Spacing();
                    if (ImGui::BeginCombo("##ovl_preset", GetPresetLabel(g_Config.preset))) {
                        for (int i = (int)OverlayPreset::JustFPS; i <= (int)OverlayPreset::Custom; ++i) {
                            OverlayPreset preset = (OverlayPreset)i;
                            bool selected = g_Config.preset == preset;
                            if (ImGui::Selectable(GetPresetLabel(preset), selected)) {
                                ApplyPreset(g_Config, preset);
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::Spacing(); ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "DISPLAY");
                    if (ImGui::Checkbox("FPS Counter (game)", &g_Config.showFPS)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("CPU Utilization", &g_Config.showCpuUtil)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("CPU Temperature", &g_Config.showCpuTemp)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU Utilization", &g_Config.showGpuUtil)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU Temperature", &g_Config.showGpuTemp)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU Hotspot Temp", &g_Config.showGpuHotspot)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU Power", &g_Config.showGpuPower)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU Fan", &g_Config.showGpuFan)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("Show FPS lows/highs", &g_Config.showFpsLowHigh)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("Show CPU frequency", &g_Config.showCpuFrequency)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("Show GPU frequency", &g_Config.showGpuFrequency)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("GPU VRAM Usage", &g_Config.showVRAM)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }
                    if (ImGui::Checkbox("RAM Usage", &g_Config.showRAM)) {
                        g_Config.preset = DetectPreset(g_Config);
                        SyncLegacyDisplayFlags(g_Config);
                    }

                    if (g_gpuCount > 0) {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "GPU SELECTION");
                        const char* previewName = (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount)
                            ? g_gpuList[g_Config.selectedGpu].name
                            : "Select GPU...";
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##ovl_gpuselect", previewName)) {
                            for (int i = 0; i < g_gpuCount; i++) {
                                bool isSelected = (g_Config.selectedGpu == i);
                                if (ImGui::Selectable(g_gpuList[i].name, isSelected)) {
                                    SelectGpu(i);
                                }
                                if (isSelected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "POSITION");
                    ImGui::RadioButton("Top Left",      &g_Config.position, 0); ImGui::SameLine(0,18);
                    ImGui::RadioButton("Top Middle",    &g_Config.position, 4); ImGui::SameLine(0,18);
                    ImGui::RadioButton("Top Right",     &g_Config.position, 1);
                    ImGui::RadioButton("Bottom Left",   &g_Config.position, 2); ImGui::SameLine(0,18);
                    ImGui::RadioButton("Bottom Middle", &g_Config.position, 5); ImGui::SameLine(0,18);
                    ImGui::RadioButton("Bottom Right",  &g_Config.position, 3);

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "LAYOUT");
                    ImGui::Text("Overlay UI Size");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##ovl_uiscale", &g_Config.uiScale, 0.75f, 2.25f, "");
                    ImGui::Text("Text Saturation");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##ovl_textsat", &g_Config.textSaturation, 0.00f, 1.40f, "");
                    ImGui::Text("Edge Padding");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##ovl_edgepad", &g_Config.edgePadding, 0.0f, 100.0f, "%.0f px");
                    ImGui::Text("Background Opacity");
                    ImGui::SetNextItemWidth(-1);
                    float ovlBgPct = g_Config.hudBgAlpha * 100.0f;
                    ImGui::SliderFloat("##ovl_hudbg", &ovlBgPct, 0.0f, 100.0f, "%.0f%%");
                    g_Config.hudBgAlpha = ovlBgPct / 100.0f;

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "TEMPERATURE");
                    int ovlTempUnit = g_Config.useFahrenheit ? 1 : 0;
                    if (ImGui::RadioButton("Celsius", &ovlTempUnit, 0)) g_Config.useFahrenheit = false;
                    ImGui::SameLine(0,24);
                    if (ImGui::RadioButton("Fahrenheit", &ovlTempUnit, 1)) g_Config.useFahrenheit = true;

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "HOTKEYS");
                    ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Leave unbound until you choose your own keys.");
                    ImGui::TextColored(ImVec4(.45f,.45f,.50f,1), "Overlay toggles FPS UI. Settings UI toggles this panel.");

                    ImGui::Text("Overlay:");
                    ImGui::SameLine(90);
                    if (g_listeningFor == 1) {
                        ImGui::TextColored(ImVec4(1,.8f,.2f,1), "Press any key...  ");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Cancel##ovl_1")) g_listeningFor = 0;
                    } else {
                        char buf[64];
                        FormatKeyBinding(buf, sizeof(buf), g_Config.toggleKey, g_Config.toggleMod);
                        ImGui::Text("%s", buf);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Change##ovl_1")) g_listeningFor = 1;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##ovl_1")) { g_Config.toggleKey = 0; g_Config.toggleMod = 0; }
                    }

                    ImGui::Text("Settings UI:");
                    ImGui::SameLine(90);
                    if (g_listeningFor == 2) {
                        ImGui::TextColored(ImVec4(1,.8f,.2f,1), "Press any key...  ");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Cancel##ovl_2")) g_listeningFor = 0;
                    } else {
                        char buf[64];
                        FormatKeyBinding(buf, sizeof(buf), g_Config.settingsKey, g_Config.settingsMod);
                        ImGui::Text("%s", buf);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Change##ovl_2")) g_listeningFor = 2;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##ovl_2")) { g_Config.settingsKey = 0; g_Config.settingsMod = 0; }
                    }

                    ImGui::Spacing();
                    if (ImGui::Button("Apply Settings", ImVec2(-1, 34))) {
                        SaveConfig(g_Config);
                    }
                    if (ImGui::Button("Close", ImVec2(-1, 34))) {
                        g_overlaySettingsOpen = false;
                    }
                }
                ImGui::End();
            }

            // Auto-resize can change the window after toggling stats, so align presets
            // with the final size every frame instead of keeping stale center/right edges.
            {
                ImVec2 wPos = ImGui::GetWindowPos();
                ImVec2 wSize = ImGui::GetWindowSize();
                HMONITOR winMon = MonitorFromPoint({ (LONG)(wPos.x + wSize.x * 0.5f), (LONG)(wPos.y + wSize.y * 0.5f) }, MONITOR_DEFAULTTONEAREST);
                MONITORINFO winMi = { sizeof(winMi) };
                GetMonitorInfo(winMon, &winMi);

                float minX = (float)winMi.rcWork.left + margin;
                float minY = (float)winMi.rcWork.top + margin;
                float maxX = (float)winMi.rcWork.right - wSize.x - margin;
                float maxY = (float)winMi.rcWork.bottom - wSize.y - margin;
                if (maxX < minX) maxX = minX;
                if (maxY < minY) maxY = minY;

                {
                    switch (g_Config.position) {
                        default:
                        case 0: wPos = ImVec2(minX, minY); break;
                        case 1: wPos = ImVec2(maxX, minY); break;
                        case 2: wPos = ImVec2(minX, maxY); break;
                        case 3: wPos = ImVec2(maxX, maxY); break;
                        case 4: wPos = ImVec2(((float)winMi.rcWork.left + (float)winMi.rcWork.right - wSize.x) * 0.5f, minY); break;
                        case 5: wPos = ImVec2(((float)winMi.rcWork.left + (float)winMi.rcWork.right - wSize.x) * 0.5f, maxY); break;
                    }
                }

                ImVec2 clamped(
                    wPos.x < minX ? minX : wPos.x > maxX ? maxX : wPos.x,
                    wPos.y < minY ? minY : wPos.y > maxY ? maxY : wPos.y);
                if (clamped.x != ImGui::GetWindowPos().x || clamped.y != ImGui::GetWindowPos().y) {
                    ImGui::SetWindowPos(clamped);
                }
                g_overlayBounds.left   = (LONG)clamped.x;
                g_overlayBounds.top    = (LONG)clamped.y;
                g_overlayBounds.right  = (LONG)(clamped.x + wSize.x);
                g_overlayBounds.bottom = (LONG)(clamped.y + wSize.y);
            }

            ImGui::End();

            // Frame pacing: overlay doesn't need 60+ FPS
            static auto lastFrameTime = std::chrono::steady_clock::now();
            auto nowFr = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(nowFr - lastFrameTime).count();
            const int targetFps = g_overlaySettingsOpen ? 30 : 15;
            const int minFrameTime = 1000 / targetFps;
            if (elapsed < minFrameTime) {
                Sleep((DWORD)(minFrameTime - elapsed));
            }
            lastFrameTime = std::chrono::steady_clock::now();

            Present(0, 0, 0, 0);
        }
    }

    // ═══ Cleanup ═══
    // Join worker threads
    if (g_lhwmInitThread.joinable()) g_lhwmInitThread.join();
    if (g_etwStartupThread.joinable()) g_etwStartupThread.join();
    SaveConfig(g_Config);  // Save settings before exit
    StopEtwSession();
    if (g_Mode == MODE_OVERLAY) RemoveTrayIcon();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClass("justFPS", g_hInstance);
    if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
    ShutdownWMI();

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Window procedure
// ═══════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:
        if (g_Mode == MODE_CONFIG)
            SaveConfig(g_Config);
        g_Running = false;
        return 0;
    case WM_DESTROY:
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            // Show update option if available
            if (g_updateAvailable) {
                char updateText[64];
                snprintf(updateText, sizeof(updateText), "Download Update (%s)", g_latestVersion);
                AppendMenu(m, MF_STRING, IDM_UPDATE, updateText);
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            }
            // Show/Hide toggle based on current visibility
            if (g_OvlVisible)
                AppendMenu(m, MF_STRING, IDM_HIDE, "Hide Overlay");
            else
                AppendMenu(m, MF_STRING, IDM_SHOW, "Show Overlay");
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenu(m, MF_STRING, IDM_SETTINGS, "Settings");
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenu(m, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(m);
            PostMessage(hWnd, WM_NULL, 0, 0);
            // Handle the command directly
            switch (cmd) {
                case IDM_UPDATE:
                    ShellExecuteA(nullptr, "open", 
                        "https://github.com/nathwn12/just-fps/releases/latest",
                        nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDM_SHOW:     g_OvlVisible = true;            break;
                case IDM_HIDE:     g_OvlVisible = false;           break;
                case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
                case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        // Right-click on overlay window itself - only show if Ctrl is held AND cursor is over overlay
        // This prevents showing our menu when user right-clicked elsewhere
        if (g_Mode == MODE_OVERLAY) {
            bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            POINT pt; GetCursorPos(&pt);
            // Only show menu if Ctrl is held AND cursor is over the overlay
            if (ctrlHeld && PtInRect(&g_overlayBounds, pt)) {
                HMENU m = CreatePopupMenu();
                AppendMenu(m, MF_STRING, IDM_SETTINGS, "Settings");
                AppendMenu(m, MF_STRING, IDM_EXIT, "Exit");
                SetForegroundWindow(hWnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(m);
                PostMessage(hWnd, WM_NULL, 0, 0);
                switch (cmd) {
                    case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
                    case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
            case IDM_SHOW:     g_OvlVisible = true;            break;
            case IDM_HIDE:     g_OvlVisible = false;           break;
            case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
            case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
