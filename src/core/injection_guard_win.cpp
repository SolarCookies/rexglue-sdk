/**
 ******************************************************************************
 * ReXGlue SDK - Xbox 360 Recompilation Toolkit                               *
 ******************************************************************************
 * Copyright (c) 2026 Tom Clay. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        injection_guard_win.cpp
 * @brief       Windows implementation of anti-injection protection
 */

#include <rex/platform/injection_guard.h>

#if REX_PLATFORM_WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <rex/logging.h>

namespace rex::platform {

bool EnableAntiInjectionProtection() {
  // Process mitigation policy structures
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature_policy = {};
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY image_load_policy = {};

  bool success = true;

  // Enable image load policy to block remote and low-integrity images
  // This prevents many injection techniques used by overlay software
  image_load_policy.NoRemoteImages = 1;
  image_load_policy.NoLowMandatoryLabelImages = 1;
  image_load_policy.PreferSystem32Images = 1;

  if (!SetProcessMitigationPolicy(ProcessImageLoadPolicy, 
                                   &image_load_policy, 
                                   sizeof(image_load_policy))) {
    DWORD error = GetLastError();
    REXLOG_WARN("Failed to set image load policy: error {}", error);
    success = false;
  } else {
    REXLOG_INFO("Anti-injection: Image load policy enabled");
  }

  // Note: We intentionally do NOT enable signature policy because:
  // 1. It would require signing all our own DLLs
  // 2. It's primarily useful for system processes, not games
  // 3. The image load policy is sufficient for our purposes

  return success;
}

struct OverlayDLLInfo {
  const char* name;
  const char* display_name;
  bool is_critical;  // If true, should show message box
};

static const OverlayDLLInfo kKnownOverlays[] = {
    {"RTSSHooks64.dll", "RivaTuner Statistics Server", true},
    {"RTSSHooks.dll", "RivaTuner Statistics Server (32-bit)", true},
    {"RTSS.dll", "RivaTuner Statistics Server", true},
    {"MSIAfterburner.dll", "MSI Afterburner", true},
    {"discord_hook.dll", "Discord Overlay (legacy)", false},
    {"DiscordHook64.dll", "Discord Overlay", false},
    {"DiscordHook.dll", "Discord Overlay (32-bit)", false},
    {"OverlayHook64.dll", "Generic Overlay Hook", false},
    {"NvCamera64.dll", "NVIDIA ShadowPlay", false},
    {"gameoverlayrenderer64.dll", "Steam Overlay", false},
};

bool DetectOverlayConflicts(bool show_message_box) {
  bool found_critical = false;
  bool found_any = false;
  std::string critical_apps;

  for (const auto& overlay : kKnownOverlays) {
    HMODULE hMod = GetModuleHandleA(overlay.name);
    if (hMod != nullptr) {
      found_any = true;

      char mod_path[MAX_PATH] = {0};
      GetModuleFileNameA(hMod, mod_path, sizeof(mod_path));

      MODULEINFO mod_info = {};
      if (GetModuleInformation(GetCurrentProcess(), hMod, &mod_info, sizeof(mod_info))) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mod_info.lpBaseOfDll);
        size_t size = mod_info.SizeOfImage;

        REXLOG_WARN("Detected overlay DLL: {} ({})", overlay.name, overlay.display_name);
        REXLOG_WARN("  Path: {}", mod_path);
        REXLOG_WARN("  Base: 0x{:016X}, Size: 0x{:08X}", base, size);

        // Check if it conflicts with our preferred physical memory range
        if (base >= 0x180000000ull && base < 0x280000000ull) {
          REXLOG_ERROR("  CRITICAL: DLL occupies address space needed for Xbox 360 memory!");
          REXLOG_ERROR("  This WILL cause crashes when accessing guest RAM.");

          if (overlay.is_critical) {
            found_critical = true;
            if (!critical_apps.empty()) {
              critical_apps += "\n";
            }
            critical_apps += overlay.display_name;
          }
        }
      }
    }
  }

  // Show message box if we found critical overlays
  if (found_critical && show_message_box) {
    std::string message = 
        "The following software is interfering with this game:\n\n" +
        critical_apps + 
        "\n\nThese programs reserve memory addresses that the game needs.\n"
        "Please close them and restart the game.\n\n"
        "If you need performance monitoring, use Task Manager or HWiNFO instead.";

    MessageBoxA(nullptr, 
                message.c_str(),
                "Overlay Software Conflict", 
                MB_OK | MB_ICONERROR | MB_TOPMOST);
  }

  return found_any;
}

bool ForceUnloadDLL(const char* module_name) {
  HMODULE hMod = GetModuleHandleA(module_name);
  if (hMod == nullptr) {
    return false;  // Not loaded, nothing to do
  }

  REXLOG_WARN("Attempting to forcibly unload DLL: {}", module_name);
  REXLOG_WARN("WARNING: This may cause crashes if the DLL has active hooks!");

  // Try to free the library multiple times
  // Overlays often increment the ref count multiple times
  bool freed = false;
  for (int i = 0; i < 100; ++i) {
    if (!FreeLibrary(hMod)) {
      // Check if it actually unloaded
      if (GetModuleHandleA(module_name) == nullptr) {
        freed = true;
        REXLOG_INFO("Successfully unloaded {}", module_name);
        break;
      }
      // Still loaded but FreeLibrary failed
      DWORD error = GetLastError();
      REXLOG_ERROR("FreeLibrary failed: error {}", error);
      break;
    }
  }

  if (!freed) {
    REXLOG_ERROR("Failed to unload {} after 100 attempts", module_name);
  }

  return freed;
}

}  // namespace rex::platform

#endif  // REX_PLATFORM_WIN32
