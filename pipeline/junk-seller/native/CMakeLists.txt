cmake_minimum_required(VERSION 3.25)
project(AGH_JunkSeller VERSION 0.9.0 LANGUAGES CXX)

option(JS_BUILD_PLUGIN "Build the Windows SKSE observation probe" ${WIN32})
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# w-probe-r71 (v0.9.0): E1 shared runtime gate headers
# (the E1 design r4 sections 2.1 and 3; the SR v1.9.0
# block). The pure headers (RuntimeGate.h, InstallLedger.h, ImageRange.h) are
# used by src/Install.h, which the host tests compile too; SkseGate.h binds
# CommonLib and is included by src/Plugin.cpp only. The Build script passes
# -DJS_AGH_RUNTIME_DIR=<runRoot>\agh-runtime (a digested copy).
set(JS_AGH_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/runtime"
    CACHE PATH "Shared runtime gate headers (pipeline/shared/runtime)")
if(NOT EXISTS "${JS_AGH_RUNTIME_DIR}/include/agh/runtime/RuntimeGate.h")
    message(FATAL_ERROR "Shared runtime gate headers missing")
endif()

if(JS_BUILD_PLUGIN)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The plugin requires Windows x64 MSVC; run selftest.py for offline checks")
    endif()
    # w-probe-r71: CommonLibSSE-NG v9.1.0 (a898f469), side by side with the
    # 800d61e tree the probes keep (design section 1).
    set(JS_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/commonlibsse-ng-v9.1.0"
        CACHE PATH "Local CommonLibSSE-NG v9.1.0 source (a898f469)")
    if(NOT EXISTS "${JS_COMMONLIB_DIR}/include/RE/F/FxDelegate.h")
        message(FATAL_ERROR "Local CommonLib source missing; no dependency download is performed")
    endif()
    set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
    # Also closes cmake/Prebuilt.cmake's git fetch / file(DOWNLOAD) path, which
    # runs only when SE, AE and VR are all ON.
    set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLib's Catch2 tests" FORCE)
    set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
    # v9 defaults this ON and then FetchContent-clones MinHook's hde64 from
    # GitHub at configure time; no build-time download (E1 design 6.3).
    set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_subdirectory("${JS_COMMONLIB_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/commonlib" EXCLUDE_FROM_ALL)
    # The DLL target. It began (w-probe-r1) as an observation probe; the four
    # sources below are still the whole DLL. They include the engine-free
    # headers of the product (selector, scheduler, conversion, settings); no
    # other source file is compiled or linked.
    # w-probe-r28 (R29-1): PapyrusBridge.cpp is the AGH_JS_Native registration
    # unit; it owns no settings state of its own.
    add_library(AGH_JunkSeller SHARED
        src/Plugin.cpp src/Probe.cpp src/receipt/Log.cpp
        src/PapyrusBridge.cpp)
    target_compile_definitions(AGH_JunkSeller PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(AGH_JunkSeller PRIVATE CommonLibSSE::CommonLibSSE)
    target_include_directories(AGH_JunkSeller PRIVATE src "${JS_AGH_RUNTIME_DIR}/include")
    target_precompile_headers(AGH_JunkSeller PRIVATE src/PCH.h)
    target_compile_options(AGH_JunkSeller PRIVATE /W4 /permissive- /EHsc /Zi /utf-8)
    if(MSVC)
        # MSVC /pathmap rewrites a path prefix in the file names the compiler
        # embeds (__FILE__, std::source_location, debug info). cl.exe honours it
        # only with /experimental:deterministic; without that it warns D9007 and
        # ignores /pathmap (proof run: 60 D9007 warnings, 37 paths kept).
        # https://learn.microsoft.com/cpp/build/reference/pathmap
        # The roots are the trees this build compiles, read from CMake: this
        # project, the CommonLibSSE target's SOURCE_DIR, the binary dir, the
        # vcpkg installed dir (it can lie outside the build tree) and, when it
        # holds the first three, their common parent (the Build-*.ps1 per-run
        # tree <WorkRoot>\<runId>). Each root is mapped in native and forward-
        # slash spelling (cmake_pch.hxx and /external:I use forward slashes) and
        # longest first, so a nested root is matched before its parent.
        # CommonLibSSE is a separate STATIC target that never sees this target's
        # PRIVATE options, so it gets the same options.
        get_target_property(_js_commonlib_root CommonLibSSE SOURCE_DIR)
        set(_js_roots
            "${CMAKE_CURRENT_SOURCE_DIR}|." "${_js_commonlib_root}|commonlib" "${CMAKE_BINARY_DIR}|build")
        if(DEFINED VCPKG_INSTALLED_DIR AND IS_ABSOLUTE "${VCPKG_INSTALLED_DIR}")
            list(APPEND _js_roots "${VCPKG_INSTALLED_DIR}|vcpkg")
        endif()
        cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _js_run_root)
        cmake_path(HAS_RELATIVE_PART _js_run_root _js_run_ok)
        foreach(_js_inner IN ITEMS "${_js_commonlib_root}" "${CMAKE_BINARY_DIR}")
            cmake_path(IS_PREFIX _js_run_root "${_js_inner}" NORMALIZE _js_run_has)
            if(NOT _js_run_has)
                set(_js_run_ok OFF)
            endif()
        endforeach()
        if(_js_run_ok)
            list(APPEND _js_roots "${_js_run_root}|run")
        endif()
        set(_js_maps "")
        foreach(_js_entry IN LISTS _js_roots)
            string(REPLACE "|" ";" _js_pair "${_js_entry}")
            list(GET _js_pair 0 _js_root)
            list(GET _js_pair 1 _js_alias)
            cmake_path(SET _js_fwd NORMALIZE "${_js_root}")
            cmake_path(NATIVE_PATH _js_fwd _js_native)
            string(LENGTH "${_js_fwd}" _js_len)
            math(EXPR _js_key "10000 + ${_js_len}")
            list(APPEND _js_maps "${_js_key}|${_js_native}=${_js_alias}" "${_js_key}|${_js_fwd}=${_js_alias}")
        endforeach()
        list(SORT _js_maps COMPARE STRING ORDER DESCENDING)
        list(TRANSFORM _js_maps REPLACE "^[0-9]+[|]" "")
        list(TRANSFORM _js_maps PREPEND "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/pathmap:")
        list(TRANSFORM _js_maps APPEND ">")
        set(_js_pathmap "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/experimental:deterministic>" ${_js_maps})
        target_compile_options(AGH_JunkSeller PRIVATE ${_js_pathmap})
        target_compile_options(CommonLibSSE PRIVATE ${_js_pathmap})
        # /PDBALTPATH sets the PDB name embedded in the DLL. It is read from the
        # target's PDB_NAME: the Visual Studio generator escaped %_PDB% into
        # %%%%_PDB%%%%, which embedded "%%%<name>.pdb%%%" (proof run).
        # https://learn.microsoft.com/cpp/build/reference/pdbaltpath-use-alternate-pdb-path
        target_link_options(AGH_JunkSeller PRIVATE "/PDBALTPATH:$<TARGET_PROPERTY:AGH_JunkSeller,PDB_NAME>.pdb")
    endif()
    target_link_options(AGH_JunkSeller PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
    set_target_properties(AGH_JunkSeller PROPERTIES
        OUTPUT_NAME AGH_JunkSeller
        PDB_NAME AGH_JunkSeller
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
