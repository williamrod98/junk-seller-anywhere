# Junk Seller Anywhere - source code

Source code of **Junk Seller Anywhere - One Key Bulk Sale v0.9.0**, a Skyrim Special Edition /
Anniversary Edition mod by William_Rod: the SKSE plugin `AGH_JunkSeller.dll`, the Papyrus scripts
`AGH_JS_MCM`, `AGH_JS_Native` and `AGH_JS_Quest`, the generator of the plugin file
`AGH_JunkSeller.esp`, and the base INI `AGH_JunkSeller.ini`.

Mod page: https://www.nexusmods.com/skyrimspecialedition/mods/192728

## License

- The plugin DLL is statically linked with CommonLibSSE-NG 9.1.0 (commit
  `a898f469851c464d05137bb74b069dd234897643`), which is licensed under GPL-3.0-or-later with the Modding
  Exception and the GPL-3.0 Linking Exception. The distributed DLL is therefore licensed under the **GNU
  General Public License v3.0 or later**; see `COPYING` and `NOTICE`.
- The files written for this mod (everything in this repository except `COPYING` and the license texts
  quoted in `THIRD_PARTY_NOTICES.md`) are licensed under the **MIT License** (`LICENSE`). MIT is
  compatible with the GPL.
- `THIRD_PARTY_NOTICES.md` lists every library the DLL is built with, with its license and copyright
  notice.

## Layout

| Path | Content |
|---|---|
| `pipeline/junk-seller/native/` | the DLL: `CMakeLists.txt`, `vcpkg.json`, `commonlib-pin-v9.json`, `src/` (C++23) |
| `pipeline/shared/runtime/include/agh/runtime/` | the runtime gate headers the DLL includes |
| `pipeline/junk-seller/Build-JunkSeller.ps1` | the Windows build script (configure, build, output checks) |
| `pipeline/junk-seller/native/commonlib-pin-v9.json` | sha256 pins of the CommonLibSSE-NG headers and of the complete CommonLibSSE-NG source tree the build expects |
| `pipeline/junk-seller/generate_js.py` | writes `AGH_JunkSeller.esp` and `Seq/AGH_JunkSeller.seq` (deterministic, Python 3 standard library only) |
| `pipeline/junk-seller/AGH_JunkSeller.ini` | the base INI shipped in `SKSE/Plugins/` |
| `pipeline/junk-seller/Source/Scripts/` | Papyrus sources `AGH_JS_MCM.psc`, `AGH_JS_Native.psc`, `AGH_JS_Quest.psc` |
| `tools/pex_header.py` | shows and sets the user and machine fields of a compiled `.pex` header |
| `vendor/` | where the CommonLibSSE-NG 9.1.0 source goes (not tracked; see below) |

The translation tables and the texts ship in the mod archive in their source form and are not built from
this repository.

The C++, Papyrus and CMake sources are copies of the v0.9.0 build inputs whose comments were trimmed for
publication (references to private development notes were removed or generalised); their code is
token-for-token identical to the build inputs, with the same line numbers. The other files are not such
copies:

- `pipeline/junk-seller/Build-JunkSeller.ps1` replaces the private build script: every location is a
  parameter, and it writes the same `src/BuildInfo.h` lines and builds the same CMake target. The Papyrus
  compile and the translation files are left out of it (see below).
- `pipeline/junk-seller/generate_js.py` has the same record-writing code as the generator the release was
  built with; only its comments and command line differ (`--out` is required). It writes the shipped
  `AGH_JunkSeller.esp` and `Seq/AGH_JunkSeller.seq` byte for byte (hashes below).
- `tools/pex_header.py` replaces a private release step; `README.md`, `NOTICE`, `vendor/README.md`,
  `.gitignore` and `.gitattributes` were written for this repository; `LICENSE`, `COPYING` and
  `THIRD_PARTY_NOTICES.md` are the mod archive's files.
- `pipeline/junk-seller/AGH_JunkSeller.ini`, `native/vcpkg.json` and `native/commonlib-pin-v9.json` are
  byte copies; their descriptive fields keep the development labels of the build.

`src/BuildInfo.h` holds host defaults; the build script replaces it in its scratch copy with the build
identity (version, release id, and the SHA-256 of the base INI and of the plugin file). Code strings were
not edited, so a few keep development wording; for example the CMake error for a non-Windows configure
names `selftest.py`, a private offline check that is not part of this repository.

## Build requirements (the exact versions v0.9.0 was built with)

- Windows x64 with Visual Studio 2022 Build Tools: MSVC toolset 14.44 (VCToolsVersion 14.44.35207,
  cl.exe 19.44.35228), Windows SDK 10.0.26100.0. The build script checks for toolset 14.44.
- CMake 4.4.3 (the build script checks the version).
- Python 3 (runs `generate_js.py`).
- vcpkg (https://github.com/microsoft/vcpkg), tool commit `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`;
  the manifest `pipeline/junk-seller/native/vcpkg.json` pins `builtin-baseline`
  `ee12231b20c95013c6638d845d04c91559a1d1ff`, triplet `x64-windows-static-md`. Resolved ports:
  directxmath 2025-04-03, directxtk 2025-10-27, fmt 12.1.0, nlohmann-json 3.12.0 (port version 1),
  rapidcsv 8.90, rsm-binary-io 2.0.6, simpleini 4.25, spdlog 1.16.0, toml11 4.4.0, xbyak 7.28,
  vcpkg-cmake 2024-04-23, vcpkg-cmake-config 2024-05-23.
- CommonLibSSE-NG 9.1.0, commit `a898f469851c464d05137bb74b069dd234897643`
  (https://github.com/alandtse/CommonLibSSE-NG), in `vendor/commonlibsse-ng-v9.1.0/`.
- To compile the Papyrus scripts: the Creation Kit's `PapyrusCompiler.exe` and its
  `TESV_Papyrus_Flags.flg`, the base game script sources from the Creation Kit, the SKSE64 script
  sources, and the SkyUI SDK script sources (`SKI_ConfigBase`). None of these are redistributed here.

To run the mod (not needed to build): game runtime 1.6.1170.0 with SKSE64 2.2.6 or later, or 1.7.104.0
with SKSE64 2.3.1; Address Library for SKSE Plugins in a release that includes your runtime; SkyUI.

## Getting CommonLibSSE-NG 9.1.0

The build pins the complete CommonLibSSE-NG source tree by a digest (`sourceTreeSha256` in
`pipeline/junk-seller/native/commonlib-pin-v9.json`), so the tree must be exactly the one v0.9.0 was
built from. Either:

1. (Recommended) Download `CommonLibSSE-NG-9.1.0-a898f469-source.zip` from the v0.9.0 release of this
   repository and extract it into `vendor/` (it creates `vendor/commonlibsse-ng-v9.1.0/`) with a tool
   that keeps the file bytes (no line-ending conversion); or
2. Clone https://github.com/alandtse/CommonLibSSE-NG into `vendor/commonlibsse-ng-v9.1.0` without
   Windows line-ending conversion, so text files keep their LF endings (CommonLibSSE-NG's own
   `.gitattributes` still applies its explicit rules, such as CRLF for `.cmd` files):

   ```
   git -c core.autocrlf=false -c core.eol=lf clone https://github.com/alandtse/CommonLibSSE-NG vendor/commonlibsse-ng-v9.1.0
   git -C vendor/commonlibsse-ng-v9.1.0 -c core.autocrlf=false -c core.eol=lf checkout a898f469851c464d05137bb74b069dd234897643
   ```

   then delete the four editor-assistant note files `.cody.md`, `.cursorrules`, `.factory/AGENTS.md` and
   `COPILOT.md`, which were not part of the v0.9.0 build tree (they are not build inputs). The
   `extern/openvr` submodule stays empty (VR is not built). A checkout with converted line endings fails
   the build script's pin check before anything is built.

The digest skips any `.git` and `build` folder.

## Building the DLL

1. Install the vcpkg ports into a dependency folder of your choice (online, once):

   ```
   <vcpkg-root>\vcpkg.exe install --x-manifest-root=pipeline\junk-seller\native --x-install-root=<deps-dir> --triplet=x64-windows-static-md --host-triplet=x64-windows-static-md
   ```

2. From a "Developer PowerShell for VS 2022" (x64), at the repository root:

   ```
   powershell -NoProfile -ExecutionPolicy Bypass -File pipeline\junk-seller\Build-JunkSeller.ps1 -VcpkgRoot <vcpkg-root> -VcpkgInstalledDir <deps-dir> -WorkRoot <scratch-dir> -OutputDirectory <out-dir>
   ```

   The script checks the tool versions and the CommonLibSSE-NG pins, copies the sources into a new
   folder under `<scratch-dir>`, runs `generate_js.py`, writes the build identity into the copied
   `src/BuildInfo.h`, configures with the Visual Studio 17 2022 generator (toolset `v143,version=14.44`,
   no dependency download: `VCPKG_MANIFEST_INSTALL=OFF`), builds `AGH_JunkSeller` (Release), checks the
   SKSE exports, and writes `AGH_JunkSeller.dll`, its `.pdb`, `AGH_JunkSeller.esp`,
   `Seq/AGH_JunkSeller.seq`, `SHA256SUMS.txt`, `provenance.json` and `exports.txt` to `<out-dir>`.
   Use `-Python <path>` when `python.exe` is not on `PATH`.

The compiler records source file names relative to the build folders (`/pathmap` with
`/experimental:deterministic`), so the build location does not appear in the DLL. A rebuild still differs
from the released DLL in the PE time stamp and the debug-directory fields (PDB signature and age).

## Compiling the Papyrus scripts

```
PapyrusCompiler.exe AGH_JS_Native.psc "-i=<folder of the .psc files>;<base game script sources>" "-o=<out-dir>" "-f=<base game script sources>\TESV_Papyrus_Flags.flg"
PapyrusCompiler.exe AGH_JS_Quest.psc "-i=<folder of the .psc files>;<SKSE64 script sources>;<base game script sources>" "-o=<out-dir>" "-f=<base game script sources>\TESV_Papyrus_Flags.flg"
PapyrusCompiler.exe AGH_JS_MCM.psc "-i=<folder of the .psc files>;<SKSE64 script sources>;<base game script sources>;<SkyUI SDK script sources>" "-o=<out-dir>" "-f=<base game script sources>\TESV_Papyrus_Flags.flg"
```

`AGH_JS_Quest` calls `SKSE.GetPluginVersion`, so it needs the SKSE64 script sources; `AGH_JS_MCM` extends
`SKI_ConfigBase` and calls `AGH_JS_Native`. The compiler writes the compiling user and machine names into
each `.pex` header; the released files carry the neutral values `build` and `AGHB`:

```
python3 tools/pex_header.py --set-neutral <out-dir>/AGH_JS_MCM.pex <out-dir>/AGH_JS_Native.pex <out-dir>/AGH_JS_Quest.pex
```

A recompiled `.pex` still differs from the released one in the compile time stored in its header.

## v0.9.0 release files

| File in the mod archive | SHA-256 |
|---|---|
| `SKSE/Plugins/AGH_JunkSeller.dll` | `06397226c6f4ea791f9dc17d7fd78c3324f8963e4eeafb7bc33c90aed8baef79` |
| `SKSE/Plugins/AGH_JunkSeller.ini` | `c442d185986ec5d46f5af13be350e2ae9510f4eee3868486bae3f0b37910ac24` |
| `AGH_JunkSeller.esp` | `ae2f621516e091b02f68ce5bee1260d81722ef8af066d240d207fca61ea51a56` |
| `Seq/AGH_JunkSeller.seq` | `fd597d14de8e7775ac5bb4d6fa065aa5e0057f50629205287b4528f05c25dbfa` |
| `Scripts/AGH_JS_MCM.pex` | `ebcfa405118a072c2f2df3fed429554070044b0eabf0b7e3d87b73ba54b0af50` |
| `Scripts/AGH_JS_Native.pex` | `1871f3bc410495cf908ffc608d572d247d7a1dc95f8caf33e07e957661a08a2d` |
| `Scripts/AGH_JS_Quest.pex` | `66ed80e97b9df63cb39d4a060f1adf10b0da07652458e32c4abe8df95f1f73b8` |
