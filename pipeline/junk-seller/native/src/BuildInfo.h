#pragma once

// Host defaults are intentionally non-provenance values. Release builds replace
// this file in their isolated native source tree before CMake configures.
namespace js::BuildInfo {
// (design-r2 section 8, S2e): the product version shown in the MCM
// Diagnostics page (AGH_JS_MCM.psc RuntimeText(), via the GetVersion native).
// Kept in step with CMakeLists.txt's `project(AGH_JunkSeller VERSION ...)` by
// convention; a release build must also carry this line into its replaced
// copy of this file (see the note at the top of this file and the comment in
// pipeline/junk-seller/Build-JunkSeller.ps1 near the other BuildInfo.h lines).
inline constexpr const char* Version = "0.9.0";
inline constexpr const char* ReleaseId = "w-probe-r71";
// w-probe-r71 (v0.9.0): no runtime or SKSE literal here any more; the
// js_startup row and GetRuntime take them from the shared runtime gate's
// labels (Install.h LabelsFor, pipeline/shared/runtime kRuntimeTable).
inline constexpr const char* AddressLibrary = "versionlib runtime check";
inline constexpr const char* BaseIniSha256 = "unknown";
inline constexpr const char* EspSha256 = "unknown";
inline constexpr const char* AcceptanceRun = "none";
}
