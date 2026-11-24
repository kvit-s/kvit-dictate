# kvit-dictate — build system.
#
# Windows-only, MSVC-only, no third-party dependencies of any kind (plan.md §5.3 and the
# repository rules). There is no find_package() call in this file and there must not be one:
# every line of C++ this project links is either in src/ or in the Windows SDK.
#
# Layout is exactly plan.md §2, plus the util/ files introduced by the C++ skeleton (utf8, json,
# log, result, clock) — all of which are named in the task that created them and are documented
# in their own headers.

cmake_minimum_required(VERSION 3.24)

# RC is a language here because the executable now carries an icon, a version block and an
# application manifest (release-plan.md §3.2). It is enabled at the top rather than added later
# so that a generator which caches the language set does not need a fresh configure.
project(kvit-dictate VERSION 1.0.0 LANGUAGES CXX RC)

# ─── Toolchain preconditions ─────────────────────────────────────────────────────────────────
#
# Fail early and legibly rather than producing a wall of errors from <windows.h>. The project is
# Windows-only by design, not by accident: it is built out of WH_KEYBOARD_LL, WASAPI, SendInput,
# BitBlt and DirectWrite (plan.md §1.3).

if(NOT WIN32)
    message(FATAL_ERROR "kvit-dictate is Windows-only (plan.md §1.3). Nothing here ports.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "kvit-dictate builds with MSVC 2022 only (plan.md §5.3, matching kvit-notes). "
        "clang-cl and MinGW are untested and the SEH/COM/manifest behaviour differs.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# std::expected is C++23 and is deliberately not used; kd::Result in src/util/result.h exists
# because of this line. Do not raise the standard to reach for <expected> without revisiting
# that header's rationale.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The Visual Studio generator is multi-config, so CMAKE_BUILD_TYPE is meaningless here; the
# configuration is chosen at build time (`cmake --build . --config Debug`). Ninja is not on PATH
# on this machine, which is why the presets use the VS generator (plan.md §5.3).
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# ─── Options ─────────────────────────────────────────────────────────────────────────────────

# Warnings-as-errors is ON.
#
# It was OFF while every module was a stub whose .cpp defined none of the functions its header
# declared, because /W4 then legitimately reported unreferenced parameters and unreachable code
# throughout and the only way to silence it was suppressions on code about to be replaced. That
# reason expired when the modules were written: Debug and Release both build at zero warnings, so
# the next warning is new, and the cheapest moment to look at a new warning is the build that
# introduced it.
#
# Note for anyone who trips over it: MSVC reports C4129 for an escape-looking sequence inside a
# *raw* string literal, where there are no escapes by definition. tests/test_json.cpp writes those
# two cases with ordinary escapes instead; the bytes are the same.
option(KD_WERROR "Treat compiler warnings as errors" ON)

# The named-pipe binding, with everything it implies: CreateProcessW, a job object, command-line
# quoting, environment-block construction, and reading a child's stdout.
#
# release-plan.md §1.2 says of the spawn machinery: "Deleted from shipped builds. There is no
# child to spawn, kill or outlive." This option is what makes that literally true rather than
# merely unreached — with it off, a `kvit-dictate.exe` contains no CreateProcessW at all, and
# attack surface a binary does not contain cannot be reached by a bug elsewhere in it.
#
# **ON by default**, because a development tree and bench/ both launch interpreters locally and
# are the reason the binding survives. packaging/windows/build-release.ps1 turns it off, and is
# the only place that does.
option(KD_ENABLE_SIDECAR_SPAWN "Compile the child-spawning named-pipe transport (dev/bench)" ON)

option(KD_BUILD_TOOLS "Build tools/probe and tools/testtarget" ON)
option(KD_BUILD_TESTS "Build the kd_tests binary" ON)

# ─── Common compile settings ─────────────────────────────────────────────────────────────────
#
# Applied through an INTERFACE target so every target gets an identical set and nothing drifts.

add_library(kd_common_flags INTERFACE)

target_compile_definitions(kd_common_flags INTERFACE
    UNICODE                       # every Win32 call resolves to its -W form
    _UNICODE                      # ...and every TCHAR macro follows
    NOMINMAX                      # windows.h's min/max macros break std::min/std::max and <limits>
    WIN32_LEAN_AND_MEAN           # drops winsock1, OLE, RPC and MMSYSTEM from the default include
    _CRT_SECURE_NO_WARNINGS       # the "use _s variants" advice; the code uses bounded APIs anyway

    # plan.md §1.9: "Diagnostic logging of recognised text is opt-in, retention-bounded, and off
    # in release builds." KD_TRANSCRIPT compiles to nothing unless this is defined, so a release
    # binary physically cannot write a transcript — a stronger guarantee than a runtime flag.
    $<$<CONFIG:Debug>:KD_ENABLE_TRANSCRIPT_LOG>
)

target_compile_options(kd_common_flags INTERFACE
    /W4                 # /Wall is unusable: the Windows SDK headers alone emit thousands
    /permissive-        # standard conformance; catches two-phase-lookup mistakes early
    /utf-8              # source AND execution charset are UTF-8. Load-bearing: the headers and
                        # tests contain UTF-8 literals, and plan.md §1.2 makes UTF-8 end-to-end
                        # the project's central text invariant. Without this MSVC would read
                        # sources as the system code page.
    /Zc:__cplusplus     # otherwise __cplusplus reports 199711L and feature tests silently fail
    /diagnostics:column
)

if(KD_WERROR)
    target_compile_options(kd_common_flags INTERFACE /WX)
endif()

if(KD_ENABLE_SIDECAR_SPAWN)
    target_compile_definitions(kd_common_flags INTERFACE KD_ENABLE_SIDECAR_SPAWN)
endif()

# ─── The C runtime: static (/MT), and this is the moment it changed ──────────────────────────
#
# It was dynamic (/MD) for the whole experimental phase, and the comment here said why: "this is
# not a redistributable yet, and /MT would have to be revisited when the binary is signed".
# release-plan.md §3.2 is that moment. The payload is one executable, and /MT removes the
# redistributable step from the installer entirely — no vc_redist bootstrapper, no "the program
# can't start because VCRUNTIME140.dll is missing" on a machine that has never had Visual Studio.
#
# The costs, stated because they are real: the binary is roughly 200 KB larger, and a CRT
# security fix reaches it only through a rebuild rather than through Windows Update. For a
# single-executable tool with no plugins and no DLLs of its own, that is the right side of the
# trade — there is no second module to share a heap with, which is the failure mode /MT is
# usually warned about.
#
# MSVC_RUNTIME_LIBRARY needs CMP0091 NEW, which cmake_minimum_required(3.24) sets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─── kd_wire: the protocol and the text primitives, shared by both executables ───────────────
#
# Split out of kd_core when kvit-asrd arrived (openai-translator.md §4.8). The translator needs
# the codec, the grapheme layer, the JSON parser and the logger; it must NOT need Direct2D,
# DirectWrite, WASAPI or the settings dialog, all of which kd_core drags in. Linking kd_core
# into a headless translator would pull Direct2D into it and make the size claim a lie.
#
# The rule for this list: a file belongs in kd_wire if it has no UI, no audio device and no
# window — the same test that decides whether a module is unit-testable without a message loop.
# `src/session.cpp` and `src/commit_engine.cpp` deliberately stay in kd_core: they are the
# *client's* halves of the conversation, and openai-translator.md §4.3 is explicit that the
# translator ports the producer of `stable_bytes` rather than calling the consumer of it.

set(KD_WIRE_SOURCES
    src/protocol.cpp
    src/util/utf8.cpp
    src/util/json.cpp
    src/util/log.cpp
    # The Nemotron model's download: the Settings page runs it, and `kvit-asrd --nemotron-download`
    # runs the same code, so it lives where both executables can reach it.
    src/model_download.cpp
)

set(KD_WIRE_HEADERS
    src/protocol.h
    src/model_download.h
    src/util/clock.h
    src/util/json.h
    src/util/wav.h
    src/util/log.h
    src/util/result.h
    src/util/utf8.h
)

add_library(kd_wire STATIC ${KD_WIRE_SOURCES} ${KD_WIRE_HEADERS})
target_include_directories(kd_wire PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# model_download.cpp: WinHTTP for the two HTTPS requests, BCrypt for SHA-256, shell32 and ole32
# for SHGetKnownFolderPath(%LOCALAPPDATA%) and the CoTaskMemFree it requires.
target_link_libraries(kd_wire PUBLIC kd_common_flags winhttp bcrypt shell32 ole32)

# ─── kd_core: every module except main.cpp ───────────────────────────────────────────────────

set(KD_CORE_SOURCES
    # Pure logic — no <windows.h>, unit-tested without a UI (see the header of each).
    src/session.cpp
    src/commit_engine.cpp
    src/settings.cpp
    src/corpus.cpp
    src/history.cpp
    src/edit_buffer.cpp

    # Windows-facing, but with no UI: launching and supervising kvit-asrd.exe.
    # openai-translator.md §4.6 — always compiled, never behind KD_ENABLE_SIDECAR_SPAWN.
    src/translator.cpp

    # Windows-facing.
    src/normalizer.cpp
    src/hotkey.cpp
    src/audio_capture.cpp
    src/ipc_client.cpp
    src/anchor.cpp
    src/injector.cpp
    src/overlay.cpp
    src/theme.cpp
    src/tray_icon.cpp
    src/updater.cpp
)

set(KD_CORE_HEADERS
    src/normalizer.h
    src/session.h
    src/commit_engine.h
    src/anchor.h
    src/audio_capture.h
    src/hotkey.h
    src/injector.h
    src/ipc_client.h
    src/overlay.h
    src/settings.h
    src/theme.h
    src/tray_icon.h
    src/corpus.h
    src/history.h
    src/edit_buffer.h
    src/translator.h
    src/updater.h
)

add_library(kd_core STATIC ${KD_CORE_SOURCES} ${KD_CORE_HEADERS})

# The version the updater compares against GitHub's releases (src/updater.h). project() carries
# only MAJOR.MINOR.PATCH, so a release candidate's tag suffix arrives separately:
# packaging/windows/build-release.ps1 passes it from the tag (v1.0.0-rc2 -> rc2), and every other
# build leaves it empty. Without it an rc build would call itself 1.0.0 and never be offered the
# 1.0.0 release. On this one file only, so changing it recompiles one file.
set(KD_VERSION_PRE "" CACHE STRING "Pre-release part of the version, e.g. rc2; empty for a release")
if(NOT KD_VERSION_PRE MATCHES "^[0-9A-Za-z.-]*$")
    message(FATAL_ERROR "KD_VERSION_PRE='${KD_VERSION_PRE}' may hold only letters, digits, '.' and '-'")
endif()
set(KD_UPDATER_DEFINES KVIT_VERSION_RAW=${PROJECT_VERSION})
if(NOT KD_VERSION_PRE STREQUAL "")
    list(APPEND KD_UPDATER_DEFINES KVIT_VERSION_PRE=${KD_VERSION_PRE})
endif()
set_source_files_properties(src/updater.cpp PROPERTIES COMPILE_DEFINITIONS "${KD_UPDATER_DEFINES}")

target_include_directories(kd_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kd_core PUBLIC kd_common_flags kd_wire)

# ─── Windows libraries ───────────────────────────────────────────────────────────────────────
#
# Only what is actually required, with the module that requires it named. When a module is
# implemented and turns out to need something else, add it here with a reason — an unexplained
# library in this list is how a dependency nobody can justify survives for years.
#
#   user32    hotkey (SetWindowsHookEx, SendInput), injector, overlay, anchor
#             (GetGUIThreadInfo, GetForegroundWindow), main (message loop, tray window)
#   gdi32     anchor — BitBlt from the desktop DC, CreateCompatibleBitmap/DC (plan.md §1.5)
#   advapi32  ipc_client — the logon-SID DACL: GetTokenInformation, InitializeSecurityDescriptor,
#             SetSecurityDescriptorDacl, InitializeAcl, AddAccessAllowedAce (docs/protocol.md §2.3)
#   bcrypt    ipc_client — BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG) for the per-launch
#             random pipe name (docs/protocol.md §2.2). Not in plan.md's sketch, but §2.2 names
#             the API, and a predictable name is a squattable name.
#   ole32     audio_capture — COM (CoInitializeEx, CoCreateInstance for IMMDeviceEnumerator) and
#             PropVariantClear; injector/overlay — CoTaskMemFree from shell APIs.
#             *There is no mmdevapi.lib to link.* WASAPI is pure COM: mmdeviceapi.h and
#             audioclient.h declare the interfaces, ole32 provides the activation.
#   oleaut32  audio_capture — VariantClear/SysAllocString paths used when reading device
#             properties through IPropertyStore.
#   uuid      audio_capture — the DEFINE_GUID'd constants (CLSID_MMDeviceEnumerator and friends)
#             that mmdeviceapi.h declares but does not define. If a GUID still fails to resolve
#             at link time, the fix is `#include <initguid.h>` in exactly ONE translation unit
#             before <mmdeviceapi.h> — never INITGUID defined project-wide, which produces
#             duplicate symbols the moment two TUs include the same header.
#   avrt      audio_capture — AvSetMmThreadCharacteristicsW(L"Audio") on the capture thread.
#             Without MMCSS, a busy machine glitches in a way indistinguishable from a bad model.
#   ksuser    audio_capture — KSDATAFORMAT_SUBTYPE_PCM / KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, needed
#             to interpret a WAVEFORMATEXTENSIBLE mix format. If the implementation ends up
#             comparing those GUIDs by value instead, delete this line.
#   d2d1      overlay — Direct2D device context and the layered-window render target;
#             tray_icon — the same DC-render-target-into-a-DIB technique, for a notification
#             icon that is drawn at run time rather than shipped as artwork
#   dwrite    overlay — DirectWrite text layout and measurement
#   dwmapi    overlay/anchor — DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) for a window
#             rectangle that excludes the invisible resize border, which the naive GetWindowRect
#             includes and which would offset every capture; theme — DwmSetWindowAttribute
#             (DWMWA_USE_IMMERSIVE_DARK_MODE), which is the only way to darken a title bar
#   uxtheme   theme — SetWindowTheme, which opts one control at a time into the DarkMode_* theme
#             classes. The undocumented ordinals theme.cpp also uses are resolved by hand at
#             runtime and need no import library; this line is for the documented call alone.
#   shcore    overlay/anchor — GetDpiForMonitor for the mixed-DPI cases plan.md §7 gates on.
#             NOTE: PROCESS_PER_MONITOR_DPI_AWARE_V2 is *not* shcore's SetProcessDpiAwareness
#             (that API's enum only reaches v1); v2 comes from the application manifest or from
#             user32's SetProcessDpiAwarenessContext. The manifest is preferred because it
#             applies before any window exists — see the TODO under kvit-dictate below.
#   shell32   main — Shell_NotifyIconW (tray icon), SHGetKnownFolderPath (%APPDATA% settings path)
#   winhttp   normalizer — the POST to the text-normalizer endpoint (normalizer.h).
#             Synchronous WinHTTP on a thread of its own, the same shape ipc_client
#             uses for the pipe. This is the ONLY outbound network call in the
#             program, it is off by default, and plan.md §1.9's "nothing is sent"
#             is why enabling it has to be an explicit act by the user.
#   crypt32   settings — CryptProtectData/CryptUnprotectData and the base64 codec, for the speech
#             service's API key at rest (settings.h, ServerSettings::api_key). DPAPI rather than
#             a key of our own, because a key this program kept would need somewhere safe to live.
#   comdlg32  settings — the Save/Open file dialogs behind "Save settings…" and "Restore
#             settings…".
#   comctl32  main/settings — InitCommonControlsEx and the common controls used by the settings
#             dialog; also required for the v6 visual styles the dialog assumes
#
# Deliberately absent, so nobody adds them back on a hunch:
#   propsys   — device friendly names are read as PROPVARIANT::pwszVal and released with
#               PropVariantClear (ole32). propsys is only needed for the PropVariantToXxx
#               helpers, which nothing here uses yet.
#   winmm     — the legacy waveIn API is not used; WASAPI replaces it entirely.
#   uiautomationcore — plan.md §1.5/§1.7 commit to a no-UIA design. Adding this library would be
#               a design change, not a build change.

target_link_libraries(kd_core PUBLIC
    user32
    gdi32
    advapi32
    bcrypt
    ole32
    oleaut32
    uuid
    avrt
    ksuser
    d2d1
    dwrite
    dwmapi
    uxtheme
    shcore
    shell32
    comctl32
    comdlg32
    crypt32
    winhttp
)

# ─── kvit-dictate.exe ────────────────────────────────────────────────────────────────────────

# ─── Resources: icon, version block, manifest (release-plan.md §3.2) ─────────────────────────
#
# This closes the TODO(phase-3) that stood here for the whole experimental phase. The manifest
# half of it was never cosmetic: anchor.cpp works in physical screen pixels and warns at run time
# (anchor.cpp:801) when PerMonitorV2 is missing, so every scaled monitor was a live correctness
# bug. See packaging/windows/kvit-dictate.manifest for the rest of the argument.
#
add_executable(kvit-dictate WIN32 src/main.cpp packaging/windows/kvit-dictate.rc)
target_link_libraries(kvit-dictate PRIVATE kd_core)

# The version numbers come from project(), so the .rc and the binary cannot drift. Passed as
# bare tokens and stringified inside the .rc rather than quoted here: a quoted define has to
# survive CMake, the generator and rc.exe's own command line, and each of the three escapes
# backslashes differently.
target_compile_definitions(kvit-dictate PRIVATE
    KVIT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KVIT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KVIT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    KVIT_VERSION_RAW=${PROJECT_VERSION}
)

# The .rc references kvit-dictate.ico and kvit-dictate.manifest by bare name, so rc.exe has to
# be able to find them; its include path is where it looks.
target_include_directories(kvit-dictate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows")

# MSVC would otherwise synthesise a default manifest and embed it *as well*, which produces two
# RT_MANIFEST resources and an executable Windows refuses to start.
set_target_properties(kvit-dictate PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# models.json sits beside the executable (see the schema block in src/settings.h). Copied rather
# than embedded so a user can point an entry at a different interpreter without a rebuild —
# which is the whole reason plan.md §1.1 puts the model→interpreter map in a data file.
#
# **The installer does not ship it** (release-plan.md §2.2). A shipped build spawns nothing, so
# there is no interpreter to map; the presence of this file next to the executable is what makes
# a *development* tree still able to launch a local sidecar. Deleting this copy step would break
# the bench; shipping the file would put a "mock" model in front of a new user, and the mock
# types invented sentences.
add_custom_command(TARGET kvit-dictate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/models.json"
            "$<TARGET_FILE_DIR:kvit-dictate>/models.json"
    COMMENT "Copying models.json next to kvit-dictate.exe"
    VERBATIM
)

# ─── kvit-asrd.exe — the translator (openai-translator.md §4.8) ──────────────────────────────
#
# The second half of the shipped application: it speaks docs/protocol.md on a loopback
# WebSocket and an OpenAI-compatible /v1/audio/transcriptions service over HTTP, so that a user
# who owns a speech box configures **one address** instead of keeping a Python window open.
#
# **It does not link kd_core.** That is the whole reason kd_wire exists: kd_core carries the
# overlay, the tray, the injector, WASAPI and the settings dialog, and linking it here would
# pull Direct2D into a headless translator and make the size claim a lie.
#
# Console subsystem on purpose: standalone use is a terminal, and when the client spawns it,
# CREATE_NO_WINDOW hides the console. No icon and no application manifest beyond asInvoker —
# there is no window to be DPI-aware about.

set(KD_ASRD_SOURCES
    src/asrd/main.cpp
    src/asrd/connection.cpp
    src/asrd/ws_server.cpp
    src/asrd/session_server.cpp
    src/asrd/stability.cpp
    src/asrd/vad.cpp
    src/asrd/backend_openai.cpp
    src/asrd/backend_nemotron.cpp
    src/asrd/power.cpp
    src/asrd/http.cpp
)

set(KD_ASRD_HEADERS
    src/asrd/connection.h
    src/asrd/ws_server.h
    src/asrd/session_server.h
    src/asrd/stability.h
    src/asrd/vad.h
    src/asrd/backend.h
    src/asrd/backend_openai.h
    src/asrd/backend_nemotron.h
    src/asrd/power.h
    src/asrd/http.h
)

# The .rc carries a version block and nothing else — no icon, no manifest. A headless child with
# no version block shows up as a bare "kvit-asrd.exe" in the installer's in-use-files dialog and
# in Task Manager, which for a process the user did not start is the wrong impression to give.
add_executable(kvit-asrd ${KD_ASRD_SOURCES} ${KD_ASRD_HEADERS} packaging/windows/kvit-asrd.rc)

# rc.exe finds kvit-asrd.rc's includes here, the same way the client's target does.
target_include_directories(kvit-asrd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows")

# **No /MANIFEST:NO here**, unlike kvit-dictate. That flag exists on the client because its .rc
# embeds packaging/windows/kvit-dictate.manifest by hand, and a second linker-generated one would
# give the executable two RT_MANIFEST resources and make Windows refuse to start it. This .rc
# embeds no manifest, so the linker's default is the only one — and that default is exactly what
# openai-translator.md §4.8 asks for: asInvoker, and nothing else. Suppressing it would leave the
# translator with no declared execution level at all.

#   bcrypt   ws_server — BCryptCreateHash(BCRYPT_SHA1_ALGORITHM) for Sec-WebSocket-Accept.
#            Already a project dependency (BCryptGenRandom in ipc_client), so this adds no new
#            DLL; crypt32 is deliberately NOT added for one base64 call, which is twenty lines.
#   winhttp  backend_openai/http — the POST to /v1/audio/transcriptions and /v1/audio/vad.
#   ws2_32   ws_server — the listening socket. WinHTTP is a client API and has no server-side
#            counterpart short of http.sys, which wants a namespace reservation and elevation.
# **onnxruntime-genai is not linked.** backend_nemotron.cpp opens it with LoadLibraryExW from
# `nemotron\` beside the executable, so a translator without that folder starts, serves every
# other backend, and says in words what is missing when Nemotron is asked for.
target_link_libraries(kvit-asrd PRIVATE kd_wire bcrypt winhttp ws2_32)

# Same version numbers as the client, from project(), so the two binaries in one installer can
# never disagree about which build they are.
target_compile_definitions(kvit-asrd PRIVATE
    KVIT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KVIT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KVIT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    KVIT_VERSION_RAW=${PROJECT_VERSION}
)

# ─── tools/ ──────────────────────────────────────────────────────────────────────────────────

if(KD_BUILD_TOOLS)
    # Console subsystem on purpose: the Phase 0B spike's entire output is measurements, and a
    # spike whose numbers are trapped behind a GUI is a spike nobody reads.
    add_executable(kd_probe tools/probe/main.cpp)
    target_link_libraries(kd_probe PRIVATE kd_core)

    # A GUI app: it hosts edit / rich edit / multiline / password / RTL controls to be typed into.
    # RichEdit needs no import library — Msftedit.dll is loaded at run time.
    add_executable(kd_testtarget WIN32 tools/testtarget/main.cpp)
    target_link_libraries(kd_testtarget PRIVATE kd_core)
endif()

# ─── tests ───────────────────────────────────────────────────────────────────────────────────

if(KD_BUILD_TESTS)
    set(KD_TEST_SOURCES
        tests/test_main.cpp
        tests/test_harness.cpp
        tests/test_utf8.cpp
        tests/test_json.cpp
        tests/test_framing.cpp
        tests/test_protocol.cpp
        tests/test_commit_engine.cpp
        tests/test_session.cpp
        tests/test_audio.cpp
        tests/test_ipc.cpp
        tests/test_hotkey.cpp
        tests/test_anchor.cpp
        tests/test_overlay.cpp
        tests/test_tray_icon.cpp
        tests/test_settings.cpp
        tests/test_corpus.cpp
        tests/test_edit_buffer.cpp
        tests/test_normalizer.cpp
        tests/test_injector.cpp
        tests/test_translator.cpp
        tests/test_updater.cpp
        tests/test_fixtures.cpp

        # kvit-asrd. openai-translator.md §4.8 and §5: the translator's code is compiled INTO
        # the test binary — the same arrangement the overlay tests use — rather than driven only
        # through CreateProcess. A translator that can only be tested by launching it is one
        # whose WebSocket layer, stability tracker and window driver are tested by observation
        # instead of by assertion.
        tests/test_asrd_ws.cpp
        tests/test_asrd_fixtures.cpp
        tests/test_asrd_stability.cpp
        tests/test_asrd_vad.cpp
        tests/test_asrd_backend.cpp
        tests/test_asrd_session.cpp
        tests/test_asrd_nemotron.cpp
    )

    # Everything under src/asrd/ except main.cpp, which owns the process and its own int main().
    set(KD_ASRD_TEST_SOURCES ${KD_ASRD_SOURCES})
    list(REMOVE_ITEM KD_ASRD_TEST_SOURCES src/asrd/main.cpp)

    add_executable(kd_tests ${KD_TEST_SOURCES} ${KD_ASRD_TEST_SOURCES} tests/kd_test.h)
    target_link_libraries(kd_tests PRIVATE kd_core bcrypt winhttp ws2_32)
    target_include_directories(kd_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")

    # Absolute path, so the binary runs identically from ctest, from the debugger, and from a
    # shell in any directory. The fixtures are shared with the Python suite and neither side may
    # keep a private copy (docs/protocol.md §12).
    target_compile_definitions(kd_tests PRIVATE
        KD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

endif()

# ─── ctest registration ──────────────────────────────────────────────────────────────────────

enable_testing()

if(KD_BUILD_TESTS)
    # The whole suite, as one test. This is the one that gates a build.
    add_test(NAME kd_tests.all COMMAND kd_tests)

    # Per-suite entries, so a failure names the area rather than "the tests failed", and so
    # `ctest -R utf8` works.
    #
    # Suites with no tests yet are registered DISABLED rather than omitted. Two reasons: ctest
    # prints them, so the gap between "planned" and "covered" is visible in every run instead of
    # living only in a document; and a disabled entry does not fail the build, whereas an enabled
    # entry over an empty suite would (kd_tests exits non-zero when a filter matches nothing —
    # see tests/test_main.cpp, which refuses to report success for an empty run).
    #
    # When you write the first test in a suite, move its name from PENDING to READY. That is the
    # entire ceremony.
    set(KD_TEST_SUITES_READY
        harness
        utf8            # src/util/utf8.cpp
        json            # src/util/json.cpp
        framing         # src/protocol.cpp — FrameReader
        protocol        # src/protocol.cpp — message codec
        commit_engine   # src/commit_engine.cpp
        session         # src/session.cpp
        hotkey          # src/hotkey.cpp — InputFilter only
        injector        # src/injector.cpp — event synthesis and clipboard ownership
        fixtures        # tests/fixtures/protocol/ — the corpus the Python suite also runs
        audio           # src/audio_capture.cpp — AudioConverter, incl. the anti-alias response
        ipc             # src/ipc_client.cpp — pipe naming and the DACL
        anchor          # src/anchor.cpp — diff scorer and DPI maths on synthetic bitmaps
        overlay         # src/overlay.cpp — overlay_mode_for, plan.md §1.5's degradation rule
        tray_icon       # src/tray_icon.cpp — the drawn notification icon: fill means mic-open
        corpus          # src/corpus.cpp - the prune rule, which deletes the user's data
        edit_buffer     # src/edit_buffer.cpp
        settings        # src/settings.cpp — the round trip, and docs/editing.md §12's block
        normalizer      # src/normalizer.cpp — the term guard and the request/response codecs
        translator      # src/translator.cpp — the client's half of the spawn contract
        updater         # src/updater.cpp — version order, the release pick, the checksum line
        asrd_ws         # src/asrd/ws_server.cpp — the handshake and RFC 6455 framing
        asrd_fixtures   # tests/fixtures/protocol/ — the same corpus, from the server's role
        asrd_stability  # src/asrd/stability.cpp — the producer of stable_bytes
        asrd_vad        # src/asrd/vad.cpp — the energy endpointer and its chunking invariance
        asrd_backend    # src/asrd/backend_openai.cpp — the window driver, WAV, multipart
        asrd_session    # src/asrd/session_server.cpp — in-process loopback and a stub service
        asrd_nemotron   # src/asrd/backend_nemotron.cpp, src/model_download.cpp — the model, when given
    )
    # Empty: every suite now has tests. Keep the list rather than deleting it — a new module
    # starts here, disabled and visible, instead of being silently uncovered.
    set(KD_TEST_SUITES_PENDING
    )

    foreach(suite IN LISTS KD_TEST_SUITES_READY)
        add_test(NAME "kd_tests.${suite}" COMMAND kd_tests --suite ${suite})
    endforeach()

    foreach(suite IN LISTS KD_TEST_SUITES_PENDING)
        add_test(NAME "kd_tests.${suite}" COMMAND kd_tests --suite ${suite})
        set_tests_properties("kd_tests.${suite}" PROPERTIES DISABLED TRUE)
    endforeach()
endif()

# ─── Python test suite ───────────────────────────────────────────────────────────────────────
#
# The C++ build must never depend on Python being installed, on uv being on PATH, or on any
# environment under server/ existing — plan.md §5 creates those lazily and per-backend, and a
# configure step that failed without them would make the C++ side unbuildable for reasons that
# have nothing to do with C++.
#
# So the Python tests are registered only when a marker file exists. Create it with:
#
#     type nul > server\.pytest-enabled
#
# after `uv sync` has produced an environment under server/. The marker is gitignored: whether
# your machine has a Python environment is a local fact, not a repository fact.

set(KD_PYTEST_MARKER "${CMAKE_CURRENT_SOURCE_DIR}/server/.pytest-enabled")

if(EXISTS "${KD_PYTEST_MARKER}")
    find_program(KD_UV_EXECUTABLE uv)
    if(KD_UV_EXECUTABLE)
        add_test(NAME python.protocol
                 COMMAND "${KD_UV_EXECUTABLE}" run pytest -q
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/server")
        # Both suites read tests/fixtures/protocol/. A disagreement between them is exactly what
        # these fixtures exist to surface (docs/protocol.md §12).
        set_tests_properties(python.protocol PROPERTIES
            ENVIRONMENT "KD_FIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    else()
        message(WARNING
            "server/.pytest-enabled exists but 'uv' was not found on PATH; "
            "the Python conformance tests will not run.")
    endif()
endif()

# ─── Configure-time summary ──────────────────────────────────────────────────────────────────

message(STATUS "kvit-dictate ${PROJECT_VERSION}")
message(STATUS "  generator        : ${CMAKE_GENERATOR}")
message(STATUS "  C++ standard     : ${CMAKE_CXX_STANDARD}")
message(STATUS "  warnings as errors: ${KD_WERROR}")
message(STATUS "  tests            : ${KD_BUILD_TESTS}")
message(STATUS "  tools            : ${KD_BUILD_TOOLS}")
if(EXISTS "${KD_PYTEST_MARKER}")
    message(STATUS "  python tests     : enabled by server/.pytest-enabled")
else()
    message(STATUS "  python tests     : off (create server/.pytest-enabled to enable)")
endif()
