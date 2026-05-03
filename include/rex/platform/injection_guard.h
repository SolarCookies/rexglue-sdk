/**
 ******************************************************************************
 * ReXGlue SDK - Xbox 360 Recompilation Toolkit                               *
 ******************************************************************************
 * Copyright (c) 2026 Tom Clay. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        injection_guard.h
 * @brief       Utilities to prevent DLL injection from overlay software
 *
 * Protects the process from RivaTuner, MSI Afterburner, Discord overlay, etc.
 * These overlays can conflict with the Xbox 360 memory mapping by reserving
 * address ranges needed for the guest physical memory mirror.
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_WIN32

namespace rex::platform {

/**
 * @brief Prevents injection of untrusted DLLs into the process
 * 
 * Sets Windows process mitigation policies to:
 * - Block remote images (DLLs loaded from network shares)
 * - Block low integrity images (prevents some injectors)
 * - Enable signature policy (requires signed DLLs in some contexts)
 * 
 * @return true if policies were successfully applied
 * 
 * @note Should be called as early as possible in the process lifetime,
 *       ideally before any other initialization.
 */
bool EnableAntiInjectionProtection();

/**
 * @brief Detects and warns about problematic overlay DLLs
 * 
 * Checks if known overlay software is already injected:
 * - RivaTuner Statistics Server (RTSSHooks64.dll)
 * - MSI Afterburner (MSIAfterburner.dll)
 * - Discord overlay (DiscordHook64.dll)
 * 
 * @param show_message_box If true, shows a Windows message box prompting
 *                          the user to close the overlay software
 * @return true if problematic DLLs were detected
 * 
 * @note This does NOT prevent injection, it only detects after the fact.
 *       Use EnableAntiInjectionProtection() to prevent injection.
 */
bool DetectOverlayConflicts(bool show_message_box = false);

/**
 * @brief Forcibly unloads a DLL from the current process
 * 
 * Attempts to free the specified module. This is a last-resort measure
 * and may cause crashes if the DLL has active threads or hooks.
 * 
 * @param module_name Name of the DLL to unload (e.g., "RTSSHooks64.dll")
 * @return true if the DLL was successfully unloaded
 * 
 * @warning This is DANGEROUS and can cause crashes! Only use if you're
 *          certain the DLL is not being actively used.
 */
bool ForceUnloadDLL(const char* module_name);

}  // namespace rex::platform

#endif  // REX_PLATFORM_WIN32
