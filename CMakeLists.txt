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
project(kvit-dictate VERSION 0.1.0 LANGUAGES CXX)

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

# Warnings-as-errors is OFF for now, deliberately.
#
# Reason, stated so this does not become permanent by inertia: every module is currently a stub
# whose .cpp defines none of the functions its header declares, so /W4 legitimately reports
# unreferenced parameters and unreachable code throughout. Turning warnings into errors today
# would mean sprinkling suppressions across code that is about to be replaced. Turn this ON at
# the end of Phase 3, when the vertical slice compiles for real — it is a one-line change and it
# is listed as such in README.md's status section.
option(KD_WERROR "Treat compiler warnings as errors (enable at the end of Phase 3)" OFF)

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

# The C runtime is left at CMake's default (dynamic, /MD and /MDd). Static linking is not
# required: this is not a redistributable yet, and /MT would have to be revisited when the
# binary is signed for the Defender work in plan.md §6 anyway.

# ─── kd_core: every module except main.cpp ───────────────────────────────────────────────────

set(KD_CORE_SOURCES
    # Pure logic — no <windows.h>, unit-tested without a UI (see the header of each).
    src/protocol.cpp
    src/session.cpp
    src/commit_engine.cpp
    src/util/utf8.cpp
    src/util/json.cpp
    src/util/log.cpp
    src/settings.cpp

    # Windows-facing.
    src/hotkey.cpp
    src/audio_capture.cpp
    src/ipc_client.cpp
    src/anchor.cpp
    src/injector.cpp
    src/overlay.cpp
)

set(KD_CORE_HEADERS
    src/protocol.h
    src/session.h
    src/commit_engine.h
    src/anchor.h
    src/audio_capture.h
    src/hotkey.h
    src/injector.h
    src/ipc_client.h
    src/overlay.h
    src/settings.h
    src/util/clock.h
    src/util/json.h
    src/util/log.h
    src/util/result.h
    src/util/utf8.h
)

add_library(kd_core STATIC ${KD_CORE_SOURCES} ${KD_CORE_HEADERS})

target_include_directories(kd_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kd_core PUBLIC kd_common_flags)

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
#   d2d1      overlay — Direct2D device context and the layered-window render target
#   dwrite    overlay — DirectWrite text layout and measurement
#   dwmapi    overlay/anchor — DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) for a window
#             rectangle that excludes the invisible resize border, which the naive GetWindowRect
#             includes and which would offset every capture
#   shcore    overlay/anchor — GetDpiForMonitor for the mixed-DPI cases plan.md §7 gates on.
#             NOTE: PROCESS_PER_MONITOR_DPI_AWARE_V2 is *not* shcore's SetProcessDpiAwareness
#             (that API's enum only reaches v1); v2 comes from the application manifest or from
#             user32's SetProcessDpiAwarenessContext. The manifest is preferred because it
#             applies before any window exists — see the TODO under kvit-dictate below.
#   shell32   main — Shell_NotifyIconW (tray icon), SHGetKnownFolderPath (%APPDATA% settings path)
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
    shcore
    shell32
    comctl32
)

# ─── kvit-dictate.exe ────────────────────────────────────────────────────────────────────────

add_executable(kvit-dictate WIN32 src/main.cpp)
target_link_libraries(kvit-dictate PRIVATE kd_core)

# TODO(phase-3): add an application manifest declaring
#   <dpiAwareness>PerMonitorV2</dpiAwareness> and <dpiAware>true/pm</dpiAware>,
#   plus requestedExecutionLevel asInvoker (never requireAdministrator — plan.md §1.7 makes
#   elevated targets an explicit unsupported state rather than something to elevate past).
# The manifest is preferred over SetProcessDpiAwarenessContext because it applies before any
# window is created, and plan.md §1.3 requires per-monitor v2 for captured physical pixels.

# models.json sits beside the executable (see the schema block in src/settings.h). Copied rather
# than embedded so a user can point an entry at a different interpreter without a rebuild —
# which is the whole reason plan.md §1.1 puts the model→interpreter map in a data file.
add_custom_command(TARGET kvit-dictate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/models.json"
            "$<TARGET_FILE_DIR:kvit-dictate>/models.json"
    COMMENT "Copying models.json next to kvit-dictate.exe"
    VERBATIM
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
        tests/test_injector.cpp
        tests/test_fixtures.cpp
    )

    add_executable(kd_tests ${KD_TEST_SOURCES} tests/kd_test.h)
    target_link_libraries(kd_tests PRIVATE kd_core)
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
message(STATUS "  warnings as errors: ${KD_WERROR} (turn on at the end of Phase 3)")
message(STATUS "  tests            : ${KD_BUILD_TESTS}")
message(STATUS "  tools            : ${KD_BUILD_TOOLS}")
if(EXISTS "${KD_PYTEST_MARKER}")
    message(STATUS "  python tests     : enabled by server/.pytest-enabled")
else()
    message(STATUS "  python tests     : off (create server/.pytest-enabled to enable)")
endif()
